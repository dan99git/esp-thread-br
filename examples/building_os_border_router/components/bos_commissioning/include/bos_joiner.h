/**
 * Building OS Border Router: Thread joiner acceptance (commissioning
 * phase 2b).
 *
 * Implements the operator-driven joiner-acceptance model of
 * docs/06.2-commissioning-workflow.md phase 2b: the operator (site server /
 * TUI) supplies the device's EUI-64 and per-device PSKd, the BR runs the
 * OpenThread Commissioner role and completes the Thread Commissioning
 * handshake, and the dataset anchored by bos_thread_dataset_anchor is pushed
 * to the joiner by the standard MeshCoP exchange. There is no
 * accept-any-joiner path.
 *
 * This is the reconciliation that docs/08.8-border-router.md section 11
 * requires before code accepts joiners: the upstream web UI's raw
 * scan/join/form controls are NOT used; acceptance happens only through the
 * token-authed routes below, one explicit EUI-64 + PSKd at a time.
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * POST /bos/joiner/accept
 *
 * Auth: identical boundary to /bos/commission
 * (bos_commissioning_request_authorized).
 *
 * Body: JSON {"eui64": "<16 hex chars, ':'/'-' separators allowed>",
 *             "pskd": "<6..32 chars, Thread PSKd alphabet>",
 *             "timeout_s": <optional, default 300, max 3600>}
 *
 * Starts the OpenThread Commissioner role when it is not already active and
 * adds the joiner entry. When the commissioner is still petitioning, the
 * joiner is queued and added from the commissioner state callback once the
 * petition succeeds (response says "queued" instead of "added").
 *
 * Error responses send an honest JSON body and return ESP_OK so the socket
 * closes gracefully; ESP_FAIL only on socket-level receive failure.
 */
esp_err_t bos_joiner_accept_http_post(httpd_req_t *req);

/**
 * GET /bos/joiner/status
 *
 * Commissioner state plus the active joiner entries from
 * otCommissionerGetNextJoinerInfo and any locally queued entries waiting for
 * the petition to complete. PSKd values are never included.
 */
esp_err_t bos_joiner_status_http_get(httpd_req_t *req);

/**
 * Renders the commissioner snapshot for /bos/thread-diag:
 * {"state":"<disabled|petitioning|active>","joiners":[...]} with the live
 * otCommissionerGetNextJoinerInfo table (PSKd values never included).
 *
 * Caller must hold the OpenThread lock and have verified
 * otInstanceIsInitialized. Returns bytes written, or -1 on truncation.
 */
int bos_joiner_commissioner_json(char *out, size_t out_len);

/**
 * Renders the commissioner joiner event trail for /bos/thread-diag:
 * {"total":<events since boot>,"events":[{"uptime_ms":..,"event":"start",
 * "eui64":".."}, ...]} holding the last 16 joiner-callback events
 * (start/connected/finalize/end/removed) with monotonic esp_timer
 * timestamps, oldest first.
 *
 * Caller must hold the OpenThread lock (the ring is written from the
 * commissioner joiner callback in the OpenThread task with the lock held).
 * Returns bytes written, or -1 on truncation.
 */
int bos_joiner_event_trail_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
