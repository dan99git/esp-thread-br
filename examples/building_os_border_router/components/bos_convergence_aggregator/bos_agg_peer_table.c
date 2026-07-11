/**
 * Building OS Border Router: mesh convergence aggregator peer table.
 *
 * Owns the LRU peer table, its mutex, self-record filtering, TXT parsing,
 * and the merge path used by the SRP server scan.
 */

#include "bos_agg_internal.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "openthread/dns.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bos_agg";

bos_peer_t bos_agg_peers[BOS_AGG_PEER_TABLE_MAX];
size_t     bos_agg_peer_count = 0;
static SemaphoreHandle_t s_mutex;
static char s_self_instance_name[32];
static char s_self_host_name[48];

uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static SemaphoreHandle_t peer_mutex(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    return s_mutex;
}

bool try_lock_peers(TickType_t wait_ticks)
{
    SemaphoreHandle_t mutex = peer_mutex();
    if (mutex) {
        return xSemaphoreTake(mutex, wait_ticks) == pdTRUE;
    }
    return false;
}

void lock_peers(void)
{
    (void)try_lock_peers(portMAX_DELAY);
}

void unlock_peers(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0U) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

static void init_self_labels(void)
{
    if (s_self_instance_name[0] != '\0') {
        return;
    }

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_ETH);
    if (ret != ESP_OK) {
        ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (ret == ESP_OK) {
        snprintf(s_self_instance_name,
                 sizeof(s_self_instance_name),
                 "br-%02x%02x%02x%02x%02x%02x",
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    } else {
        snprintf(s_self_instance_name, sizeof(s_self_instance_name), "border-router");
    }
    snprintf(s_self_host_name, sizeof(s_self_host_name), "bos-%s", s_self_instance_name);
}

bool is_self_srp_record(const bos_peer_t *peer)
{
    if (!peer) {
        return false;
    }

    init_self_labels();
    return (peer->id[0] != '\0' && strcmp(peer->id, s_self_instance_name) == 0) ||
           (peer->mesh_instance[0] != '\0' && strcmp(peer->mesh_instance, s_self_instance_name) == 0) ||
           (peer->tmfs_instance[0] != '\0' && strcmp(peer->tmfs_instance, s_self_instance_name) == 0) ||
           (peer->host_name[0] != '\0' && strcmp(peer->host_name, s_self_host_name) == 0);
}

static bool parse_u32(const char *value, uint32_t *out)
{
    if (!value || !out || value[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

static bool parse_bool_txt(const char *value, bool *out)
{
    if (!value || !out) {
        return false;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void txt_value_to_cstr(const otDnsTxtEntry *entry, char *out, size_t out_len)
{
    if (!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';
    if (!entry || !entry->mValue || entry->mValueLength == 0U) {
        return;
    }
    size_t n = entry->mValueLength < out_len - 1U ? entry->mValueLength : out_len - 1U;
    memcpy(out, entry->mValue, n);
    out[n] = '\0';
}

void parse_txt_into_peer(bos_peer_t *peer, const uint8_t *txt, uint16_t txt_len)
{
    if (!peer || !txt || txt_len == 0U) {
        return;
    }

    otDnsTxtEntryIterator iterator;
    otDnsInitTxtEntryIterator(&iterator, txt, txt_len);
    otDnsTxtEntry entry;
    while (otDnsGetNextTxtEntry(&iterator, &entry) == OT_ERROR_NONE) {
        if (!entry.mKey) {
            continue;
        }
        char value[80];
        txt_value_to_cstr(&entry, value, sizeof(value));
        // _mesh._udp TXT contract: class, model, pkg, ver, hw, mid, fw, http, cm.
        // v3 keys (phonebook v3): sku (stock code string, may be empty),
        // rpw (rated_power_w watts), rl (rated_lumens int, may be empty),
        // cct (kelvin int).
        // Reserved future ledger keys from the device service: lver, ldig.
        if (strcmp(entry.mKey, "class") == 0) {
            copy_str(peer->device_class, sizeof(peer->device_class), value);
        } else if (strcmp(entry.mKey, "dc") == 0 || strcmp(entry.mKey, "device_class") == 0) {
            copy_str(peer->device_class, sizeof(peer->device_class), value);
        } else if (strcmp(entry.mKey, "model") == 0) {
            copy_str(peer->model, sizeof(peer->model), value);
        } else if (strcmp(entry.mKey, "mid") == 0 || strcmp(entry.mKey, "model_id") == 0) {
            copy_str(peer->model_id, sizeof(peer->model_id), value);
        } else if (strcmp(entry.mKey, "sku") == 0) {
            /* sku: manufacturer stock code string (e.g. "OPA-123940").
             * Empty broadcast => empty string (recorded as null, never invented). */
            copy_str(peer->sku, sizeof(peer->sku), value);
        } else if (strcmp(entry.mKey, "rpw") == 0) {
            /* rated_power_w: system/panel watts, string form (e.g. "31").
             * Never amps. Empty broadcast => empty string. */
            copy_str(peer->rated_power_w, sizeof(peer->rated_power_w), value);
        } else if (strcmp(entry.mKey, "rl") == 0) {
            /* rated_lumens: integer. Empty TXT (card omits it) => parse fails,
             * has_rated_lumens stays false so the column renders blank/null. */
            uint32_t lumens = 0;
            if (parse_u32(value, &lumens)) {
                peer->rated_lumens = (int)lumens;
                peer->has_rated_lumens = true;
            }
        } else if (strcmp(entry.mKey, "cct") == 0) {
            uint32_t cct = 0;
            if (parse_u32(value, &cct)) {
                peer->cct = (int)cct;
                peer->has_cct = true;
            }
        } else if (strcmp(entry.mKey, "pkg") == 0) {
            copy_str(peer->package_state, sizeof(peer->package_state), value);
        } else if (strcmp(entry.mKey, "ver") == 0 || strcmp(entry.mKey, "v") == 0) {
            copy_str(peer->service_version, sizeof(peer->service_version), value);
        } else if (strcmp(entry.mKey, "fw") == 0 ||
                   strcmp(entry.mKey, "firmware") == 0 ||
                   strcmp(entry.mKey, "firmware_version") == 0) {
            copy_str(peer->firmware_version, sizeof(peer->firmware_version), value);
        } else if (strcmp(entry.mKey, "http") == 0) {
            uint32_t port = 0;
            if (parse_u32(value, &port) && port <= UINT16_MAX) {
                peer->http_port = (uint16_t)port;
                peer->has_http_port = true;
            }
        } else if (strcmp(entry.mKey, "cm") == 0 || strcmp(entry.mKey, "commissioned") == 0) {
            bool commissioned = false;
            if (parse_bool_txt(value, &commissioned)) {
                peer->commissioned = commissioned;
                peer->has_commissioned = true;
            }
        } else if (strcmp(entry.mKey, "nid") == 0) {
            copy_str(peer->id, sizeof(peer->id), value);
        } else if (strcmp(entry.mKey, "hw") == 0 ||
                   strcmp(entry.mKey, "hid") == 0 ||
                   strcmp(entry.mKey, "eui64") == 0 ||
                   strcmp(entry.mKey, "hardware_id") == 0) {
            copy_str(peer->hardware_id, sizeof(peer->hardware_id), value);
        } else if (strcmp(entry.mKey, "cat") == 0) {
            copy_str(peer->tmfs_catalog, sizeof(peer->tmfs_catalog), value);
        } else if (strcmp(entry.mKey, "caps") == 0) {
            copy_str(peer->tmfs_caps, sizeof(peer->tmfs_caps), value);
        } else if (strcmp(entry.mKey, "wp") == 0) {
            copy_str(peer->tmfs_wire_payload, sizeof(peer->tmfs_wire_payload), value);
        } else if (strcmp(entry.mKey, "role") == 0) {
            copy_str(peer->role, sizeof(peer->role), value);
        } else if (strcmp(entry.mKey, "state") == 0) {
            copy_str(peer->reported_state, sizeof(peer->reported_state), value);
        } else if (strcmp(entry.mKey, "lv") == 0) {
            uint32_t version = 0;
            if (parse_u32(value, &version)) {
                peer->ledger_version = version;
                peer->has_ledger_version = true;
            }
        } else if (strcmp(entry.mKey, "ld") == 0) {
            copy_str(peer->digest_hex, sizeof(peer->digest_hex), value);
            peer->has_digest = peer->digest_hex[0] != '\0';
        } else if (strcmp(entry.mKey, "have") == 0) {
            uint32_t have = 0;
            if (parse_u32(value, &have)) {
                peer->chunks_have = have;
                peer->has_chunks = true;
            }
        } else if (strcmp(entry.mKey, "chunks") == 0 || strcmp(entry.mKey, "ct") == 0) {
            uint32_t total = 0;
            if (parse_u32(value, &total)) {
                peer->chunks_total = total;
                peer->has_chunks = true;
            }
        }
    }
}

static int find_peer_by_id(const char *id)
{
    if (!id || id[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < bos_agg_peer_count; i++) {
        if (strcmp(bos_agg_peers[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t allocate_peer_slot(uint64_t seen_ms)
{
    if (bos_agg_peer_count < BOS_AGG_PEER_TABLE_MAX) {
        return bos_agg_peer_count++;
    }

    size_t oldest = 0;
    for (size_t i = 1; i < bos_agg_peer_count; i++) {
        if (bos_agg_peers[i].last_seen_ms < bos_agg_peers[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    memset(&bos_agg_peers[oldest], 0, sizeof(bos_agg_peers[oldest]));
    bos_agg_peers[oldest].last_seen_ms = seen_ms;
    return oldest;
}

void merge_peer(const bos_peer_t *update)
{
    if (!update || update->id[0] == '\0') {
        return;
    }

    uint64_t seen = now_ms();
    lock_peers();
    int existing = find_peer_by_id(update->id);
    size_t idx = existing >= 0 ? (size_t)existing : allocate_peer_slot(seen);
    bos_peer_t *peer = &bos_agg_peers[idx];
    if (existing < 0) {
        copy_str(peer->id, sizeof(peer->id), update->id);
    }

    if (update->has_mesh) {
        peer->has_mesh = true;
        copy_str(peer->mesh_instance, sizeof(peer->mesh_instance), update->mesh_instance);
        peer->coap_port = update->coap_port;
    }
    if (update->has_tmfs) {
        peer->has_tmfs = true;
        copy_str(peer->tmfs_instance, sizeof(peer->tmfs_instance), update->tmfs_instance);
        peer->tmfs_port = update->tmfs_port;
    }
    if (update->host_name[0] != '\0') {
        copy_str(peer->host_name, sizeof(peer->host_name), update->host_name);
    }
    if (update->hardware_id[0] != '\0') {
        copy_str(peer->hardware_id, sizeof(peer->hardware_id), update->hardware_id);
    }
    if (update->has_address) {
        memcpy(peer->address, update->address, sizeof(peer->address));
        peer->has_address = true;
    }
    if (update->model[0] != '\0') {
        copy_str(peer->model, sizeof(peer->model), update->model);
    }
    if (update->model_id[0] != '\0') {
        copy_str(peer->model_id, sizeof(peer->model_id), update->model_id);
    }
    if (update->sku[0] != '\0') {
        copy_str(peer->sku, sizeof(peer->sku), update->sku);
    }
    if (update->rated_power_w[0] != '\0') {
        copy_str(peer->rated_power_w, sizeof(peer->rated_power_w), update->rated_power_w);
    }
    if (update->has_rated_lumens) {
        peer->rated_lumens = update->rated_lumens;
        peer->has_rated_lumens = true;
    }
    if (update->has_cct) {
        peer->cct = update->cct;
        peer->has_cct = true;
    }
    if (update->device_class[0] != '\0') {
        copy_str(peer->device_class, sizeof(peer->device_class), update->device_class);
    }
    if (update->package_state[0] != '\0') {
        copy_str(peer->package_state, sizeof(peer->package_state), update->package_state);
    }
    if (update->service_version[0] != '\0') {
        copy_str(peer->service_version, sizeof(peer->service_version), update->service_version);
    }
    if (update->firmware_version[0] != '\0') {
        copy_str(peer->firmware_version, sizeof(peer->firmware_version), update->firmware_version);
    }
    if (update->has_http_port) {
        peer->http_port = update->http_port;
        peer->has_http_port = true;
    }
    if (update->has_commissioned) {
        peer->commissioned = update->commissioned;
        peer->has_commissioned = true;
    }
    if (update->tmfs_catalog[0] != '\0') {
        copy_str(peer->tmfs_catalog, sizeof(peer->tmfs_catalog), update->tmfs_catalog);
    }
    if (update->tmfs_caps[0] != '\0') {
        copy_str(peer->tmfs_caps, sizeof(peer->tmfs_caps), update->tmfs_caps);
    }
    if (update->tmfs_wire_payload[0] != '\0') {
        copy_str(peer->tmfs_wire_payload, sizeof(peer->tmfs_wire_payload), update->tmfs_wire_payload);
    }
    if (update->role[0] != '\0') {
        copy_str(peer->role, sizeof(peer->role), update->role);
    }
    if (update->reported_state[0] != '\0') {
        copy_str(peer->reported_state, sizeof(peer->reported_state), update->reported_state);
    }
    if (update->has_ledger_version) {
        peer->ledger_version = update->ledger_version;
        peer->has_ledger_version = true;
    }
    if (update->has_digest) {
        copy_str(peer->digest_hex, sizeof(peer->digest_hex), update->digest_hex);
        peer->has_digest = true;
    }
    if (update->has_chunks) {
        peer->chunks_have = update->chunks_have;
        peer->chunks_total = update->chunks_total;
        peer->has_chunks = true;
    }
    peer->last_seen_ms = seen;
    unlock_peers();
}

esp_err_t bos_convergence_aggregator_init(void)
{
    (void)peer_mutex();
    lock_peers();
    memset(bos_agg_peers, 0, sizeof(bos_agg_peers));
    bos_agg_peer_count = 0;
    unlock_peers();
    ESP_LOGI(TAG, "convergence peer table initialised");
    return ESP_OK;
}
