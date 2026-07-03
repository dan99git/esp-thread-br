/**
 * Building OS Border Router: floating book (BR-authored touch log).
 *
 * Spec: docs/scratch/phonebook-v2-two-book-spec.md sections 2.1, 3, 4.
 *
 * A row is appended when an unpaired BOS device SRP-registers on the BR
 * (hook: bos_convergence_aggregator merge path, cm TXT not "1"). Rows use
 * the v3 17-column schema; spatial columns (x,y,z,spatial_id) and tags are
 * blank while floating (photometric columns rated_power_w/rated_lumens/cct
 * ARE populated from the device SRP broadcast). Sole writer: BR. Read-only to the
 * gateway via GET /bos/phonebook/floating. NEVER mesh-distributed.
 *
 * Persistence: NVS namespace "bos_float", single blob, capped ring
 * (oldest floating row drops first). Sizing (spec open item 7.1, decided
 * here): BR NVS partition is 0x6000 = 24 KB (partitions.csv). Worst-case
 * serialized row is ~340 bytes; cap 16 rows => <= ~5.5 KB blob worst case,
 * which coexists with the operational-book blob (realistic <= ~8 KB) and
 * the small bos_reg/bos_time entries inside the ~20 KB of usable NVS pages.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_FLOATING_MAX_ROWS 16
#define BOS_FLOATING_NVS_NAMESPACE "bos_float"
#define BOS_FLOATING_NVS_KEY "rows"

/* Render buffer that always fits a full book (header + 16 worst-case rows). */
#define BOS_FLOATING_RENDER_MAX 8192

// One SRP touch. eui64 is required (16 lowercase hex). model_id is the raw
// SRP TXT mid ("make::model"; split happens inside; no "::" => make blank).
// Every other field may be NULL/empty. eid must be the mesh-local ML-EID
// string, never the OMR (spec column 9).
typedef struct {
    const char *eui64;
    const char *device_id;
    const char *device_class;
    const char *model_id;
    const char *fw;
    const char *eid;
    const char *sku;           /* manufacturer stock code; "" when absent */
    const char *rated_power_w; /* watts, string form ("31"); "" when absent */
    const char *rated_lumens;  /* integer string; "" when the card omits it */
    const char *cct;           /* kelvin integer string; "" when absent */
} bos_floating_touch_t;

// Creates the lock and loads the persisted ring from NVS.
esp_err_t bos_floating_book_init(void);

// Records/updates a touch. First touch stamps discovered_at from the BR
// clock with a |synced / |unsynced marker (bos_time); later touches keep the
// original stamp and refresh the mutable fields. RAM-only and cheap: safe to
// call under the OpenThread lock. Returns ESP_ERR_INVALID_ARG on a missing/
// malformed eui64.
esp_err_t bos_floating_book_touch(const bos_floating_touch_t *touch);

// Persists the ring to NVS when it changed since the last persist. Call OFF
// the OpenThread lock (NVS commits block).
void bos_floating_book_persist_if_dirty(void);

// Renders the full 17-column v3 CSV document (comment header + column
// header + rows). Returns bytes written or -1.
int bos_floating_book_render_csv(char *out, size_t out_len);

size_t bos_floating_book_row_count(void);

#ifdef __cplusplus
}
#endif
