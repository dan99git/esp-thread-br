/**
 * Building OS Border Router: mesh convergence aggregator.
 *
 * Subscribes to OpenThread DNS-SD/SRP browse callbacks on _mesh._udp and
 * _tmfs._udp.
 * Maintains an LRU peer table keyed by the SRP/TMFS node label.
 * Recomputes counts on snapshot.
 * Caches a JSON payload for the heartbeat to the site server.
 *
 * Spec: docs/08.8-border-router.md sections 3.3, 6.1 (/bos/convergence), 9.2.
 * JSON shape: locked in docs/scratch/ledger-transport-audit.md section 5.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_AGG_PEER_TABLE_MAX 64
#define BOS_AGG_STALE_TIMEOUT_MS  (5 * 60 * 1000)

typedef enum {
    BOS_PEER_STATE_UNKNOWN = 0,
    BOS_PEER_STATE_SEEDING,
    BOS_PEER_STATE_LEECHING,
    BOS_PEER_STATE_STALE,
    BOS_PEER_STATE_OFFLINE,
} bos_peer_state_t;

typedef struct {
    char     id[64];
    char     hardware_id[64];
    char     mesh_instance[64];
    char     tmfs_instance[64];
    char     host_name[96];
    char     model[48];
    char     model_id[64];
    char     sku[24];
    char     rated_power_w[16];
    char     device_class[24];
    char     package_state[12];
    char     service_version[12];
    char     firmware_version[32];
    char     tmfs_catalog[24];
    char     tmfs_caps[64];
    char     tmfs_wire_payload[12];
    char     role[16];
    char     reported_state[20];
    char     digest_hex[65];
    uint8_t  address[16];
    uint32_t ledger_version;
    uint32_t chunks_have;
    uint32_t chunks_total;
    int      rated_lumens;
    int      cct;
    uint16_t coap_port;
    uint16_t tmfs_port;
    uint16_t http_port;
    bool     has_mesh;
    bool     has_tmfs;
    bool     has_address;
    bool     has_http_port;
    bool     has_commissioned;
    bool     commissioned;
    bool     has_ledger_version;
    bool     has_digest;
    bool     has_chunks;
    bool     has_rated_lumens;
    bool     has_cct;
    bos_peer_state_t state;
    uint64_t last_seen_ms;
} bos_peer_t;

typedef struct {
    uint32_t target_version;
    uint8_t  target_digest_first_4[4];
    uint16_t online;
    uint16_t seeders;
    uint16_t leechers;
    uint16_t stale;
    uint16_t offline;
} bos_convergence_counts_t;

esp_err_t bos_convergence_aggregator_init(void);
esp_err_t bos_convergence_aggregator_start(void);

// Snapshot the current convergence into a caller-supplied JSON buffer.
// Returns bytes written or negative ESP error.
int bos_convergence_aggregator_snapshot_json(char *out, size_t out_len);
int bos_convergence_aggregator_peers_json(char *out, size_t out_len);
int bos_convergence_aggregator_torrent_json(char *out, size_t out_len);

// Renders a compact JSON array of TMFS bootstrap peers for the mesh /mesh/have
// response: fresh peers whose advertised ledger matches the BR active ledger
// with complete chunks. Each row carries ledger_version, manifest_digest,
// chunks_have, chunks_total, address (32 plain hex chars), and
// service{tmfs,port}. Returns bytes written or -1.
int bos_convergence_aggregator_bootstrap_peers_json(char *out, size_t out_len, size_t max_peers);

#ifdef __cplusplus
}
#endif
