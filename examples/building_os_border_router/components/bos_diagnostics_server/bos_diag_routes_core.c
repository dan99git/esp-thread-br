/**
 * Building OS Border Router: local diagnostics web surface.
 * Split part: ledger/convergence/status routes + backbone event handlers
 * (see bos_diag_internal.h).
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

static const char *TAG = "bos_diag";

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

esp_err_t ledger_active_get_handler(httpd_req_t *req)
{
    bos_ledger_active_t active;
    esp_err_t err = bos_ledger_ingress_get_active(&active);
    char last_error_json[256];
    char json[640];

    if (bos_ledger_ingress_last_error_json(last_error_json, sizeof(last_error_json)) < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ledger error snapshot too large");
        return ESP_OK;
    }

    if (err == ESP_OK && active.present) {
        char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
        char committed_str[21];
        digest_hex(active.digest, digest);
        u64_to_dec(active.committed_at, committed_str);
        snprintf(json,
                 sizeof(json),
                 "{\"present\":true,\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                 "\"committed_at_ms\":%s,\"state\":\"%s\",\"persist\":\"%s\",\"size_bytes\":%u,"
                 "\"chunk_size\":%u,\"chunk_count\":%u,\"last_error\":%s}",
                 (unsigned)active.version,
                 digest,
                 committed_str,
                 ledger_state_str(bos_ledger_ingress_state()),
                 bos_ledger_ingress_persist_state_str(),
                 (unsigned)active.size_bytes,
                 (unsigned)active.chunk_size,
                 (unsigned)active.chunk_count,
                 last_error_json);
    } else {
        snprintf(json,
                 sizeof(json),
                 "{\"present\":false,\"ledger_version\":null,\"manifest_digest\":null,"
                 "\"committed_at_ms\":null,\"state\":\"%s\",\"persist\":\"%s\",\"last_error\":%s}",
                 ledger_state_str(bos_ledger_ingress_state()),
                 bos_ledger_ingress_persist_state_str(),
                 last_error_json);
    }

    send_json(req, json);
    return ESP_OK;
}

esp_err_t ledger_push_handler(httpd_req_t *req)
{
    /* Snapshot the active ledger before the push so we can tell whether this
     * deploy actually installed a new version/digest. */
    bos_ledger_active_t before;
    bool had_before = (bos_ledger_ingress_get_active(&before) == ESP_OK && before.present);

    esp_err_t ret = bos_ledger_ingress_http_push(req);

    /* On a genuine new ledger, refresh the SRP advertisement and nudge the live
     * mesh so nodes pull now instead of on their next poll (symmetric with the
     * phonebook announce). PUSH/pull authority is unchanged: the nudge only
     * wakes the node's existing version-compare + pull. */
    bos_ledger_active_t after;
    if (bos_ledger_ingress_get_active(&after) == ESP_OK && after.present) {
        bool changed = !had_before ||
                       after.version != before.version ||
                       memcmp(after.digest, before.digest, sizeof(after.digest)) != 0;
        if (changed) {
            esp_err_t announced = bos_ledger_mesh_serve_announce_ledger(after.version, after.digest);
            if (announced != ESP_OK) {
                ESP_LOGW(TAG, "ledger deploy announce failed: %s", esp_err_to_name(announced));
            }
        }
    }
    return ret;
}

esp_err_t ledger_torrent_get_handler(httpd_req_t *req)
{
    return send_generated_json(req,
                               12288,
                               bos_convergence_aggregator_torrent_json,
                               "failed to render ledger torrent snapshot");
}

esp_err_t convergence_get_handler(httpd_req_t *req)
{
    return send_generated_json(req,
                               12288,
                               bos_convergence_aggregator_snapshot_json,
                               "failed to render convergence snapshot");
}

esp_err_t peers_get_handler(httpd_req_t *req)
{
    return send_generated_json(req,
                               12288,
                               bos_convergence_aggregator_peers_json,
                               "failed to render peer snapshot");
}

