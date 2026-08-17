/**
 * Building OS Border Router: timekeeping.
 *
 * Spec: docs/scratch/phonebook-v2-two-book-spec.md section 5 (locked).
 * Source order:
 *   1. SNTP when an internet route exists.
 *   2. Gateway time when not. The gateway exposes NO time endpoint
 *      (host/runtime/server/src has only /api/health, no /api/time), so the
 *      documented interim source is the HTTP Date header on gateway
 *      responses, fed in via bos_time_note_http_date() from the
 *      bos_server_registration HTTP client.
 *   3. Free-run on the internal clock through dropouts.
 *
 * No battery RTC. Last-known epoch is persisted to NVS periodically so a
 * cold boot resumes approximately; a boot restored only from NVS is
 * UNSYNCED until a real source (SNTP or gateway Date) lands, so a pre-sync
 * discovery stamp cannot masquerade as real time.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_TIME_NVS_NAMESPACE "bos_time"
#define BOS_TIME_NVS_KEY_EPOCH "epoch"

/* ISO-8601 UTC "2026-07-02T03:04:05Z" = 20 chars + NUL. */
#define BOS_TIME_ISO8601_LEN 21

// Restores last-known time from NVS (marked unsynced), registers the IP
// event hook that starts SNTP once a backbone route exists, and starts the
// periodic NVS persist timer.
esp_err_t bos_time_init(void);

// Feed an RFC 7231 HTTP Date header value (e.g. from a gateway response).
// Applied only while SNTP has not synced this boot; SNTP always outranks.
void bos_time_note_http_date(const char *http_date);

// True once a real source (SNTP or gateway Date) has set the clock this
// boot. False when free-running from an NVS restore or from epoch zero.
bool bos_time_is_synced(void);

// "sntp" | "gateway" | "nvs-restore" | "none".
const char *bos_time_source(void);

// Formats current UTC time as ISO-8601 into out. Returns chars written or
// -1 on error/too-small buffer. Renders whatever the clock holds, synced or
// not; pair with bos_time_is_synced() for the marker.
int bos_time_now_iso8601(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
