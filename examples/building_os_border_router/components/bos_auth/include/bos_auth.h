/**
 * Building OS Border Router: the ONE HTTP write-auth trust anchor.
 *
 * A token-authed BR write is authorized by either:
 *   - X-Device-Token matching the BR's persisted device_token
 *     (bos_server_registration_get_token), or
 *   - x-api-key matching the compile-time CONFIG_BOS_SERVER_API_KEY.
 *
 * This existed as three verbatim copies (bos_ledger_ingress push,
 * bos_br_ota, bos_commissioning), each using plain strcmp - a CWE-208
 * timing side channel on the token compare. Deduped 2026-08-24 with a
 * constant-time comparison (length can still be inferred; content cannot).
 */
#pragma once

#include <stdbool.h>

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* True when the request carries a valid device token or API key. */
bool bos_auth_request_authorized(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
