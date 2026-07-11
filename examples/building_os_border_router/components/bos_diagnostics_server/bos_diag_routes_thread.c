/**
 * Building OS Border Router: local diagnostics web surface.
 * Split part: Thread diagnostics, radio, neighbor, reset, and identity-reset
 * routes (see bos_diag_internal.h).
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
esp_err_t thread_diag_get_handler(httpd_req_t *req)
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

/* GET /bos/radio: the BR 802.15.4 radio window. The H2 RCP TX power is the
 * only C6-equivalent RF lever on this board (H2-MINI fixed PCB antenna,
 * single RF path; docs/scratch/br-range-and-fork-review-2026-06-12.md), so
 * the read returns tx_power_dbm (otPlatRadioGetTransmitPower readback, the
 * radio's achieved value, not a cached request) plus the live channel.
 * Open-on-LAN read, same boundary as /bos/status. Consumer contract:
 * tools/codex-plugin/plugins/building-os/scripts/br-radio.mjs. */
esp_err_t radio_get_handler(httpd_req_t *req)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        return thread_diag_send_unavailable(req, "openthread lock timeout");
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        return thread_diag_send_unavailable(req, "openthread not running");
    }
    int8_t tx_power = 0;
    otError tx_err = otPlatRadioGetTransmitPower(instance, &tx_power);
    uint8_t channel = otLinkGetChannel(instance);
    esp_openthread_lock_release();

    if (tx_err != OT_ERROR_NONE) {
        return thread_diag_send_unavailable(req, "tx power not readable");
    }

    char json[96];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"tx_power_dbm\":%d,\"channel\":%u}",
                           (int)tx_power,
                           (unsigned)channel);
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "radio snapshot too large");
        return ESP_OK;
    }
    send_json(req, json);
    return ESP_OK;
}

/* POST /bos/radio: set the live H2 802.15.4 TX power. Body
 * {"tx_power_dbm":N}, N integer -24..20 (H2 RCP supported range; the radio
 * clamps and the response returns the achieved readback). Token-authed, same
 * trust anchor as /bos/commission (bos_commissioning_request_authorized).
 * Does NOT persist: the boot default is the app_main.c 20 dBm assert. */
esp_err_t radio_post_handler(httpd_req_t *req)
{
    if (!bos_commissioning_request_authorized(req)) {
        return send_unauthorized(req);
    }

    char body[160];
    if (read_request_body(req, body, sizeof(body)) < 0) {
        return send_status_json(req, "400 Bad Request", "empty or oversized body");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_status_json(req, "400 Bad Request", "invalid JSON");
    }
    const cJSON *tx_j = cJSON_GetObjectItemCaseSensitive(root, "tx_power_dbm");
    if (!cJSON_IsNumber(tx_j)) {
        cJSON_Delete(root);
        return send_status_json(req, "400 Bad Request", "tx_power_dbm must be a number");
    }
    int requested = tx_j->valueint;
    cJSON_Delete(root);
    if (requested < -24 || requested > 20) {
        return send_status_json(req, "400 Bad Request", "tx_power_dbm out of range (-24..20 dBm)");
    }

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        return thread_diag_send_unavailable(req, "openthread lock timeout");
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        return thread_diag_send_unavailable(req, "openthread not running");
    }
    otError set_err = otPlatRadioSetTransmitPower(instance, (int8_t)requested);
    int8_t achieved = 0;
    otError get_err = otPlatRadioGetTransmitPower(instance, &achieved);
    esp_openthread_lock_release();

    if (set_err != OT_ERROR_NONE) {
        return send_status_json(req, "500 Internal Server Error", "failed to set tx power");
    }
    ESP_LOGI(TAG, "802.15.4 TX power set over /bos/radio: requested %d, achieved %d",
             requested, (get_err == OT_ERROR_NONE) ? (int)achieved : requested);

    char json[96];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"ok\":true,\"tx_power_dbm\":%d}",
                           (get_err == OT_ERROR_NONE) ? (int)achieved : requested);
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "radio snapshot too large");
        return ESP_OK;
    }
    send_json(req, json);
    return ESP_OK;
}

/* GET /bos/thread-neighbors: local neighbor table with per-link RSSI/LQI -
 * the C6<->BR link-quality window seen from the BR (the missing window from
 * the 2026-06-12 morning bench, docs/scratch/radio-levers-and-windows W2).
 * Rendering is the shared bos_thread_diag component
 * (firmware/shared-components/bos_thread_diag, docs/08.2 section 7); the
 * walk runs under the held OT lock, BR cap 32. Output shape matches the C6
 * /api/thread-neighbors: role, rloc16, neighbor_count, neighbors[].
 * Open-on-LAN read. Consumer contract: br-thread-neighbors.mjs. */
