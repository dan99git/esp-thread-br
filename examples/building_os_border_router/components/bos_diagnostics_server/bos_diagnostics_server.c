/**
 * Building OS Border Router: local diagnostics web surface.
 */

#include "bos_diagnostics_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_convergence_aggregator.h"
#include "bos_br_ota.h"
#include "bos_ledger_ingress.h"
#include "bos_server_registration.h"
#include "esp_br_web.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

static const char *TAG = "bos_diag";

static bool s_registered;
static bool s_backbone_link_up;
static bool s_backbone_has_ipv4;
static char s_backbone_ipv4[16] = "";
static bool s_backbone_has_ipv6;
static char s_backbone_ipv6[40] = "";
static char s_backbone_if[24] = "unknown";

static bool is_backbone_netif(esp_netif_t *netif)
{
    const char *desc = esp_netif_get_desc(netif);
    if (!desc) {
        return false;
    }

#if CONFIG_EXAMPLE_CONNECT_ETHERNET
    if (strcmp(desc, EXAMPLE_NETIF_DESC_ETH) == 0) {
        return true;
    }
#endif
#if CONFIG_EXAMPLE_CONNECT_WIFI
    if (strcmp(desc, EXAMPLE_NETIF_DESC_STA) == 0) {
        return true;
    }
#endif
    return false;
}

static const char *ledger_state_str(bos_ledger_state_t state)
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

static void digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < BOS_LEDGER_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[BOS_LEDGER_DIGEST_LEN * 2] = '\0';
}

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

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}

static esp_err_t send_generated_json(httpd_req_t *req,
                                     size_t buffer_len,
                                     int (*renderer)(char *out, size_t out_len),
                                     const char *error_message)
{
    char *json = (char *)malloc(buffer_len);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    int written = renderer(json, buffer_len);
    if (written < 0) {
        free(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, error_message);
        return ESP_FAIL;
    }

    send_json(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t ledger_active_get_handler(httpd_req_t *req)
{
    bos_ledger_active_t active;
    esp_err_t err = bos_ledger_ingress_get_active(&active);
    char json[256];

    if (err == ESP_OK && active.present) {
        char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
        digest_hex(active.digest, digest);
        snprintf(json,
                 sizeof(json),
                 "{\"present\":true,\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                 "\"committed_at_ms\":%llu,\"state\":\"%s\",\"size_bytes\":%u,"
                 "\"chunk_size\":%u,\"chunk_count\":%u}",
                 (unsigned)active.version,
                 digest,
                 (unsigned long long)active.committed_at,
                 ledger_state_str(bos_ledger_ingress_state()),
                 (unsigned)active.size_bytes,
                 (unsigned)active.chunk_size,
                 (unsigned)active.chunk_count);
    } else {
        snprintf(json,
                 sizeof(json),
                 "{\"present\":false,\"ledger_version\":null,\"manifest_digest\":null,"
                 "\"committed_at_ms\":null,\"state\":\"%s\"}",
                 ledger_state_str(bos_ledger_ingress_state()));
    }

    send_json(req, json);
    return ESP_OK;
}

static esp_err_t ledger_push_handler(httpd_req_t *req)
{
    return bos_ledger_ingress_http_push(req);
}

static esp_err_t ledger_torrent_get_handler(httpd_req_t *req)
{
    return send_generated_json(req,
                               12288,
                               bos_convergence_aggregator_torrent_json,
                               "failed to render ledger torrent snapshot");
}

static esp_err_t convergence_get_handler(httpd_req_t *req)
{
    return send_generated_json(req,
                               12288,
                               bos_convergence_aggregator_snapshot_json,
                               "failed to render convergence snapshot");
}

static esp_err_t peers_get_handler(httpd_req_t *req)
{
    return send_generated_json(req,
                               12288,
                               bos_convergence_aggregator_peers_json,
                               "failed to render peer snapshot");
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    typedef struct {
        char device_id_json[390];
        char site_server_url_json[970];
        char firmware_version_json[96];
        char ota_json[1536];
        char json[2304];
    } status_json_buffers_t;

    status_json_buffers_t *buffers = (status_json_buffers_t *)calloc(1, sizeof(status_json_buffers_t));
    if (buffers == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    char device_id[64] = "";
    char site_server_url[160] = "";
    char firmware_version[40] = "";
    if (bos_server_registration_get_device_id(device_id, sizeof(device_id)) != ESP_OK) {
        device_id[0] = '\0';
    }
    if (bos_server_registration_get_server_url(site_server_url, sizeof(site_server_url)) != ESP_OK) {
        site_server_url[0] = '\0';
    }
    if (bos_br_ota_app_version(firmware_version, sizeof(firmware_version)) != ESP_OK) {
        firmware_version[0] = '\0';
    }
    if (json_string_or_null(buffers->device_id_json, sizeof(buffers->device_id_json), device_id) < 0 ||
        json_string_or_null(buffers->site_server_url_json, sizeof(buffers->site_server_url_json), site_server_url) < 0 ||
        json_string_or_null(buffers->firmware_version_json, sizeof(buffers->firmware_version_json), firmware_version) < 0 ||
        bos_br_ota_status_json(buffers->ota_json, sizeof(buffers->ota_json)) < 0) {
        free(buffers);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status metadata too large");
        return ESP_FAIL;
    }
    int written = snprintf(buffers->json,
                           sizeof(buffers->json),
                           "{\"device_class\":\"border_router\",\"firmware\":\"building-os-border-router\","
                           "\"firmware_version\":%s,\"device_id\":%s,\"site_server_url\":%s,"
                           "\"uptime_ms\":%u,\"heap_free_bytes\":%u,\"registered\":%s,"
                           "\"backbone\":{\"connected\":%s,\"link_up\":%s,\"interface\":\"%s\","
                           "\"ipv4\":\"%s\",\"ipv6\":\"%s\"},"
                           "\"thread\":{\"role\":\"not_polled\",\"network_name\":\"\",\"rloc16\":\"0xffff\"},"
                           "\"rcp\":{\"target\":\"%s\",\"version\":\"not_polled\"},"
                           "\"ledger\":{\"state\":\"%s\"},\"ota\":%s}",
                           buffers->firmware_version_json,
                           buffers->device_id_json,
                           buffers->site_server_url_json,
                           (unsigned)(esp_timer_get_time() / 1000ULL),
                           (unsigned)esp_get_free_heap_size(),
                           bos_server_registration_is_registered() ? "true" : "false",
                           (s_backbone_link_up && (s_backbone_has_ipv4 || s_backbone_has_ipv6)) ? "true" : "false",
                           s_backbone_link_up ? "true" : "false",
                           s_backbone_if,
                           s_backbone_ipv4,
                           s_backbone_ipv6,
#if CONFIG_ESP_BR_H2_TARGET
                           "esp32h2",
#elif CONFIG_ESP_BR_C6_TARGET
                           "esp32c6",
#else
                           "unknown",
#endif
                           ledger_state_str(bos_ledger_ingress_state()),
                           buffers->ota_json);
    if (written < 0 || written >= (int)sizeof(buffers->json)) {
        free(buffers);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status snapshot too large");
        return ESP_FAIL;
    }

    send_json(req, buffers->json);
    free(buffers);
    return ESP_OK;
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != IP_EVENT) {
        return;
    }

    if (event_id == IP_EVENT_ETH_GOT_IP || event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_backbone_if, sizeof(s_backbone_if), "%s", esp_netif_get_desc(event->esp_netif));
        snprintf(s_backbone_ipv4, sizeof(s_backbone_ipv4), IPSTR, IP2STR(&event->ip_info.ip));
        s_backbone_has_ipv4 = true;
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        if (!is_backbone_netif(event->esp_netif)) {
            return;
        }
        snprintf(s_backbone_if, sizeof(s_backbone_if), "%s", esp_netif_get_desc(event->esp_netif));
        snprintf(s_backbone_ipv6,
                 sizeof(s_backbone_ipv6),
                 IPV6STR,
                 IPV62STR(event->ip6_info.ip));
        s_backbone_has_ipv6 = true;
    }
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == ETHERNET_EVENT_CONNECTED) {
        s_backbone_link_up = true;
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED || event_id == ETHERNET_EVENT_STOP) {
        s_backbone_link_up = false;
        s_backbone_has_ipv4 = false;
        s_backbone_has_ipv6 = false;
        s_backbone_ipv4[0] = '\0';
        s_backbone_ipv6[0] = '\0';
    }
}

