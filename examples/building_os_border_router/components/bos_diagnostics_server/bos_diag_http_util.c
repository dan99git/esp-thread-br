/**
 * Building OS Border Router: local diagnostics web surface.
 * Split part: shared send/JSON/body helpers (see bos_diag_internal.h).
 */

#include "bos_diagnostics_server.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bos_commissioning.h"
#include "bos_convergence_aggregator.h"
#include "bos_floating_book.h"
#include "bos_joiner.h"
#include "bos_thread_dataset_anchor.h"
#include "bos_thread_diag.h"
#include "bos_br_ota.h"
#include "bos_ledger_ingress.h"
#include "bos_ledger_mesh_serve.h"
#include "bos_server_registration.h"
#include "cJSON.h"
#include "esp_br_web.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_rcp_update.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "openthread/coap.h"
#include "openthread/dns.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/platform/radio.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

#include "bos_diag_internal.h"

const char *ledger_state_str(bos_ledger_state_t state)
{
    switch (state) {
    case BOS_LEDGER_STATE_NONE:
        return "none";
    case BOS_LEDGER_STATE_RECEIVING:
        return "receiving";
    case BOS_LEDGER_STATE_VALIDATING:
        return "validating";
    case BOS_LEDGER_STATE_COMMITTING:
        return "committing";
    case BOS_LEDGER_STATE_ACTIVE:
        return "active";
    default:
        return "unknown";
    }
}

void digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < BOS_LEDGER_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[BOS_LEDGER_DIGEST_LEN * 2] = '\0';
}

/* CONFIG_NEWLIB_NANO_FORMAT has no 64-bit printf support; %llu misaligns the
 * variadic args (LoadProhibited panic, first hit when the active-ledger
 * handler rendered committed_at). Format u64 manually instead. */
void u64_to_dec(uint64_t value, char out[21])
{
    char tmp[21];
    size_t i = 0;
    do {
        tmp[i++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0 && i < sizeof(tmp) - 1);
    size_t n = 0;
    while (i > 0) {
        out[n++] = tmp[--i];
    }
    out[n] = '\0';
}

int json_string_or_null(char *out, size_t out_len, const char *value)
{
    if (!value || value[0] == '\0') {
        return snprintf(out, out_len, "null");
    }

    size_t pos = 0;
    if (out_len == 0) {
        return -1;
    }
    out[pos++] = '"';
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        const char *escape = NULL;
        char unicode_escape[7];
        switch (*p) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*p < 0x20) {
                snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *p);
                escape = unicode_escape;
            }
            break;
        }

        if (escape) {
            size_t len = strlen(escape);
            if (pos + len >= out_len) {
                return -1;
            }
            memcpy(out + pos, escape, len);
            pos += len;
        } else {
            if (pos + 1 >= out_len) {
                return -1;
            }
            out[pos++] = (char)*p;
        }
    }
    if (pos + 1 >= out_len) {
        return -1;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return (int)pos;
}

void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}

/* Error paths return ESP_OK after the error response is sent: a non-ESP_OK
 * handler return makes esp_http_server close the socket immediately, which
 * can reset the connection before the response body is delivered. */
esp_err_t send_generated_json(httpd_req_t *req,
                              size_t buffer_len,
                              int (*renderer)(char *out, size_t out_len),
                              const char *error_message)
{
    char *json = (char *)malloc(buffer_len);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    int written = renderer(json, buffer_len);
    if (written < 0) {
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, error_message);
        return ESP_OK;
    }

    send_json(req, json);
    free(json);
    return ESP_OK;
}

/* Honest-unavailable JSON error for /bos/thread-diag with a real HTTP status
 * (esp_http_server has no 503 in httpd_err_code_t, so the status line is set
 * directly, same as bos_joiner.c send_error_json). Returns ESP_OK after the
 * error response is sent (socket-RST rule, see send_generated_json). */
esp_err_t thread_diag_send_unavailable(httpd_req_t *req, const char *error)
{
    char json[96];
    int written = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"internal error\"}");
    } else {
        httpd_resp_sendstr(req, json);
    }
    return ESP_OK;
}

/* Honest JSON error with an explicit status line (esp_http_server's
 * httpd_err_code_t has no 401/502/503). Returns ESP_OK after the error
 * response is sent (socket-RST rule, see send_generated_json). */
esp_err_t send_status_json(httpd_req_t *req, const char *status_line, const char *error)
{
    char json[160];
    int written = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"internal error\"}");
    } else {
        httpd_resp_sendstr(req, json);
    }
    return ESP_OK;
}

esp_err_t send_unauthorized(httpd_req_t *req)
{
    return send_status_json(req, "401 Unauthorized", "device token or api key required");
}

/* Serialize a cJSON tree and send it. Returns ESP_OK always (RST rule). */
esp_err_t send_cjson(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    send_json(req, text);
    cJSON_free(text);
    return ESP_OK;
}

/* Read the request body into buf (NUL-terminated). Returns body length,
 * or -1 on socket error / body larger than buf. */
int read_request_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len >= buf_len) {
        return -1;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return -1;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return (int)received;
}

const char *http_status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Status";
    }
}
