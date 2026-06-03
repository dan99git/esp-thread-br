/**
 * Building OS Border Router: mesh convergence aggregator.
 */

#include "bos_convergence_aggregator.h"

#include "bos_ledger_ingress.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/dns.h"
#include "openthread/ip6.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bos_agg";

#define BOS_AGG_SERVICE_MESH "_mesh._udp"
#define BOS_AGG_SERVICE_TMFS "_tmfs._udp"
#define BOS_AGG_ATTACH_POLL_MS 1000U
#define BOS_AGG_FIRST_BROWSE_DELAY_MS 10000U
#define BOS_AGG_BROWSE_PERIOD_MS 30000U
#define BOS_AGG_OFFLINE_TIMEOUT_MS (30U * 60U * 1000U)
#define BOS_AGG_HOST_MAX 96U

static bos_peer_t s_peers[BOS_AGG_PEER_TABLE_MAX];
static size_t     s_peer_count = 0;
static bool       s_started = false;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_mutex;
static char s_self_instance_name[32];
static char s_self_host_name[48];

static uint64_t now_ms(void)
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

static bool try_lock_peers(TickType_t wait_ticks)
{
    SemaphoreHandle_t mutex = peer_mutex();
    if (mutex) {
        return xSemaphoreTake(mutex, wait_ticks) == pdTRUE;
    }
    return false;
}

static void lock_peers(void)
{
    (void)try_lock_peers(portMAX_DELAY);
}