esp_err_t bos_diagnostics_server_start(void)
{
    if (s_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bos_br_ota_init(), TAG, "failed to initialize BR OTA routes");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL),
                        TAG,
                        "failed to register IP event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL),
                        TAG,
                        "failed to register Ethernet event handler");

    httpd_uri_t status_uri = {
        .uri = "/bos/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ledger_uri = {
        .uri = "/bos/ledger/active",
        .method = HTTP_GET,
        .handler = ledger_active_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ledger_push_uri = {
        .uri = "/bos/ledger/push",
        .method = HTTP_POST,
        .handler = ledger_push_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ledger_torrent_uri = {
        .uri = "/bos/ledger/torrent",
        .method = HTTP_GET,
        .handler = ledger_torrent_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t convergence_uri = {
        .uri = "/bos/convergence",
        .method = HTTP_GET,
        .handler = convergence_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t peers_uri = {
        .uri = "/bos/peers",
        .method = HTTP_GET,
        .handler = peers_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_status_uri = {
        .uri = "/bos/ota/status",
        .method = HTTP_GET,
        .handler = bos_br_ota_http_status,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_upload_uri = {
        .uri = "/bos/ota/app",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_upload,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_fetch_uri = {
        .uri = "/bos/ota/app/fetch",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_fetch,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_confirm_uri = {
        .uri = "/bos/ota/confirm",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_confirm,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_rollback_uri = {
        .uri = "/bos/ota/rollback",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_rollback,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&status_uri), TAG, "failed to register /bos/status");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_uri), TAG, "failed to register /bos/ledger/active");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_push_uri), TAG, "failed to register /bos/ledger/push");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_torrent_uri), TAG, "failed to register /bos/ledger/torrent");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&convergence_uri), TAG, "failed to register /bos/convergence");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&peers_uri), TAG, "failed to register /bos/peers");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_status_uri), TAG, "failed to register /bos/ota/status");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_upload_uri), TAG, "failed to register /bos/ota/app");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_fetch_uri), TAG, "failed to register /bos/ota/app/fetch");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_confirm_uri), TAG, "failed to register /bos/ota/confirm");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_rollback_uri), TAG, "failed to register /bos/ota/rollback");

    s_registered = true;
    esp_err_t confirm_err = bos_br_ota_confirm_running_app();
    if (confirm_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "failed to confirm running OTA image after diagnostics route registration: %s",
                 esp_err_to_name(confirm_err));
    }
    ESP_LOGI(TAG, "Building OS diagnostics routes registered on BR port 80");
    return ESP_OK;
}
