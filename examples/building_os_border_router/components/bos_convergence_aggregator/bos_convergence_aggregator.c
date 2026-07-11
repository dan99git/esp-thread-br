/**
 * Building OS Border Router: mesh convergence aggregator.
 *
 * Peer-state derivation and the public JSON snapshot renderers. The peer
 * table lives in bos_agg_peer_table.c, the SRP scan task in bos_agg_scan.c,
 * and the append helpers in bos_agg_json_util.c.
 */

#include "bos_convergence_aggregator.h"

#include "bos_agg_internal.h"
#include "bos_ledger_ingress.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

bool digest_matches_target(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (!peer || !active || !active->present || !peer->has_digest) {
        return false;
    }
    char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
    bos_agg_digest_hex(active->digest, digest);
    size_t peer_len = strlen(peer->digest_hex);
    return peer_len > 0U && peer_len <= BOS_LEDGER_DIGEST_LEN * 2U &&
           strncmp(peer->digest_hex, digest, peer_len) == 0;
}

bool peer_is_current(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    return peer && active && active->present && peer->has_ledger_version &&
           peer->ledger_version == active->version && digest_matches_target(peer, active);
}

const char *peer_role(const bos_peer_t *peer, const bos_ledger_active_t *active)
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

const char *peer_transfer_state(const bos_peer_t *peer, const bos_ledger_active_t *active, uint64_t now)
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

uint32_t peer_chunks_have(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (peer_is_current(peer, active)) {
        return active->chunk_count;
    }
    if (peer && peer->has_chunks) {
        return peer->chunks_have;
    }
    return 0;
}

uint32_t peer_chunks_total(const bos_peer_t *peer, const bos_ledger_active_t *active)
{
    if (peer && peer->has_chunks && peer->chunks_total > 0U) {
        return peer->chunks_total;
    }
    return active && active->present ? active->chunk_count : 0;
}

