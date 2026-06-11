/**
 * Building OS Border Router: LAN-side HTTP ingress for ledger pushes.
 *
 * Owns:
 *   POST /bos/ledger/push   site server pushes a new ledger artifact
 *   GET  /bos/ledger/active current active version and digest
 *   GET  /bos/convergence   cached mesh convergence aggregation
 *   GET  /bos/status        BR health
 *
 * Implements the ledger lifecycle state machine per
 * docs/08.8-border-router.md sections 6.1, 8.2, 9.1, 9.3.
 * Atomic swap is a single NVS commit on key active_partition.
 *
 * Acceptance vs persistence (p2p-swarm-architecture-2026-06-11): a push is
 * ACCEPTED once validation passes; the ledger serves from RAM immediately.
 * Flash partition + NVS metadata are a replaceable delivery cache written
 * afterwards; cache-write failure returns 200 with persist:"failed" and the
 * BR keeps serving the RAM copy (degraded cache, lost on reboot).
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_LEDGER_NVS_NAMESPACE  "bos_ledger"
#define BOS_LEDGER_KEY_ACTIVE_PART "active_part" /* NVS keys max 15 chars; "active_partition" (16) faulted every metadata commit */
#define BOS_LEDGER_KEY_VERSION     "active_version"
#define BOS_LEDGER_KEY_DIGEST      "active_digest"
#define BOS_LEDGER_KEY_COMMITTED   "committed_at"
#define BOS_LEDGER_KEY_SIZE        "active_size"
#define BOS_LEDGER_KEY_CHUNK_SIZE  "chunk_size"
#define BOS_LEDGER_KEY_CHUNKS      "chunk_count"

#define BOS_LEDGER_PARTITION_A "ledger_a"
#define BOS_LEDGER_PARTITION_B "ledger_b"

#define BOS_LEDGER_DIGEST_LEN 32
#define BOS_LEDGER_CHUNK_SIZE 256

typedef enum {
    BOS_LEDGER_STATE_NONE = 0,
    BOS_LEDGER_STATE_RECEIVING,
    BOS_LEDGER_STATE_VALIDATING,
    BOS_LEDGER_STATE_COMMITTING,
    BOS_LEDGER_STATE_ACTIVE,
} bos_ledger_state_t;

/* Where the serving ledger copy lives. The server is the source of truth;
 * the BR holds a replaceable delivery cache. A failed flash cache write
 * degrades the cache (ram_only) but never fails an accepted push. */
typedef enum {
    BOS_LEDGER_PERSIST_NONE = 0,   /* no ledger copy held */
    BOS_LEDGER_PERSIST_PERSISTED,  /* serving copy is flash-backed (partition + NVS metadata) */
    BOS_LEDGER_PERSIST_RAM_ONLY,   /* cache write failed; serving copy is RAM-only, lost on reboot */
} bos_ledger_persist_t;

typedef struct {
    uint32_t version;
    uint8_t  digest[BOS_LEDGER_DIGEST_LEN];
    uint64_t committed_at;
    uint32_t size_bytes;
    uint32_t chunk_size;
    uint32_t chunk_count;
    uint8_t  active_partition;
    bool     present;
} bos_ledger_active_t;

esp_err_t bos_ledger_ingress_init(void);

// Current ledger lifecycle state.
bos_ledger_state_t bos_ledger_ingress_state(void);

// Cache residency of the serving ledger copy: persisted | ram_only | none.
bos_ledger_persist_t bos_ledger_ingress_persist_state(void);

// String form of bos_ledger_ingress_persist_state() for JSON rendering.
const char *bos_ledger_ingress_persist_state_str(void);

// Renders the last ledger push failure as JSON.
// Returns the snprintf-style byte count, or a negative value on formatting failure.
int bos_ledger_ingress_last_error_json(char *out, size_t out_len);

// Reads the serving ledger metadata. RAM-first: when the cache is degraded
// (ram_only) the accepted RAM copy is reported; otherwise NVS metadata.
// Returns ESP_ERR_NVS_NOT_FOUND if no ledger has been activated.
esp_err_t bos_ledger_ingress_get_active(bos_ledger_active_t *out);

// Protected LAN-side push handler for POST /bos/ledger/push.
esp_err_t bos_ledger_ingress_http_push(httpd_req_t *req);

// Reads bytes from the serving ledger for mesh Block2 serving. RAM-first:
// sources the accepted RAM copy when the cache is degraded, the active
// flash partition otherwise.
esp_err_t bos_ledger_ingress_read_active(size_t offset, uint8_t *out, size_t out_len, size_t *read_len);

#ifdef __cplusplus
}
#endif
