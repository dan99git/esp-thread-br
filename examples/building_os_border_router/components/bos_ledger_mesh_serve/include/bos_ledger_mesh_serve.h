/**
 * Building OS Border Router: mesh-side CoAP serving.
 *
 * Resources on the Thread mesh-local address:
 *   GET /mesh/ledger/manifest   active version, digest, chunk count, chunk size
 *   GET /mesh/ledger/chunk      Block2 chunk N of stored active envelope, 256 bytes per chunk
 *   GET /mesh/have              active ledger target plus compact TMFS device
 *                               peer bootstrap rows for device-to-device fetch,
 *                               and a phonebook availability target (version,
 *                               digest, bytes) so a polling node learns a newer
 *                               BR phonebook exists and wakes its own pull.
 *
 * A fresh ledger deploy or phonebook ingest also fires a mesh-wide nudge
 * (bos_mesh_announce, coap://[ff03::1]/mesh/announce) so nodes pull now.
 *
 * BR SRP TXT publishing registers active seed state on _mesh._udp after
 * activation and at boot when active metadata exists.
 *
 * Spec: docs/08.8-border-router.md sections 6.2, 6.3.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_MESH_CHUNK_SIZE 256

esp_err_t bos_ledger_mesh_serve_init(void);

// Publishes or refreshes active ledger seed TXT after activation.
esp_err_t bos_ledger_mesh_serve_publish_active(uint32_t version, const uint8_t *digest_first_4);

// Renders the BR-only self torrent snapshot (no SRP peer rows). /mesh/have now
// serves the compact bootstrap shape; this renderer remains for LAN/UI callers.
int bos_ledger_mesh_serve_torrent_json(char *out, size_t out_len);

/**
 * Refreshes the ledger SRP advertisement for a freshly-deployed ledger and
 * fires a mesh-wide availability nudge (coap://[ff03::1]/mesh/announce,
 * artifact_class "ledger") so live nodes poll /mesh/have and pull now instead
 * of on their next backoff cycle. The nudge carries no authority: each node
 * still version-compares and pulls through the existing manifest/chunk path.
 * Call after a successful POST /bos/ledger/push installs a new active ledger.
 */
esp_err_t bos_ledger_mesh_serve_announce_ledger(uint32_t version, const uint8_t *digest_first_4);

/**
 * Records the active operational PHONEBOOK version/digest/size so the
 * GET /mesh/have "phonebook" target reports it to polling nodes, and (when
 * @p announce is true) fires the mesh-wide availability nudge
 * (coap://[ff03::1]/mesh/announce, artifact_class "phonebook").
 *
 * The BR is NOT the phonebook byte seed: nodes leech the document over TMFS
 * from a peer. The existing PUSH seeds the first nodes and node-to-node gossip
 * spreads it viral; this call only advertises that a newer version exists and
 * nudges the pull. It is additive to the PUSH, which is preserved.
 *
 * @param raw          the operational phonebook document bytes (unused for
 *                     seeding; retained so a future BR seed can serve them).
 * @param len          document length in bytes (used as bytes_total).
 * @param version_str  decimal version string from the document header.
 * @param digest_hex64 64-char lowercase sha256 hex of the document.
 * @param announce     true on a fresh /bos/phonebook ingest (fire the nudge);
 *                     false on boot re-seed from NVS (advertise target only).
 * @return ESP_OK once the target is recorded; the announce error if the nudge
 *         was requested and failed (visible, never silent).
 */
esp_err_t bos_ledger_mesh_serve_publish_phonebook(const char *raw,
                                                  size_t len,
                                                  const char *version_str,
                                                  const char *digest_hex64,
                                                  bool announce);

#ifdef __cplusplus
}
#endif
