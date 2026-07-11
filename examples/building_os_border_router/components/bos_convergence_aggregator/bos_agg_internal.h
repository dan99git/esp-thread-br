/**
 * Building OS Border Router: mesh convergence aggregator internals.
 *
 * Shared between bos_convergence_aggregator.c (peer-state derivation + JSON
 * rendering), bos_agg_peer_table.c (peer table + TXT parsing + merge),
 * bos_agg_scan.c (SRP server scan task), and bos_agg_json_util.c (append
 * helpers). Not a public component surface.
 */

#pragma once

#include "bos_convergence_aggregator.h"
#include "bos_ledger_ingress.h"
#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_AGG_SERVICE_MESH "_mesh._udp"
#define BOS_AGG_SERVICE_TMFS "_tmfs._udp"
#define BOS_AGG_ATTACH_POLL_MS 1000U
#define BOS_AGG_FIRST_BROWSE_DELAY_MS 10000U
#define BOS_AGG_BROWSE_PERIOD_MS 30000U
#define BOS_AGG_OFFLINE_TIMEOUT_MS (30U * 60U * 1000U)
#define BOS_AGG_HOST_MAX 96U

/* Shared aggregator state. bos_agg_peer_table.c owns the peer table;
 * bos_agg_scan.c owns the started flag. */
extern bos_peer_t bos_agg_peers[BOS_AGG_PEER_TABLE_MAX];
extern size_t     bos_agg_peer_count;
extern bool       bos_agg_started;

/* bos_agg_peer_table.c */
uint64_t now_ms(void);
bool try_lock_peers(TickType_t wait_ticks);
void lock_peers(void);
void unlock_peers(void);
void copy_str(char *dst, size_t dst_len, const char *src);
bool is_self_srp_record(const bos_peer_t *peer);
void parse_txt_into_peer(bos_peer_t *peer, const uint8_t *txt, uint16_t txt_len);
void merge_peer(const bos_peer_t *update);

/* bos_agg_json_util.c */
int appendf(char **cursor, size_t *remaining, const char *fmt, ...);
int append_raw(char **cursor, size_t *remaining, const char *value);
int append_u64_decimal(char **cursor, size_t *remaining, uint64_t value);
int append_json_string(char **cursor, size_t *remaining, const char *value);
void bos_agg_digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1]);
void ip6_to_hex(const uint8_t addr[16], char out[40]);
void ip6_to_plain_hex(const uint8_t addr[16], char out[33]);

/* bos_convergence_aggregator.c: peer-state derivation */
bool digest_matches_target(const bos_peer_t *peer, const bos_ledger_active_t *active);
bool peer_is_current(const bos_peer_t *peer, const bos_ledger_active_t *active);
const char *peer_role(const bos_peer_t *peer, const bos_ledger_active_t *active);
const char *peer_transfer_state(const bos_peer_t *peer, const bos_ledger_active_t *active, uint64_t now);
uint32_t peer_chunks_have(const bos_peer_t *peer, const bos_ledger_active_t *active);
uint32_t peer_chunks_total(const bos_peer_t *peer, const bos_ledger_active_t *active);
uint32_t peer_progress_pct(const bos_peer_t *peer, const bos_ledger_active_t *active);

#ifdef __cplusplus
}
#endif
