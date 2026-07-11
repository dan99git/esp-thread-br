/**
 * Building OS Border Router: app firmware OTA over the LAN-side BR HTTP server.
 *
 * HTTP surface: authorization, JSON responses, and the five OTA handlers.
 * The stream engine and lifecycle entry points live in bos_br_ota_stream.c.
 */

#include "bos_br_ota.h"
#include "bos_br_ota_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_server_registration.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static int json_string_or_null(char *out, size_t out_len, const char *value)
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

static esp_err_t header_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return ESP_ERR_NOT_FOUND;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_len);
}

static bool request_authorized(httpd_req_t *req)
{
    char expected[BOS_BR_OTA_HEADER_VALUE_MAX];
    char provided[BOS_BR_OTA_HEADER_VALUE_MAX];

    if (bos_server_registration_get_token(expected, sizeof(expected)) == ESP_OK && expected[0] != '\0') {
        if (header_value(req, "X-Device-Token", provided, sizeof(provided)) == ESP_OK &&
            strcmp(expected, provided) == 0) {
            return true;
        }
    }

#ifdef CONFIG_BOS_SERVER_API_KEY
    if (CONFIG_BOS_SERVER_API_KEY[0] != '\0') {
        if (header_value(req, "x-api-key", provided, sizeof(provided)) == ESP_OK &&
            strcmp(CONFIG_BOS_SERVER_API_KEY, provided) == 0) {
            return true;
        }
    }
#endif

    return false;
}

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}

static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *error)
{
    char error_json[192];
    char error_value[160];

    if (json_string_or_null(error_value, sizeof(error_value), error) < 0) {
        snprintf(error_value, sizeof(error_value), "\"error\"");
    }
    snprintf(error_json, sizeof(error_json), "{\"ok\":false,\"error\":%s}", error_value);

    httpd_resp_set_status(req, status);
    send_json(req, error_json);
    return ESP_FAIL;
}

static bool begin_operation(httpd_req_t *req)
{
    if (bos_br_ota_operation_lock == NULL) {
        if (bos_br_ota_init() != ESP_OK) {
            send_error_json(req, "500 Internal Server Error", "OTA lock unavailable");
            return false;
        }
    }
    if (xSemaphoreTake(bos_br_ota_operation_lock, 0) != pdTRUE) {
        send_error_json(req, "409 Conflict", "OTA operation already in progress");
        return false;
    }
    if (bos_br_ota_status.reboot_pending ||
        bos_br_ota_status.state == BOS_BR_OTA_STATE_READY_TO_REBOOT ||
        bos_br_ota_status.state == BOS_BR_OTA_STATE_REBOOTING ||
        bos_br_ota_status.state == BOS_BR_OTA_STATE_ROLLING_BACK) {
        xSemaphoreGive(bos_br_ota_operation_lock);
        send_error_json(req, "409 Conflict", "OTA reboot already pending");
        return false;
    }
    return true;
}

static void end_operation(void)
{
    if (bos_br_ota_operation_lock != NULL) {
        xSemaphoreGive(bos_br_ota_operation_lock);
    }
}

