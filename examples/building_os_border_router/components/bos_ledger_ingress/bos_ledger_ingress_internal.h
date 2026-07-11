/**
 * Building OS Border Router: ledger ingress internals.
 *
 * Shared between bos_ledger_ingress.c (state, NVS/partition metadata, RAM
 * store), bos_ledger_ingress_cbor.c (envelope parsing/validation), and
 * bos_ledger_ingress_push.c (protected push handler). Not a public
 * component surface.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bos_ledger_ingress.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_LEDGER_PUSH_MAX_BYTES (64U * 1024U)
#define BOS_LEDGER_HEADER_TOKEN_MAX 128

typedef struct {
    const uint8_t *body;
    size_t body_len;
    const uint8_t *manifest_digest;
    uint32_t ledger_version;
} bos_ledger_body_view_t;

/* bos_ledger_ingress.c */
void bos_ledger_ingress_set_state(bos_ledger_state_t state);
void clear_last_error(void);
void bos_ledger_set_last_error(const char *phase,
                               esp_err_t code,
                               const char *message,
                               uint32_t size_bytes,
                               const char *partition);
void bos_ledger_digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1]);
esp_err_t read_partition_idx(uint8_t *idx);
const esp_partition_t *partition_for_idx(uint8_t idx);
esp_err_t commit_active_metadata(uint8_t partition_idx,
                                 uint32_t version,
                                 const uint8_t digest[BOS_LEDGER_DIGEST_LEN],
                                 uint32_t size_bytes);
void ram_ledger_set(uint8_t *body, size_t len, const bos_ledger_body_view_t *view);
void ram_ledger_clear(void);

/* bos_ledger_ingress_cbor.c */
esp_err_t validate_ledger_envelope(const uint8_t *bytes,
                                   size_t len,
                                   bos_ledger_body_view_t *view);

/* bos_ledger_ingress_push.c */
bool push_guard_acquire(const char *initial_phase);
void push_guard_set_phase(const char *phase);
void push_guard_release(void);
esp_err_t push_send_conflict(httpd_req_t *req);
esp_err_t write_staged_partition(const esp_partition_t *partition,
                                 const uint8_t *bytes,
                                 size_t len);

#ifdef __cplusplus
}
#endif