static void unlock_peers(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static void copy_str(char *dst, size_t dst_len, const char *src)
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

static bool is_self_srp_record(const bos_peer_t *peer)
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

static bool is_zero_addr(const otIp6Address *addr)
{
    static const uint8_t zero[16] = {0};
    return !addr || memcmp(addr->mFields.m8, zero, sizeof(zero)) == 0;
}

static void ip6_to_hex(const uint8_t addr[16], char out[40])
{
    snprintf(out,
             40,
             "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             addr[0],
             addr[1],
             addr[2],
             addr[3],
             addr[4],
             addr[5],
             addr[6],
             addr[7],
             addr[8],
             addr[9],
             addr[10],
             addr[11],
             addr[12],
             addr[13],
             addr[14],
             addr[15]);
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

static bool digest_matches_target(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (!peer || !active || !active->present || !peer->has_digest) {
        return false;
    }
    char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
    digest_hex(active->digest, digest);
    size_t peer_len = strlen(peer->digest_hex);
    return peer_len > 0U && peer_len <= BOS_LEDGER_DIGEST_LEN * 2U &&
           strncmp(peer->digest_hex, digest, peer_len) == 0;
}

static bool peer_is_current(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    return peer && active && active->present && peer->has_ledger_version &&
           peer->ledger_version == active->version && digest_matches_target(peer, active);
}

static const char *peer_role(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (!peer) {
        return "peer";
    }
    if (peer->role[0] != '\0') {
        return peer->role;
    }
    if (peer_is_current(peer, active)) {
        return "seed";
    }
    return peer->has_tmfs ? "peer" : "leaf";
}

static const char *peer_transfer_state(const bos_peer_t *peer, const bos_ledger_active_t *active, uint64_t now)
{
    if (!peer) {
        return "unknown";
    }
    uint64_t age = now > peer->last_seen_ms ? now - peer->last_seen_ms : 0;
    if (age > BOS_AGG_OFFLINE_TIMEOUT_MS) {
        return "offline";
    }
    if (age > BOS_AGG_STALE_TIMEOUT_MS) {
        return "stale";
    }
    if (peer->reported_state[0] != '\0') {
        return peer->reported_state;
    }
    if (peer_is_current(peer, active)) {
        return "seeding";
    }
    if (peer->has_chunks && peer->chunks_total > 0U && peer->chunks_have < peer->chunks_total) {
        return "leeching";
    }
    if (peer->has_tmfs) {
        return "torrent-capable";
    }
    if (peer->has_mesh) {
        return "discovered";
    }
    return "unknown";
}

static uint32_t peer_chunks_have(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (peer_is_current(peer, active)) {
        return active->chunk_count;
    }
    if (peer && peer->has_chunks) {
        return peer->chunks_have;
    }
    return 0;
}

static uint32_t peer_chunks_total(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (peer && peer->has_chunks && peer->chunks_total > 0U) {
        return peer->chunks_total;
    }
    return active && active->present ? active->chunk_count : 0;
}

static uint32_t peer_progress_pct(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (peer_is_current(peer, active)) {
        return 100;
    }
    uint32_t total = peer_chunks_total(peer, active);
    uint32_t have = peer_chunks_have(peer, active);
    if (total == 0U || have == 0U) {
        return 0;
    }
    return (have * 100U) / total;
}

static int appendf(char **cursor, size_t *remaining, const char *fmt, ...)
{
    if (!cursor || !*cursor || !remaining || *remaining == 0U || !fmt) {
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(*cursor, *remaining, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= *remaining) {
        return -1;
    }
    *cursor += written;
    *remaining -= (size_t)written;
    return written;
}

static int append_raw(char **cursor, size_t *remaining, const char *value)
{
    if (!cursor || !*cursor || !remaining || *remaining == 0U || !value) {
        return -1;
    }

    size_t len = strlen(value);
    if (len >= *remaining) {
        return -1;
    }
    memcpy(*cursor, value, len);
    *cursor += len;
    *remaining -= len;
    **cursor = '\0';
    return (int)len;
}

static int append_u64_decimal(char **cursor, size_t *remaining, uint64_t value)
{
    char buf[21];
    size_t pos = sizeof(buf);
    buf[--pos] = '\0';

    do {
        buf[--pos] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value > 0U && pos > 0U);

    return append_raw(cursor, remaining, &buf[pos]);
}

static int append_json_string(char **cursor, size_t *remaining, const char *value)
{
    if (appendf(cursor, remaining, "\"") < 0) {
        return -1;
    }
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    while (*p) {
        if (*p == '"' || *p == '\\') {
            if (appendf(cursor, remaining, "\\%c", *p) < 0) {
                return -1;
            }
        } else if (*p >= 0x20U && *p < 0x7fU) {
            if (appendf(cursor, remaining, "%c", *p) < 0) {
                return -1;
            }
        } else {
            if (appendf(cursor, remaining, "\\u%04x", (unsigned)*p) < 0) {
                return -1;
            }
        }
        p++;
    }
    return appendf(cursor, remaining, "\"");
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

static void parse_txt_into_peer(bos_peer_t *peer, const uint8_t *txt, uint16_t txt_len)
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
        if (strcmp(entry.mKey, "class") == 0) {
            copy_str(peer->device_class, sizeof(peer->device_class), value);
        } else if (strcmp(entry.mKey, "model") == 0) {
            copy_str(peer->model, sizeof(peer->model), value);
        } else if (strcmp(entry.mKey, "pkg") == 0) {
            copy_str(peer->package_state, sizeof(peer->package_state), value);
        } else if (strcmp(entry.mKey, "ver") == 0 || strcmp(entry.mKey, "v") == 0) {
            copy_str(peer->service_version, sizeof(peer->service_version), value);
        } else if (strcmp(entry.mKey, "nid") == 0) {
            copy_str(peer->id, sizeof(peer->id), value);
        } else if (strcmp(entry.mKey, "hw") == 0 ||
                   strcmp(entry.mKey, "hid") == 0 ||
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
    for (size_t i = 0; i < s_peer_count; i++) {
        if (strcmp(s_peers[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t allocate_peer_slot(uint64_t seen_ms)
{
    if (s_peer_count < BOS_AGG_PEER_TABLE_MAX) {
        return s_peer_count++;
    }

    size_t oldest = 0;
    for (size_t i = 1; i < s_peer_count; i++) {
        if (s_peers[i].last_seen_ms < s_peers[oldest].last_seen_ms) {
            oldest = i;
        }
    }
    memset(&s_peers[oldest], 0, sizeof(s_peers[oldest]));
    s_peers[oldest].last_seen_ms = seen_ms;
    return oldest;
}

static void merge_peer(const bos_peer_t *update)
{
    if (!update || update->id[0] == '\0') {
        return;
    }

    uint64_t seen = now_ms();
    lock_peers();
    int existing = find_peer_by_id(update->id);
    size_t idx = existing >= 0 ? (size_t)existing : allocate_peer_slot(seen);
    bos_peer_t *peer = &s_peers[idx];
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
    if (update->device_class[0] != '\0') {
        copy_str(peer->device_class, sizeof(peer->device_class), update->device_class);
    }
    if (update->package_state[0] != '\0') {
        copy_str(peer->package_state, sizeof(peer->package_state), update->package_state);
    }
    if (update->service_version[0] != '\0') {
        copy_str(peer->service_version, sizeof(peer->service_version), update->service_version);
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

static bool service_name_matches_type(const char *service_name, const char *service_type)
{
    return service_name && service_type && strstr(service_name, service_type) != NULL;
}

static void merge_srp_server_service(const otSrpServerHost *host, const otSrpServerService *service)
{
    const char *service_name = otSrpServerServiceGetServiceName(service);
    bool is_tmfs = service_name_matches_type(service_name, BOS_AGG_SERVICE_TMFS);
    bool is_mesh = service_name_matches_type(service_name, BOS_AGG_SERVICE_MESH);

    if (!is_tmfs && !is_mesh) {
        return;
    }

    const char *instance = otSrpServerServiceGetInstanceLabel(service);
    if (!instance || instance[0] == '\0') {
        instance = otSrpServerServiceGetInstanceName(service);
    }
    if (!instance || instance[0] == '\0') {
        return;
    }

    bos_peer_t update = {0};
    copy_str(update.id, sizeof(update.id), instance);
    copy_str(update.host_name, sizeof(update.host_name), otSrpServerHostGetFullName(host));

    uint16_t txt_len = 0;
    const uint8_t *txt = otSrpServerServiceGetTxtData(service, &txt_len);
    parse_txt_into_peer(&update, txt, txt_len);
    if (update.id[0] == '\0') {
        copy_str(update.id, sizeof(update.id), instance);
    }

    if (is_tmfs) {
        update.has_tmfs = true;
        update.tmfs_port = otSrpServerServiceGetPort(service);
        copy_str(update.tmfs_instance, sizeof(update.tmfs_instance), instance);
    }
    if (is_mesh) {
        update.has_mesh = true;
        update.coap_port = otSrpServerServiceGetPort(service);
        copy_str(update.mesh_instance, sizeof(update.mesh_instance), instance);
    }

    uint8_t address_count = 0;
    const otIp6Address *addresses = otSrpServerHostGetAddresses(host, &address_count);
    for (uint8_t i = 0; addresses && i < address_count; i++) {
        if (!is_zero_addr(&addresses[i])) {
            memcpy(update.address, addresses[i].mFields.m8, sizeof(update.address));
            update.has_address = true;
            break;
        }
    }

    if (!is_self_srp_record(&update)) {
        merge_peer(&update);
    }
}

static void scan_srp_server_services(otInstance *instance)
{
    if (otSrpServerGetState(instance) != OT_SRP_SERVER_STATE_RUNNING) {
        return;
    }

    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL) {
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }

        const otSrpServerService *service = NULL;
        while ((service = otSrpServerHostGetNextService(host, service)) != NULL) {
            if (otSrpServerServiceIsDeleted(service)) {
                continue;
            }
            merge_srp_server_service(host, service);
        }
    }
}

static bool thread_role_attached(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_CHILD ||
           role == OT_DEVICE_ROLE_ROUTER ||
           role == OT_DEVICE_ROLE_LEADER;
}

static void browse_task(void *arg)
{
    (void)arg;
    bool wait_after_attach = true;

    for (;;) {
        otInstance *instance = esp_openthread_get_instance();
        if (!instance) {
            vTaskDelay(pdMS_TO_TICKS(BOS_AGG_ATTACH_POLL_MS));
            continue;
        }
        esp_openthread_lock_acquire(portMAX_DELAY);
        otDeviceRole role = otThreadGetDeviceRole(instance);
        if (!thread_role_attached(role)) {
            wait_after_attach = true;
            esp_openthread_lock_release();
            vTaskDelay(pdMS_TO_TICKS(BOS_AGG_ATTACH_POLL_MS));
            continue;
        }
        if (wait_after_attach) {
            wait_after_attach = false;
            esp_openthread_lock_release();
            vTaskDelay(pdMS_TO_TICKS(BOS_AGG_FIRST_BROWSE_DELAY_MS));
            continue;
        }
        scan_srp_server_services(instance);
        esp_openthread_lock_release();
        vTaskDelay(pdMS_TO_TICKS(BOS_AGG_BROWSE_PERIOD_MS));
    }
}

static void count_peers(const bos_ledger_active_t *active, bos_convergence_counts_t *counts)
{
    if (!counts) {
        return;
    }
    memset(counts, 0, sizeof(*counts));
    if (active && active->present) {
        counts->target_version = active->version;
        memcpy(counts->target_digest_first_4, active->digest, sizeof(counts->target_digest_first_4));
    }

    uint64_t now = now_ms();
    for (size_t i = 0; i < s_peer_count; i++) {
        const bos_peer_t *peer = &s_peers[i];
        uint64_t age = now > peer->last_seen_ms ? now - peer->last_seen_ms : 0;
        if (age > BOS_AGG_OFFLINE_TIMEOUT_MS) {
            counts->offline++;
            continue;
        }
        if (age > BOS_AGG_STALE_TIMEOUT_MS) {
            counts->stale++;
            continue;
        }
        counts->online++;
        if (peer_is_current(peer, active)) {
            counts->seeders++;
        } else if (peer->has_chunks && peer->chunks_total > 0U && peer->chunks_have < peer->chunks_total) {
            counts->leechers++;
        }
    }
}

static int append_target_json(char **cursor, size_t *remaining, const bos_ledger_active_t *active)
{
    if (active && active->present) {
        char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
        digest_hex(active->digest, digest);
        return appendf(cursor,
                       remaining,
                       "\"target\":{\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                       "\"chunk_size\":%u,\"chunks_total\":%u,\"bytes_total\":%u}",
                       (unsigned)active->version,
                       digest,
                       (unsigned)active->chunk_size,
                       (unsigned)active->chunk_count,
                       (unsigned)active->size_bytes);
    }
    return appendf(cursor,
                   remaining,
                   "\"target\":{\"ledger_version\":null,\"manifest_digest\":null,"
                   "\"chunk_size\":%u,\"chunks_total\":0,\"bytes_total\":0}",
                   (unsigned)BOS_LEDGER_CHUNK_SIZE);
}

static int append_peer_json(char **cursor,
                            size_t *remaining,
                            const bos_peer_t *peer,
                            const bos_ledger_active_t *active,
                            uint64_t now,
                            size_t queue_position)
{
    char address[40] = "";
    if (peer->has_address) {
        ip6_to_hex(peer->address, address);
    }
    const char *state = peer_transfer_state(peer, active, now);
    const char *role = peer_role(peer, active);
    uint32_t chunks_total = peer_chunks_total(peer, active);
    uint32_t chunks_have = peer_chunks_have(peer, active);
    uint32_t progress = peer_progress_pct(peer, active);
    uint32_t ledger_version = peer->has_ledger_version ? peer->ledger_version : 0;
    uint16_t port = peer->tmfs_port != 0U ? peer->tmfs_port : peer->coap_port;

    if (appendf(cursor, remaining, "{") < 0 ||
        appendf(cursor, remaining, "\"id\":") < 0 ||
        append_json_string(cursor, remaining, peer->id) < 0 ||
        appendf(cursor, remaining, ",\"transport_id\":") < 0 ||
        append_json_string(cursor, remaining, peer->id) < 0 ||
        appendf(cursor, remaining, ",\"node_id\":") < 0 ||
        append_json_string(cursor, remaining, peer->id) < 0 ||
        appendf(cursor, remaining, ",\"hardware_id\":") < 0) {
        return -1;
    }
    if (peer->hardware_id[0] != '\0') {
        if (append_json_string(cursor, remaining, peer->hardware_id) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"role\":") < 0 ||
        append_json_string(cursor, remaining, role) < 0 ||
        appendf(cursor, remaining, ",\"state\":") < 0 ||
        append_json_string(cursor, remaining, state) < 0 ||
        appendf(cursor, remaining, ",\"transfer_state\":") < 0 ||
        append_json_string(cursor, remaining, state) < 0 ||
        appendf(cursor, remaining, ",\"ledger_version\":") < 0) {
        return -1;
    }
    if (peer->has_ledger_version) {
        if (appendf(cursor, remaining, "%u", (unsigned)ledger_version) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"manifest_digest\":") < 0) {
        return -1;
    }
    if (peer->has_digest) {
        if (append_json_string(cursor, remaining, peer->digest_hex) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }

    if (appendf(cursor,
                remaining,
                ",\"chunks_have\":%u,\"chunks_total\":%u,\"progress_pct\":%u,"
                "\"rate_bps\":0,\"eta_ms\":null,\"last_seen_ms\":",
                (unsigned)chunks_have,
                (unsigned)chunks_total,
                (unsigned)progress) < 0 ||
        append_u64_decimal(cursor, remaining, peer->last_seen_ms) < 0 ||
        appendf(cursor,
                remaining,
                ",\"queue_position\":%u,\"priority\":\"normal\",\"bytes_total\":%u,"
                "\"service\":{\"mesh\":",
                (unsigned)queue_position,
                active && active->present ? (unsigned)active->size_bytes : 0U) < 0 ||
        append_raw(cursor, remaining, peer->has_mesh ? "true" : "false") < 0 ||
        appendf(cursor, remaining, ",\"tmfs\":") < 0 ||
        append_raw(cursor, remaining, peer->has_tmfs ? "true" : "false") < 0 ||
        appendf(cursor, remaining, ",\"port\":%u},\"address\":", (unsigned)port) < 0 ||
        append_json_string(cursor, remaining, address) < 0 ||
        appendf(cursor, remaining, ",\"model\":") < 0 ||
        append_json_string(cursor, remaining, peer->model) < 0 ||
        appendf(cursor, remaining, ",\"model_id\":") < 0 ||
        append_json_string(cursor, remaining, peer->model) < 0 ||
        appendf(cursor, remaining, ",\"device_class\":") < 0 ||
        append_json_string(cursor, remaining, peer->device_class) < 0 ||
        appendf(cursor, remaining, ",\"thread_address\":") < 0 ||
        append_json_string(cursor, remaining, address) < 0 ||
        appendf(cursor, remaining, ",\"source\":\"srp-dns\"") < 0 ||
        appendf(cursor, remaining, ",\"package_state\":") < 0 ||
        append_json_string(cursor, remaining, peer->package_state) < 0 ||
        appendf(cursor, remaining, ",\"tmfs_catalog\":") < 0 ||
        append_json_string(cursor, remaining, peer->tmfs_catalog) < 0 ||
        appendf(cursor, remaining, ",\"tmfs_caps\":") < 0 ||
        append_json_string(cursor, remaining, peer->tmfs_caps) < 0 ||
        appendf(cursor, remaining, "}") < 0) {
        return -1;
    }
    return 0;
}

esp_err_t bos_convergence_aggregator_init(void)
{
    (void)peer_mutex();
    lock_peers();
    memset(s_peers, 0, sizeof(s_peers));
    s_peer_count = 0;
    unlock_peers();
    ESP_LOGI(TAG, "convergence peer table initialised");
    return ESP_OK;
}

esp_err_t bos_convergence_aggregator_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (!esp_openthread_get_instance()) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t created = xTaskCreate(browse_task, "bos_srp_browse", 6144, NULL, 4, &s_task);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "SRP server scan task started for %s; %s scan deferred during bench recovery", BOS_AGG_SERVICE_TMFS, BOS_AGG_SERVICE_MESH);
    return ESP_OK;
}

int bos_convergence_aggregator_snapshot_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }

    bos_ledger_active_t active = {0};
    (void)bos_ledger_ingress_get_active(&active);
    char *cursor = out;
    size_t remaining = out_len;
    uint64_t now = now_ms();

    if (!try_lock_peers(pdMS_TO_TICKS(50))) {
        return -1;
    }
    bos_convergence_counts_t counts;
    count_peers(&active, &counts);

    int ok = appendf(&cursor, &remaining, "{") >= 0 &&
             append_target_json(&cursor, &remaining, &active) >= 0 &&
             appendf(&cursor,
                     &remaining,
                     ",\"counts\":{\"online\":%u,\"seeders\":%u,\"leechers\":%u,"
                     "\"current\":%u,\"stale\":%u,\"offline\":%u,\"failed\":0},\"peers\":[",
                     counts.online,
                     counts.seeders,
                     counts.leechers,
                     counts.seeders,
                     counts.stale,
                     counts.offline) >= 0;
    for (size_t i = 0; ok && i < s_peer_count; i++) {
        if (i > 0) {
            ok = appendf(&cursor, &remaining, ",") >= 0;
        }
        if (ok) {
            ok = append_peer_json(&cursor, &remaining, &s_peers[i], &active, now, i + 1U) == 0;
        }
    }
    if (ok) {
        ok = appendf(&cursor, &remaining, "],\"source\":\"srp-dns\",\"state\":\"%s\"}", s_started ? "active" : "initialising") >= 0;
    }
    unlock_peers();

    if (!ok) {
        return -1;
    }
    return (int)(out_len - remaining);
}

int bos_convergence_aggregator_peers_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }

    bos_ledger_active_t active = {0};
    (void)bos_ledger_ingress_get_active(&active);
    char *cursor = out;
    size_t remaining = out_len;
    uint64_t now = now_ms();

    if (!try_lock_peers(pdMS_TO_TICKS(50))) {
        return -1;
    }
    bool ok = appendf(&cursor,
                      &remaining,
                      "{\"source\":\"srp-dns\",\"state\":\"%s\",\"peers\":[",
                      s_started ? "active" : "initialising") >= 0;
    for (size_t i = 0; ok && i < s_peer_count; i++) {
        if (i > 0) {
            ok = appendf(&cursor, &remaining, ",") >= 0;
        }
        if (ok) {
            ok = append_peer_json(&cursor, &remaining, &s_peers[i], &active, now, i + 1U) == 0;
        }
    }
    if (ok) {
        ok = appendf(&cursor, &remaining, "],\"note\":\"Local SRP server scan of _tmfs._udp; _mesh._udp scan deferred during bench recovery\"}") >= 0;
    }
    unlock_peers();

    if (!ok) {
        return -1;
    }
    return (int)(out_len - remaining);
}

int bos_convergence_aggregator_torrent_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }

    bos_ledger_active_t active = {0};
    bool active_ok = bos_ledger_ingress_get_active(&active) == ESP_OK && active.present;
    char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1] = "";
    if (active_ok) {
        digest_hex(active.digest, digest);
    }

    char *cursor = out;
    size_t remaining = out_len;
    uint64_t now = now_ms();

    if (!try_lock_peers(pdMS_TO_TICKS(50))) {
        return -1;
    }
    bos_convergence_counts_t counts;
    count_peers(&active, &counts);

    bool ok = appendf(&cursor, &remaining, "{") >= 0 &&
              append_target_json(&cursor, &remaining, &active) >= 0 &&
              appendf(&cursor, &remaining, ",\"self\":{") >= 0 &&
              appendf(&cursor,
                      &remaining,
                      "\"id\":\"border-router\",\"role\":\"seed\",\"state\":\"%s\","
                      "\"ledger_version\":",
                      active_ok ? "seedable" : "waiting") >= 0;
    if (ok && active_ok) {
        ok = appendf(&cursor, &remaining, "%u", (unsigned)active.version) >= 0;
    } else if (ok) {
        ok = appendf(&cursor, &remaining, "null") >= 0;
    }
    if (ok) {
        ok = appendf(&cursor, &remaining, ",\"manifest_digest\":") >= 0;
    }
    if (ok && active_ok) {
        ok = append_json_string(&cursor, &remaining, digest) >= 0;
    } else if (ok) {
        ok = appendf(&cursor, &remaining, "null") >= 0;
    }
    if (ok) {
        ok = appendf(&cursor,
                     &remaining,
                     ",\"chunks_have\":%u,\"chunks_total\":%u,\"progress_pct\":%u,"
                     "\"rate_bps\":0,\"eta_ms\":null,\"last_seen_ms\":0,"
                     "\"last_error\":null,\"bytes_total\":%u},\"peers\":[",
                     active_ok ? (unsigned)active.chunk_count : 0U,
                     active_ok ? (unsigned)active.chunk_count : 0U,
                     active_ok ? 100U : 0U,
                     active_ok ? (unsigned)active.size_bytes : 0U) >= 0;
    }
    for (size_t i = 0; ok && i < s_peer_count; i++) {
        if (i > 0) {
            ok = appendf(&cursor, &remaining, ",") >= 0;
        }
        if (ok) {
            ok = append_peer_json(&cursor, &remaining, &s_peers[i], &active, now, i + 1U) == 0;
        }
    }
    if (ok) {
        ok = appendf(&cursor,
                     &remaining,
                     "],\"counts\":{\"online\":%u,\"seeders\":%u,\"leechers\":%u,"
                     "\"current\":%u,\"stale\":%u,\"offline\":%u,\"failed\":0},"
                     "\"source\":\"br-ledger-srp\",\"state\":\"%s\","
                     "\"note\":\"Peer rows come from local SRP server records; ledger progress appears when TXT exposes lv/ld/have/chunks\"}",
                     counts.online + (active_ok ? 1U : 0U),
                     counts.seeders + (active_ok ? 1U : 0U),
                     counts.leechers,
                     counts.seeders + (active_ok ? 1U : 0U),
                     counts.stale,
                     counts.offline,
                     s_started ? "active" : "initialising") >= 0;
    }
    unlock_peers();

    if (!ok) {
        return -1;
    }
    return (int)(out_len - remaining);
}
