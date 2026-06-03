/**
 * Building OS Border Router: mesh-side CoAP serving.
 *
 * Resources on the Thread mesh-local address:
 *   GET /mesh/ledger/manifest   active version, digest, chunk count, chunk size
 *   GET /mesh/ledger/chunk      Block2 chunk N of stored active envelope, 256 bytes per chunk
 *   GET /mesh/have              BR self-state, role=seed
 *
 * BR SRP TXT publishing registers active seed state on _mesh._udp after
 * activation and at boot when active metadata exists.
 *
 * Spec: docs/08.8-border-router.md sections 6.2, 6.3.
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_MESH_CHUNK_SIZE 256

esp_err_t bos_ledger_mesh_serve_init(void);

// Publishes or refreshes active ledger seed TXT after activation.
esp_err_t bos_ledger_mesh_serve_publish_active(uint32_t version, const uint8_t *digest_first_4);

// Renders the BR self torrent snapshot used by /mesh/have.
int bos_ledger_mesh_serve_torrent_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
