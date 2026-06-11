/**
 * Building OS Border Router: local diagnostics web surface.
 */

#include "bos_diagnostics_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_commissioning.h"
#include "bos_convergence_aggregator.h"
#include "bos_joiner.h"
#include "bos_thread_dataset_anchor.h"
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
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "openthread/instance.h"
#include "openthread/link.h"
#include "openthread/thread.h"
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

/* CONFIG_NEWLIB_NANO_FORMAT has no 64-bit printf support; %llu misaligns the
 * variadic args (LoadProhibited panic, first hit when the active-ledger
 * handler rendered committed_at). Format u64 manually instead. */
static void u64_to_dec(uint64_t value, char out[21])
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

/* Error paths return ESP_OK after the error response is sent: a non-ESP_OK
 * handler return makes esp_http_server close the socket immediately, which
 * can reset the connection before the response body is delivered. */
static esp_err_t send_generated_json(httpd_req_t *req,
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

static esp_err_t ledger_active_get_handler(httpd_req_t *req)
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

/* Honest-unavailable JSON error for /bos/thread-diag with a real HTTP status
 * (esp_http_server has no 503 in httpd_err_code_t, so the status line is set
 * directly, same as bos_joiner.c send_error_json). Returns ESP_OK after the
 * error response is sent (socket-RST rule, see send_generated_json). */
static esp_err_t thread_diag_send_unavailable(httpd_req_t *req, const char *error)
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

/* GET /bos/thread-diag: Thread RX-path self-diagnostics.
 *
 * Built for the 2026-06-11 bench failure: a XIAO C6 joiner's legacy beacon
 * scan and MLE discovery both went unanswered while this BR was leader with
 * an active commissioner, and the BR had no self-diagnostics to show whether
 * scanner frames were even reaching the host MAC. Everything served here is
 * read from APIs that exist in this tree; nothing is fabricated:
 *   - otLinkGetCounters (openthread/link.h:888, otMacCounters link.h:87):
 *     MAC rx/tx totals plus the rx filtered/error counters. rx_beacon_request
 *     only increments when a scanner's Beacon Request makes it through RCP RX
 *     filtering to the host MAC, so a nearby scanning joiner that never moves
 *     rx_beacon_request/rx_total is direct evidence of wedged RCP RX
 *     filtering; tx_beacon shows whether the BR answered.
 *   - otThreadGetMleCounters (openthread/thread.h:920, otMleCounters
 *     thread.h:169): role transitions, attach attempts, partition changes.
 *   - otThreadGetDeviceRole / otThreadDeviceRoleToString
 *     (openthread/thread.h:747,756).
 *   - bos_joiner_commissioner_json: otCommissionerGetState
 *     (openthread/commissioner.h:459) plus the live joiner table via
 *     otCommissionerGetNextJoinerInfo (commissioner.h:266).
 *   - bos_joiner_event_trail_json: ring of the last 16 commissioner joiner
 *     callback events (bos_commissioning/bos_joiner.c).
 *   - uptime_ms (esp_timer) and heap_free_bytes (esp_get_free_heap_size).
 * Deliberately NOT served, because no public API exposes them in this tree:
 * RCP/spinel failure and reset counters. OpenThread keeps mRcpFailureCount
 * private to the C++ RadioSpinel class (openthread/src/lib/spinel/
 * radio_spinel.cpp:89) and esp-openthread exposes only callback registration
 * (esp_openthread_spinel.h:20,40); that single handler slot is already owned
 * by the upstream RCP recovery path (components/esp_rcp_update/src/
 * esp_ot_rcp_update.c:66..71), so re-registering here would replace upstream
 * recovery. Omitted rather than faked. A discovery-request-level counter
 * likewise does not exist in the OT public API; the MAC rx counters plus the
 * joiner event trail are the deliverable.
 * Auth: none; read-only diagnostics, same boundary as /bos/status. */
static esp_err_t thread_diag_get_handler(httpd_req_t *req)
{
    typedef struct {
        char commissioner_json[512];
        char events_json[1664];
        char json[4096];
    } thread_diag_buffers_t;

    thread_diag_buffers_t *buffers = (thread_diag_buffers_t *)calloc(1, sizeof(thread_diag_buffers_t));
    if (buffers == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }

    char uptime_str[21];
    u64_to_dec((uint64_t)esp_timer_get_time() / 1000ULL, uptime_str);

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        free(buffers);
        return thread_diag_send_unavailable(req, "openthread lock timeout");
    }

    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        free(buffers);
        return thread_diag_send_unavailable(req, "openthread not running");
    }

    const char *role = otThreadDeviceRoleToString(otThreadGetDeviceRole(instance));
    const otMacCounters *mac = otLinkGetCounters(instance);
    const otMleCounters *mle = otThreadGetMleCounters(instance);
    otMacCounters mac_copy = *mac;
    otMleCounters mle_copy = *mle;

    if (bos_joiner_commissioner_json(buffers->commissioner_json, sizeof(buffers->commissioner_json)) < 0 ||
        bos_joiner_event_trail_json(buffers->events_json, sizeof(buffers->events_json)) < 0) {
        esp_openthread_lock_release();
        free(buffers);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "commissioner snapshot too large");
        return ESP_OK;
    }

    esp_openthread_lock_release();

    /* otMleCounters time-in-role fields are uint64 milliseconds; rendered via
     * u64_to_dec (never %llu, nano printf rule above). */
    char disabled_time[21], detached_time[21], child_time[21];
    char router_time[21], leader_time[21], tracked_time[21];
    u64_to_dec(mle_copy.mDisabledTime, disabled_time);
    u64_to_dec(mle_copy.mDetachedTime, detached_time);
    u64_to_dec(mle_copy.mChildTime, child_time);
    u64_to_dec(mle_copy.mRouterTime, router_time);
    u64_to_dec(mle_copy.mLeaderTime, leader_time);
    u64_to_dec(mle_copy.mTrackedTime, tracked_time);

    int written = snprintf(buffers->json,
                           sizeof(buffers->json),
                           "{\"uptime_ms\":%s,\"heap_free_bytes\":%u,\"role\":\"%s\","
                           "\"mac_counters\":{"
                           "\"tx_total\":%u,\"tx_unicast\":%u,\"tx_broadcast\":%u,"
                           "\"tx_acked\":%u,\"tx_retry\":%u,"
                           "\"tx_beacon\":%u,\"tx_beacon_request\":%u,"
                           "\"tx_err_cca\":%u,\"tx_err_abort\":%u,\"tx_err_busy_channel\":%u,"
                           "\"rx_total\":%u,\"rx_unicast\":%u,\"rx_broadcast\":%u,"
                           "\"rx_data\":%u,\"rx_data_poll\":%u,"
                           "\"rx_beacon\":%u,\"rx_beacon_request\":%u,\"rx_other\":%u,"
                           "\"rx_address_filtered\":%u,\"rx_dest_addr_filtered\":%u,"
                           "\"rx_duplicated\":%u,\"rx_err_no_frame\":%u,"
                           "\"rx_err_unknown_neighbor\":%u,\"rx_err_invalid_src_addr\":%u,"
                           "\"rx_err_sec\":%u,\"rx_err_fcs\":%u,\"rx_err_other\":%u},"
                           "\"mle_counters\":{"
                           "\"disabled_role\":%u,\"detached_role\":%u,\"child_role\":%u,"
                           "\"router_role\":%u,\"leader_role\":%u,"
                           "\"attach_attempts\":%u,\"partition_id_changes\":%u,"
                           "\"better_partition_attach_attempts\":%u,\"parent_changes\":%u,"
                           "\"disabled_time_ms\":%s,\"detached_time_ms\":%s,\"child_time_ms\":%s,"
                           "\"router_time_ms\":%s,\"leader_time_ms\":%s,\"tracked_time_ms\":%s},"
                           "\"commissioner\":%s,\"joiner_events\":%s}",
                           uptime_str,
                           (unsigned)esp_get_free_heap_size(),
                           role,
                           (unsigned)mac_copy.mTxTotal,
                           (unsigned)mac_copy.mTxUnicast,
                           (unsigned)mac_copy.mTxBroadcast,
                           (unsigned)mac_copy.mTxAcked,
                           (unsigned)mac_copy.mTxRetry,
                           (unsigned)mac_copy.mTxBeacon,
                           (unsigned)mac_copy.mTxBeaconRequest,
                           (unsigned)mac_copy.mTxErrCca,
                           (unsigned)mac_copy.mTxErrAbort,
                           (unsigned)mac_copy.mTxErrBusyChannel,
                           (unsigned)mac_copy.mRxTotal,
                           (unsigned)mac_copy.mRxUnicast,
                           (unsigned)mac_copy.mRxBroadcast,
                           (unsigned)mac_copy.mRxData,
                           (unsigned)mac_copy.mRxDataPoll,
                           (unsigned)mac_copy.mRxBeacon,
                           (unsigned)mac_copy.mRxBeaconRequest,
                           (unsigned)mac_copy.mRxOther,
                           (unsigned)mac_copy.mRxAddressFiltered,
                           (unsigned)mac_copy.mRxDestAddrFiltered,
                           (unsigned)mac_copy.mRxDuplicated,
                           (unsigned)mac_copy.mRxErrNoFrame,
                           (unsigned)mac_copy.mRxErrUnknownNeighbor,
                           (unsigned)mac_copy.mRxErrInvalidSrcAddr,
                           (unsigned)mac_copy.mRxErrSec,
                           (unsigned)mac_copy.mRxErrFcs,
                           (unsigned)mac_copy.mRxErrOther,
                           (unsigned)mle_copy.mDisabledRole,
                           (unsigned)mle_copy.mDetachedRole,
                           (unsigned)mle_copy.mChildRole,
                           (unsigned)mle_copy.mRouterRole,
                           (unsigned)mle_copy.mLeaderRole,
                           (unsigned)mle_copy.mAttachAttempts,
                           (unsigned)mle_copy.mPartitionIdChanges,
                           (unsigned)mle_copy.mBetterPartitionAttachAttempts,
                           (unsigned)mle_copy.mParentChanges,
                           disabled_time,
                           detached_time,
                           child_time,
                           router_time,
                           leader_time,
                           tracked_time,
                           buffers->commissioner_json,
                           buffers->events_json);
    if (written < 0 || written >= (int)sizeof(buffers->json)) {
        free(buffers);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "thread diagnostics snapshot too large");
        return ESP_OK;
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
    /* Unauthenticated read-only GET, same boundary as /bos/status. */
    httpd_uri_t thread_diag_uri = {
        .uri = "/bos/thread-diag",
        .method = HTTP_GET,
        .handler = thread_diag_get_handler,
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
    httpd_uri_t commission_uri = {
        .uri = "/bos/commission",
        .method = HTTP_POST,
        .handler = bos_commissioning_http_post,
        .user_ctx = NULL,
    };
    /* The site server CommissioningService.imprint() hardcodes
     * POST /api/commission (host/runtime/server/src/commissioning.ts); keep
     * that path as an alias of the LAN-direct route so the existing imprint
     * sender reaches the BR unmodified. */
    httpd_uri_t commission_alias_uri = {
        .uri = "/api/commission",
        .method = HTTP_POST,
        .handler = bos_commissioning_http_post,
        .user_ctx = NULL,
    };
    /* Commissioning phase 2b joiner acceptance (docs/06.2 phase 2b); handlers
     * and auth in bos_commissioning/bos_joiner.c. */
    httpd_uri_t joiner_accept_uri = {
        .uri = "/bos/joiner/accept",
        .method = HTTP_POST,
        .handler = bos_joiner_accept_http_post,
        .user_ctx = NULL,
    };
    httpd_uri_t joiner_status_uri = {
        .uri = "/bos/joiner/status",
        .method = HTTP_GET,
        .handler = bos_joiner_status_http_get,
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
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&thread_diag_uri), TAG, "failed to register /bos/thread-diag");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_uri), TAG, "failed to register /bos/ledger/active");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_push_uri), TAG, "failed to register /bos/ledger/push");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&commission_uri), TAG, "failed to register /bos/commission");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&commission_alias_uri), TAG, "failed to register /api/commission");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&joiner_accept_uri), TAG, "failed to register /bos/joiner/accept");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&joiner_status_uri), TAG, "failed to register /bos/joiner/status");
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