esp_err_t status_get_handler(httpd_req_t *req)
{
    typedef struct {
        char device_id_json[390];
        char site_server_url_json[970];
        char firmware_version_json[96];
        char ledger_error_json[256];
        char ota_json[1536];
        char identity_json[1024];
        char thread_json[224];
        char json[3840];
    } status_json_buffers_t;

    status_json_buffers_t *buffers = (status_json_buffers_t *)calloc(1, sizeof(status_json_buffers_t));
    if (buffers == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
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
        bos_ledger_ingress_last_error_json(buffers->ledger_error_json, sizeof(buffers->ledger_error_json)) < 0 ||
        bos_br_ota_status_json(buffers->ota_json, sizeof(buffers->ota_json)) < 0 ||
        bos_commissioning_identity_json(buffers->identity_json, sizeof(buffers->identity_json)) < 0) {
        free(buffers);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status metadata too large");
        return ESP_OK;
    }

    /* Live Thread runtime: role, network name, PANID, channel, RLOC16.
     * Never the network key. Renders as null when OpenThread is not up yet
     * (honest-unavailable, same convention as identity). */
    bos_thread_runtime_status_t thread_status;
    if (bos_thread_runtime_status(&thread_status) == ESP_OK) {
        /* 16-char name, worst case fully escaped (\uXXXX per byte) plus quotes. */
        char network_name_json[104];
        int thread_written = -1;
        if (json_string_or_null(network_name_json, sizeof(network_name_json), thread_status.network_name) >= 0) {
            thread_written = snprintf(buffers->thread_json,
                                      sizeof(buffers->thread_json),
                                      "{\"role\":\"%s\",\"network_name\":%s,\"panid\":\"0x%04x\","
                                      "\"channel\":%u,\"rloc16\":\"0x%04x\"}",
                                      thread_status.role,
                                      network_name_json,
                                      (unsigned)thread_status.panid,
                                      (unsigned)thread_status.channel,
                                      (unsigned)thread_status.rloc16);
        }
        if (thread_written < 0 || thread_written >= (int)sizeof(buffers->thread_json)) {
            free(buffers);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "thread status too large");
            return ESP_OK;
        }
    } else {
        snprintf(buffers->thread_json, sizeof(buffers->thread_json), "null");
    }
    /* Heartbeat buffer counters (docs/08.8 s12 degraded operation): entries
     * held in the registration RAM ring while the server is unreachable plus
     * total overflow drops since boot. */
    uint32_t hb_buffered = 0;
    uint32_t hb_dropped = 0;
    bos_server_registration_heartbeat_stats(&hb_buffered, &hb_dropped);

    int written = snprintf(buffers->json,
                           sizeof(buffers->json),
                           "{\"device_class\":\"border_router\",\"firmware\":\"building-os-border-router\","
                           "\"firmware_version\":%s,\"device_id\":%s,\"site_server_url\":%s,"
                           "\"identity\":%s,"
                           "\"uptime_ms\":%u,\"heap_free_bytes\":%u,\"registered\":%s,"
                           "\"heartbeat_buffer\":{\"buffered_count\":%u,\"dropped_count\":%u},"
                           "\"backbone\":{\"connected\":%s,\"link_up\":%s,\"interface\":\"%s\","
                           "\"ipv4\":\"%s\",\"ipv6\":\"%s\"},"
                           "\"thread\":%s,"
                           "\"rcp\":{\"target\":\"%s\",\"version\":\"not_polled\"},"
                           "\"ledger\":{\"state\":\"%s\",\"persist\":\"%s\",\"last_error\":%s},\"ota\":%s}",
                           buffers->firmware_version_json,
                           buffers->device_id_json,
                           buffers->site_server_url_json,
                           buffers->identity_json,
                           (unsigned)(esp_timer_get_time() / 1000ULL),
                           (unsigned)esp_get_free_heap_size(),
                           bos_server_registration_is_registered() ? "true" : "false",
                           (unsigned)hb_buffered,
                           (unsigned)hb_dropped,
                           (s_backbone_link_up && (s_backbone_has_ipv4 || s_backbone_has_ipv6)) ? "true" : "false",
                           s_backbone_link_up ? "true" : "false",
                           s_backbone_if,
                           s_backbone_ipv4,
                           s_backbone_ipv6,
                           buffers->thread_json,
#if CONFIG_ESP_BR_H2_TARGET
                           "esp32h2",
#elif CONFIG_ESP_BR_C6_TARGET
                           "esp32c6",
#else
                           "unknown",
#endif
                           ledger_state_str(bos_ledger_ingress_state()),
                           bos_ledger_ingress_persist_state_str(),
                           buffers->ledger_error_json,
                           buffers->ota_json);
    if (written < 0 || written >= (int)sizeof(buffers->json)) {
        free(buffers);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status snapshot too large");
        return ESP_OK;
    }

    send_json(req, buffers->json);
    free(buffers);
    return ESP_OK;
}

void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
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

void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
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