static esp_err_t read_json_body(httpd_req_t *req, char **out)
{
    if (!req || !out || req->content_len == 0 || req->content_len > BOS_BR_OTA_FETCH_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = (char *)malloc(req->content_len + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_ERR_INVALID_RESPONSE;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';
    *out = body;
    return ESP_OK;
}

static esp_err_t read_sha_header(httpd_req_t *req,
                                 uint8_t digest[BOS_BR_OTA_DIGEST_LEN],
                                 char digest_hex[BOS_BR_OTA_SHA256_HEX_LEN + 1])
{
    char value[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    if (header_value(req, "X-BOS-OTA-SHA256", value, sizeof(value)) != ESP_OK &&
        header_value(req, "x-bos-ota-sha256", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!parse_sha256_hex(value, digest)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(digest_hex, BOS_BR_OTA_SHA256_HEX_LEN + 1, "%s", value);
    return ESP_OK;
}

int bos_br_ota_status_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    bool has_ota_state = running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK;
    char running_version[32] = "";
    char running_version_json[80];
    char target_json[64];
    char source_json[48];
    char expected_sha_json[96];
    char actual_sha_json[96];
    char new_version_json[80];
    char last_error_json[224];
    char running_label_json[64];
    char boot_label_json[64];
    uint32_t progress_percent = 0;

    bos_br_ota_app_version(running_version, sizeof(running_version));
    if (bos_br_ota_status.expected_size > 0) {
        progress_percent = (bos_br_ota_status.received * 100U) / bos_br_ota_status.expected_size;
        if (progress_percent > 100U) {
            progress_percent = 100U;
        }
    }

    if (json_string_or_null(running_version_json, sizeof(running_version_json), running_version) < 0 ||
        json_string_or_null(target_json, sizeof(target_json), bos_br_ota_status.target_partition) < 0 ||
        json_string_or_null(source_json, sizeof(source_json), bos_br_ota_status.source) < 0 ||
        json_string_or_null(expected_sha_json, sizeof(expected_sha_json), bos_br_ota_status.expected_sha256) < 0 ||
        json_string_or_null(actual_sha_json, sizeof(actual_sha_json), bos_br_ota_status.actual_sha256) < 0 ||
        json_string_or_null(new_version_json, sizeof(new_version_json), bos_br_ota_status.new_version) < 0 ||
        json_string_or_null(last_error_json, sizeof(last_error_json), bos_br_ota_status.last_error) < 0 ||
        json_string_or_null(running_label_json, sizeof(running_label_json), running ? running->label : NULL) < 0 ||
        json_string_or_null(boot_label_json, sizeof(boot_label_json), boot ? boot->label : NULL) < 0) {
        return -1;
    }

    return snprintf(out,
                    out_len,
                    "{\"state\":\"%s\",\"busy\":%s,\"running_partition\":%s,"
                    "\"boot_partition\":%s,\"running_version\":%s,"
                    "\"running_ota_state\":\"%s\",\"pending_verify\":%s,"
                    "\"rollback_possible\":%s,\"reboot_pending\":%s,"
                    "\"source\":%s,\"target_partition\":%s,\"expected_size_bytes\":%u,"
                    "\"received_bytes\":%u,\"progress_percent\":%u,"
                    "\"new_version\":%s,\"expected_sha256\":%s,\"actual_sha256\":%s,"
                    "\"last_error\":%s}",
                    state_str(bos_br_ota_status.state),
                    bos_br_ota_status.state != BOS_BR_OTA_STATE_IDLE && bos_br_ota_status.state != BOS_BR_OTA_STATE_FAILED ? "true" : "false",
                    running_label_json,
                    boot_label_json,
                    running_version_json,
                    has_ota_state ? ota_img_state_str(ota_state) : "unknown",
                    has_ota_state && ota_state == ESP_OTA_IMG_PENDING_VERIFY ? "true" : "false",
                    esp_ota_check_rollback_is_possible() ? "true" : "false",
                    bos_br_ota_status.reboot_pending ? "true" : "false",
                    source_json,
                    target_json,
                    (unsigned)bos_br_ota_status.expected_size,
                    (unsigned)bos_br_ota_status.received,
                    (unsigned)progress_percent,
                    new_version_json,
                    expected_sha_json,
                    actual_sha_json,
                    last_error_json);
}

esp_err_t bos_br_ota_http_status(httpd_req_t *req)
{
    char json[1536];
    int written = bos_br_ota_status_json(json, sizeof(json));
    if (written < 0 || written >= (int)sizeof(json)) {
        return send_error_json(req, "500 Internal Server Error", "OTA status too large");
    }
    send_json(req, json);
    return ESP_OK;
}

esp_err_t bos_br_ota_http_upload(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    uint8_t expected_digest[BOS_BR_OTA_DIGEST_LEN];
    char expected_digest_hex[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    esp_err_t err = read_sha_header(req, expected_digest, expected_digest_hex);
    if (err != ESP_OK) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "missing or invalid X-BOS-OTA-SHA256");
    }
    if (req->content_len == 0 || req->content_len > UINT32_MAX) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA image size");
    }

    bos_br_ota_upload_ctx_t ctx = {
        .req = req,
        .remaining = req->content_len,
    };

    err = perform_ota_stream("upload",
                             (uint32_t)req->content_len,
                             expected_digest,
                             expected_digest_hex,
                             upload_reader,
                             &ctx);
    if (err != ESP_OK) {
        char error[160];
        snprintf(error, sizeof(error), "%s", bos_br_ota_status.last_error[0] ? bos_br_ota_status.last_error : esp_err_to_name(err));
        end_operation();
        return send_error_json(req, "400 Bad Request", error);
    }

    char json[384];
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"mode\":\"upload\",\"size_bytes\":%u,"
             "\"version\":\"%s\",\"target_partition\":\"%s\",\"rebooting\":true}",
             (unsigned)bos_br_ota_status.expected_size,
             bos_br_ota_status.new_version,
             bos_br_ota_status.target_partition);
    send_json(req, json);
    end_operation();
    schedule_restart();
    return ESP_OK;
}

esp_err_t bos_br_ota_http_fetch(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    char *body = NULL;
    esp_err_t err = read_json_body(req, &body);
    if (err != ESP_OK) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA fetch JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA fetch JSON");
    }

    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *sha_item = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsString(url_item) || !url_item->valuestring ||
        strncmp(url_item->valuestring, "http://", 7) != 0 ||
        strlen(url_item->valuestring) >= BOS_BR_OTA_URL_MAX ||
        !cJSON_IsString(sha_item) || !sha_item->valuestring ||
        !cJSON_IsNumber(size_item) || size_item->valuedouble <= 0 ||
        size_item->valuedouble > (double)UINT32_MAX) {
        cJSON_Delete(root);
        end_operation();
        return send_error_json(req, "400 Bad Request", "OTA fetch requires http url, sha256, and size");
    }

    char url[BOS_BR_OTA_URL_MAX];
    char expected_digest_hex[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    uint8_t expected_digest[BOS_BR_OTA_DIGEST_LEN];
    uint32_t image_size = (uint32_t)size_item->valuedouble;
    snprintf(url, sizeof(url), "%s", url_item->valuestring);
    snprintf(expected_digest_hex, sizeof(expected_digest_hex), "%s", sha_item->valuestring);
    cJSON_Delete(root);

    if (!parse_sha256_hex(expected_digest_hex, expected_digest)) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA SHA-256");
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = BOS_BR_OTA_CHUNK_BYTES,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        end_operation();
        return send_error_json(req, "500 Internal Server Error", "failed to initialize OTA fetch client");
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        end_operation();
        return send_error_json(req, "400 Bad Request", "failed to open OTA fetch URL");
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300 ||
        (content_length >= 0 && (uint64_t)content_length != (uint64_t)image_size)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        end_operation();
        return send_error_json(req, "400 Bad Request", "OTA fetch URL returned invalid response");
    }

    bos_br_ota_fetch_ctx_t ctx = {
        .client = client,
        .remaining = image_size,
    };
    err = perform_ota_stream("fetch", image_size, expected_digest, expected_digest_hex, fetch_reader, &ctx);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        char error[160];
        snprintf(error, sizeof(error), "%s", bos_br_ota_status.last_error[0] ? bos_br_ota_status.last_error : esp_err_to_name(err));
        end_operation();
        return send_error_json(req, "400 Bad Request", error);
    }

    char json[384];
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"mode\":\"fetch\",\"size_bytes\":%u,"
             "\"version\":\"%s\",\"target_partition\":\"%s\",\"rebooting\":true}",
             (unsigned)bos_br_ota_status.expected_size,
             bos_br_ota_status.new_version,
             bos_br_ota_status.target_partition);
    send_json(req, json);
    end_operation();
    schedule_restart();
    return ESP_OK;
}

esp_err_t bos_br_ota_http_confirm(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    bos_br_ota_status.state = BOS_BR_OTA_STATE_CONFIRMING;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_mark_app_valid_cancel_rollback failed", err);
        end_operation();
        return send_error_json(req, "500 Internal Server Error", bos_br_ota_status.last_error);
    }

    bos_br_ota_status.state = BOS_BR_OTA_STATE_IDLE;
    bos_br_ota_status.reboot_pending = false;
    bos_br_ota_status.last_error[0] = '\0';
    send_json(req, "{\"ok\":true,\"confirmed\":true}");
    end_operation();
    return ESP_OK;
}

esp_err_t bos_br_ota_http_rollback(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    if (!esp_ota_check_rollback_is_possible()) {
        set_last_error("rollback app partition unavailable");
        end_operation();
        return send_error_json(req, "409 Conflict", bos_br_ota_status.last_error);
    }

    bos_br_ota_status.state = BOS_BR_OTA_STATE_ROLLING_BACK;
    bos_br_ota_status.reboot_pending = true;
    send_json(req, "{\"ok\":true,\"rollback\":true,\"rebooting\":true}");
    end_operation();
    schedule_rollback();
    return ESP_OK;
}