uint32_t peer_progress_pct(const bos_peer_t *peer, const bos_ledger_active_t *active)
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
    for (size_t i = 0; i < bos_agg_peer_count; i++) {
        const bos_peer_t *peer = &bos_agg_peers[i];
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
        bos_agg_digest_hex(active->digest, digest);
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
        append_json_string(cursor, remaining, peer->model_id[0] != '\0' ? peer->model_id : peer->model) < 0 ||
        appendf(cursor, remaining, ",\"sku\":") < 0) {
        return -1;
    }
    /* sku (phonebook v3 col 7): manufacturer stock code; absent => null. */
    if (peer->sku[0] != '\0') {
        if (append_json_string(cursor, remaining, peer->sku) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"rated_power_w\":") < 0) {
        return -1;
    }
    /* v3 photometric properties the gateway phonebook-publisher reads by these
     * exact names. rated_power_w is a string (watts); absent => null. */
    if (peer->rated_power_w[0] != '\0') {
        if (append_json_string(cursor, remaining, peer->rated_power_w) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"rated_lumens\":") < 0) {
        return -1;
    }
    if (peer->has_rated_lumens) {
        if (appendf(cursor, remaining, "%d", peer->rated_lumens) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"cct\":") < 0) {
        return -1;
    }
    if (peer->has_cct) {
        if (appendf(cursor, remaining, "%d", peer->cct) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"fw\":") < 0 ||
        append_json_string(cursor, remaining, peer->firmware_version) < 0 ||
        appendf(cursor, remaining, ",\"http\":") < 0) {
        return -1;
    }
    if (peer->has_http_port) {
        if (appendf(cursor, remaining, "%u", (unsigned)peer->http_port) < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"commissioned\":") < 0) {
        return -1;
    }
    if (peer->has_commissioned) {
        if (append_raw(cursor, remaining, peer->commissioned ? "true" : "false") < 0) {
            return -1;
        }
    } else if (appendf(cursor, remaining, "null") < 0) {
        return -1;
    }
    if (appendf(cursor, remaining, ",\"device_class\":") < 0 ||
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
    for (size_t i = 0; ok && i < bos_agg_peer_count; i++) {
        if (i > 0) {
            ok = appendf(&cursor, &remaining, ",") >= 0;
        }
        if (ok) {
            ok = append_peer_json(&cursor, &remaining, &bos_agg_peers[i], &active, now, i + 1U) == 0;
        }
    }
    if (ok) {
        ok = appendf(&cursor, &remaining, "],\"source\":\"srp-dns\",\"state\":\"%s\"}", bos_agg_started ? "active" : "initialising") >= 0;
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
                      bos_agg_started ? "active" : "initialising") >= 0;
    for (size_t i = 0; ok && i < bos_agg_peer_count; i++) {
        if (i > 0) {
            ok = appendf(&cursor, &remaining, ",") >= 0;
        }
        if (ok) {
            ok = append_peer_json(&cursor, &remaining, &bos_agg_peers[i], &active, now, i + 1U) == 0;
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
        bos_agg_digest_hex(active.digest, digest);
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
    for (size_t i = 0; ok && i < bos_agg_peer_count; i++) {
        if (i > 0) {
            ok = appendf(&cursor, &remaining, ",") >= 0;
        }
        if (ok) {
            ok = append_peer_json(&cursor, &remaining, &bos_agg_peers[i], &active, now, i + 1U) == 0;
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
                     bos_agg_started ? "active" : "initialising") >= 0;
    }
    unlock_peers();

    if (!ok) {
        return -1;
    }
    return (int)(out_len - remaining);
}

int bos_convergence_aggregator_bootstrap_peers_json(char *out, size_t out_len, size_t max_peers)
{
    if (!out || out_len == 0) {
        return -1;
    }

    bos_ledger_active_t active = {0};
    bool active_ok = bos_ledger_ingress_get_active(&active) == ESP_OK && active.present;

    char *cursor = out;
    size_t remaining = out_len;
    uint64_t now = now_ms();

    if (!try_lock_peers(pdMS_TO_TICKS(50))) {
        return -1;
    }
    bool ok = append_raw(&cursor, &remaining, "[") >= 0;
    size_t emitted = 0;
    for (size_t i = 0; ok && active_ok && i < bos_agg_peer_count && emitted < max_peers; i++) {
        const bos_peer_t *peer = &bos_agg_peers[i];
        uint64_t age = now > peer->last_seen_ms ? now - peer->last_seen_ms : 0;
        if (age > BOS_AGG_STALE_TIMEOUT_MS ||
            !peer->has_tmfs || !peer->has_address || peer->tmfs_port == 0U ||
            !peer->has_chunks || peer->chunks_total == 0U ||
            peer->chunks_have < peer->chunks_total ||
            !peer_is_current(peer, &active)) {
            continue;
        }
        char address[33];
        ip6_to_plain_hex(peer->address, address);
        if (emitted > 0U) {
            ok = append_raw(&cursor, &remaining, ",") >= 0;
        }
        if (ok) {
            ok = appendf(&cursor,
                         &remaining,
                         "{\"ledger_version\":%u,\"manifest_digest\":",
                         (unsigned)peer->ledger_version) >= 0 &&
                 append_json_string(&cursor, &remaining, peer->digest_hex) >= 0 &&
                 appendf(&cursor,
                         &remaining,
                         ",\"chunks_have\":%u,\"chunks_total\":%u,\"address\":\"%s\","
                         "\"service\":{\"tmfs\":true,\"port\":%u}}",
                         (unsigned)peer->chunks_have,
                         (unsigned)peer->chunks_total,
                         address,
                         (unsigned)peer->tmfs_port) >= 0;
        }
        if (ok) {
            emitted++;
        }
    }
    if (ok) {
        ok = append_raw(&cursor, &remaining, "]") >= 0;
    }
    unlock_peers();

    if (!ok) {
        return -1;
    }
    return (int)(out_len - remaining);
}