esp_err_t thread_neighbors_get_handler(httpd_req_t *req)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        return thread_diag_send_unavailable(req, "openthread lock timeout");
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        return thread_diag_send_unavailable(req, "openthread not running");
    }
    const char *role = otThreadDeviceRoleToString(otThreadGetDeviceRole(instance));
    uint16_t self_rloc16 = otThreadGetRloc16(instance);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = root ? cJSON_CreateArray() : NULL;
    if (!root || !arr) {
        esp_openthread_lock_release();
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return thread_diag_send_unavailable(req, "out of memory");
    }
    size_t count = bos_thread_diag_add_neighbors(instance, arr, 32);
    esp_openthread_lock_release();

    cJSON_AddStringToObject(root, "role", role);
    char self_hex[8];
    snprintf(self_hex, sizeof(self_hex), "0x%04x", (unsigned)self_rloc16);
    cJSON_AddStringToObject(root, "rloc16", self_hex);
    cJSON_AddNumberToObject(root, "neighbor_count", (double)count);
    cJSON_AddItemToObject(root, "neighbors", arr);

    esp_err_t ret = send_cjson(req, root);
    cJSON_Delete(root);
    return ret;
}

/* POST /bos/thread-reset: RCP hard reset, the wedged-RCP recovery lever
 * (skill br-thread-commissioning-diagnostics; first built and executed live
 * 2026-06-11, lost in the 2026-06-12 revert). Uses the established
 * esp_rcp_update mechanism (esp_rcp_reset(), components/esp_rcp_update/src/
 * esp_rcp_update.c:322 - drives CONFIG_PIN_TO_RCP_RESET, GPIO 7 on this
 * board) rather than raw GPIO. Sequence: Thread + IPv6 down under the OT
 * lock, RCP reset pulse, settle, IPv6 + Thread back up; the persisted
 * operational dataset (bos_thread_dataset_anchor) re-adopts on re-enable, so
 * the network re-forms (~20 s to leader on the bench record). Token-authed
 * write, same trust anchor as /bos/commission. */
esp_err_t thread_reset_post_handler(httpd_req_t *req)
{
    if (!bos_commissioning_request_authorized(req)) {
        return send_unauthorized(req);
    }

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {
        return thread_diag_send_unavailable(req, "openthread lock timeout");
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        return thread_diag_send_unavailable(req, "openthread not running");
    }
    otThreadSetEnabled(instance, false);
    otIp6SetEnabled(instance, false);
    esp_openthread_lock_release();

    ESP_LOGW(TAG, "RCP hard reset requested over /bos/thread-reset");
    esp_rcp_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {
        return thread_diag_send_unavailable(req, "openthread lock timeout after rcp reset");
    }
    otError ip6_err = otIp6SetEnabled(instance, true);
    otError thread_err = otThreadSetEnabled(instance, true);
    const char *role = otThreadDeviceRoleToString(otThreadGetDeviceRole(instance));
    esp_openthread_lock_release();

    char json[192];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"ok\":%s,\"reset\":\"rcp_hard_reset\",\"ip6_reenabled\":%s,"
                           "\"thread_reenabled\":%s,\"role\":\"%s\"}",
                           (ip6_err == OT_ERROR_NONE && thread_err == OT_ERROR_NONE) ? "true" : "false",
                           (ip6_err == OT_ERROR_NONE) ? "true" : "false",
                           (thread_err == OT_ERROR_NONE) ? "true" : "false",
                           role);
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reset snapshot too large");
        return ESP_OK;
    }
    send_json(req, json);
    return ESP_OK;
}

/* POST /bos/device-id/reset: confirm-gated identity reset. Erases the NVS
 * bos_reg/device_id override AND bos_reg/device_token so the next boot
 * re-derives the EUI-64 device id (bos_server_registration.c load_device_id:
 * ESP_MAC_IEEE802154, else base MAC with ff:fe inserted), persists it, and
 * re-claims with the site server under the new id. Shipped to flip the
 * legacy colon-MAC identity ("9c:13:9e:0a:47:47") to the durable EUI-64
 * without reflashing: the override wins over derivation, so OTA alone cannot
 * fix it. The token erase is load-bearing (0.1.21 fix, live defect on the
 * 0.1.20 bench run 2026-07-02): with the old token kept, is_registered()
 * stays true, the registration task never re-claims, the gateway never
 * learns the new id, and every heartbeat 404s into the RAM buffer.
 * Token-authed write, same trust anchor as /bos/ota/app (X-Device-Token /
 * api key via bos_commissioning_request_authorized). Refusal shape mirrors
 * the C6 install-retrigger 428 plan (firmware/xiao-esp32c6/main/
 * web_routes_card.c handle_api_install_retrigger): {"confirm": true}
 * required in the JSON body; without it the route returns the exact plan and
 * takes no action. On accept the 200 response is sent FIRST, then the
 * two-key erase runs and the BR restarts after a flush delay. A failed erase
 * logs ESP_LOGE and does NOT restart. */
