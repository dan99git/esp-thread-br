/**
 * Building OS Border Router: LAN-direct commissioning identity.
 *
 * Owns: the device-side identity write for BR commissioning. The site server
 * imprint flow POSTs the commissioned identity to the BR over its existing
 * port-80 LAN surface; this component validates the request against the BR's
 * own persisted device_token (same trust anchor as /bos/ledger/push), parses
 * the spatial identity, and persists it in NVS namespace "bos_identity".
 *
 * BRs are LAN devices: no mesh discovery, no identify-blink, no pairing
 * ceremony. This is the LAN-direct variant of the mesh-device /api/commission
 * contract (same persisted fields, transport-appropriate path).
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace and keys. NVS limits both namespace and key names to 15
 * characters; every name below is <= 15 (the 16-char "active_partition" key
 * cost a day of ESP_ERR_NVS_KEY_TOO_LONG on every ledger push). */
#define BOS_IDENTITY_NVS_NAMESPACE "bos_identity"  /* 12 chars */
#define BOS_IDENTITY_KEY_SPATIAL   "spatial_id"    /* 10 chars */
#define BOS_IDENTITY_KEY_SITE      "site_id"       /*  7 chars */
#define BOS_IDENTITY_KEY_BUILDING  "building"      /*  8 chars */
#define BOS_IDENTITY_KEY_LEVEL     "level"         /*  5 chars */
#define BOS_IDENTITY_KEY_SPACE     "space"         /*  5 chars */
#define BOS_IDENTITY_KEY_DEV_INDEX "device_index"  /* 12 chars */
#define BOS_IDENTITY_KEY_CLASS     "device_class"  /* 12 chars */
#define BOS_IDENTITY_KEY_TAGS      "tags_csv"      /*  8 chars */

/* Field sizes mirror the mesh-device identity record
 * (firmware/xiao-esp32c6/components/sdcard/device_config.h). */
typedef struct {
    bool commissioned;
    char spatial_id[32];
    char site_id[32];
    char building[32];
    char level[32];
    char space[64];
    char device_index[32];
    char device_class[32];
    char tags_csv[256];
} bos_commissioning_identity_t;

/**
 * Shared write-route auth check: X-Device-Token must match the BR's own
 * persisted device_token (bos_server_registration_get_token), or x-api-key
 * must match the compile-time CONFIG_BOS_SERVER_API_KEY. Used by
 * /bos/commission and the /bos/joiner routes (bos_joiner.c).
 */
bool bos_commissioning_request_authorized(httpd_req_t *req);

/**
 * POST handler for /bos/commission (and the /api/commission alias).
 *
 * Auth: X-Device-Token header must match the BR's own persisted device_token
 * (bos_server_registration_get_token); CONFIG_BOS_SERVER_API_KEY accepted as
 * the same compile-time alternative /bos/ledger/push honors.
 *
 * Body: JSON {spatial_id, device_class, tags[]} (the mesh-device
 * /api/commission contract). site_id/building/level/space/device_index are
 * derived from spatial_id; if the body also carries them they must match the
 * derived values or the request is rejected.
 *
 * Error responses send an honest JSON body and return ESP_OK so the socket
 * closes gracefully; ESP_FAIL only on socket-level receive failure.
 */
esp_err_t bos_commissioning_http_post(httpd_req_t *req);

/**
 * Loads the persisted identity. Returns ESP_OK with out->commissioned=false
 * when nothing is persisted; non-ESP_OK only on NVS access failure.
 */
esp_err_t bos_commissioning_get_identity(bos_commissioning_identity_t *out);

/**
 * Renders the /bos/status "identity" value into out: "null" when not
 * commissioned, otherwise a JSON object with the persisted fields.
 * Returns the number of bytes written, or -1 on truncation/failure.
 */
int bos_commissioning_identity_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
