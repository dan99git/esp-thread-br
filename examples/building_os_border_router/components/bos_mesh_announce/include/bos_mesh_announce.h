/**
 * Building OS Border Router: mesh-wide artifact-availability announce (nudge).
 *
 * When the BR receives a NEW artifact version (a fresh phonebook via
 * POST /bos/phonebook, or a fresh ledger via POST /bos/ledger/push) it fires a
 * one-shot mesh-wide NUDGE so the pull machinery on already-running nodes wakes
 * immediately instead of waiting for its next poll cadence.
 *
 * Wire: NON-confirmable CoAP POST to coap://[ff03::1]/mesh/announce
 * (realm-local all-nodes), content-format JSON, body:
 *   {"artifact_class":"phonebook"|"ledger","version":N,"digest":"<8-hex-prefix>"}
 *
 * It is a nudge ONLY. The BR has no push authority: each node still
 * version-compares and pulls the artifact through its existing manifest/chunk
 * path (mesh/<class>/manifest + mesh/<class>/chunk). A node that missed the
 * announce still self-heals via its boot-time neighbour compare.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fire the mesh-wide availability nudge for one artifact class.
 *
 * @param artifact_class  "phonebook" or "ledger" (must match the node's
 *                        /mesh/have artifact_class + manifest/chunk paths).
 * @param version         the newly-available version number.
 * @param digest_prefix8  8-char (4-byte) lowercase hex prefix of the artifact
 *                        digest, so a node can distinguish content at the same
 *                        version before pulling the full manifest.
 * @return ESP_OK on a queued send; a visible error otherwise (OpenThread not
 *         running, lock timeout, no buffers). Never falls back silently.
 */
esp_err_t bos_mesh_announce_fire(const char *artifact_class, uint32_t version, const char *digest_prefix8);

#ifdef __cplusplus
}
#endif