esp_err_t device_id_reset_post_handler(httpd_req_t *req)
{
    if (!bos_commissioning_request_authorized(req)) {
        return send_unauthorized(req);
    }

    char body[160];
    if (read_request_body(req, body, sizeof(body)) < 0) {
        return send_status_json(req, "400 Bad Request", "empty or oversized body");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_status_json(req, "400 Bad Request", "invalid JSON");
    }
    const cJSON *confirm_j = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    bool confirmed = cJSON_IsTrue(confirm_j);
    cJSON_Delete(root);

    char current_id[64] = "";
    if (bos_server_registration_get_device_id(current_id, sizeof(current_id)) != ESP_OK) {
        current_id[0] = '\0';
    }

    if (!confirmed) {
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return send_status_json(req, "500 Internal Server Error", "out of memory");
        }
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddBoolToObject(resp, "erase_executed", false);
        cJSON_AddStringToObject(resp, "refusal", "confirmation_required");
        cJSON_AddStringToObject(resp, "required_confirmation", "{\"confirm\": true}");
        cJSON *plan = cJSON_CreateObject();
        cJSON_AddStringToObject(plan, "nvs_namespace", BOS_REG_NVS_NAMESPACE);
        cJSON *keys = cJSON_CreateArray();
        cJSON_AddItemToArray(keys, cJSON_CreateString(BOS_REG_KEY_DEVICE_ID));
        cJSON_AddItemToArray(keys, cJSON_CreateString(BOS_REG_KEY_TOKEN));
        cJSON_AddItemToObject(plan, "nvs_keys", keys);
        cJSON_AddStringToObject(plan, "current_device_id", current_id);
        cJSON *effect = cJSON_CreateArray();
        cJSON_AddItemToArray(effect, cJSON_CreateString(
            "NVS keys bos_reg/device_id and bos_reg/device_token erased (server_url and all other keys untouched)"));
        cJSON_AddItemToArray(effect, cJSON_CreateString(
            "the BR restarts right after the erase"));
        cJSON_AddItemToArray(effect, cJSON_CreateString(
            "on boot the device id is re-derived as EUI-64 (ESP_MAC_IEEE802154, else base MAC with ff:fe inserted), persisted to NVS, and the BR re-claims with the site server under the new id for a fresh token"));
        cJSON_AddItemToObject(plan, "effect", effect);
        cJSON_AddStringToObject(plan, "why",
            "The NVS device_id override wins over EUI-64 derivation (load_device_id override-first), so the legacy colon-MAC id survives every OTA; the old token must also go or is_registered() stays true and the BR never re-claims, leaving the gateway on the old id and heartbeats failing.");
        cJSON_AddItemToObject(resp, "plan", plan);
        httpd_resp_set_status(req, "428 Precondition Required");
        esp_err_t ret = send_cjson(req, resp);
        cJSON_Delete(resp);
        return ret;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return send_status_json(req, "500 Internal Server Error", "out of memory");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "rebooting", true);
    cJSON_AddStringToObject(resp, "old_device_id", current_id);
    cJSON_AddStringToObject(resp, "derivation", "eui64");
    esp_err_t ret = send_cjson(req, resp);
    cJSON_Delete(resp);

    /* Response is sent; give lwip time to flush the socket before the erase
     * and restart (same flush model as the C6 install retrigger). */
    vTaskDelay(pdMS_TO_TICKS(750));

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(BOS_REG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, BOS_REG_KEY_DEVICE_ID);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            /* No override stored: nothing to erase, boot derivation already
             * rules. Proceed to the token erase. */
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            /* Token erase is load-bearing: with the old token kept,
             * is_registered() stays true and the BR never re-claims under
             * the re-derived id (0.1.20 live defect, 2026-07-02). */
            err = nvs_erase_key(nvs, BOS_REG_KEY_TOKEN);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        /* Loud failure, no restart: rebooting after a failed erase would just
         * re-register the legacy id and mask the fault. The 200 response is
         * already sent, so the log is the only remaining reporting channel. */
        ESP_LOGE(TAG, "device-id reset: NVS erase of %s/%s+%s FAILED (%s); NOT restarting",
                 BOS_REG_NVS_NAMESPACE, BOS_REG_KEY_DEVICE_ID, BOS_REG_KEY_TOKEN, esp_err_to_name(err));
        return ret;
    }
    ESP_LOGI(TAG, "device-id reset: NVS %s/%s and %s erased (old id \"%s\"); restarting to re-derive EUI-64 and re-claim",
             BOS_REG_NVS_NAMESPACE, BOS_REG_KEY_DEVICE_ID, BOS_REG_KEY_TOKEN, current_id);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ret; /* not reached */
}
