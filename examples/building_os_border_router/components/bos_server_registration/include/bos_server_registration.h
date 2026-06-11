/**
 * Building OS Border Router: site server registration and heartbeat.
 *
 * Owns: site server discovery (mDNS browse for _bos-server._tcp.local with
 * NVS fallback URL), first-boot registration as a border_router class device,
 * heartbeat loop, persistence of device_token in NVS namespace bos_reg.
 *
 * Spec: docs/08.8-border-router.md sections 8.1, 10, 11.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_REG_NVS_NAMESPACE "bos_reg"
#define BOS_REG_KEY_DEVICE_ID  "device_id"
#define BOS_REG_KEY_TOKEN      "device_token"
#define BOS_REG_KEY_SERVER_URL "server_url"

#define BOS_REG_HEARTBEAT_PERIOD_S 60

// Heartbeats buffered in RAM while the site server is unreachable
// (docs/08.8-border-router.md s12: capped at 60 entries, drops oldest).
#define BOS_REG_HEARTBEAT_BUFFER_CAP 60

esp_err_t bos_server_registration_init(void);

// True if a device_token is persisted in NVS.
bool bos_server_registration_is_registered(void);

// Copies the persisted device_token into the caller's buffer.
// Returns ESP_ERR_NVS_NOT_FOUND if not registered.
esp_err_t bos_server_registration_get_token(char *out, size_t out_len);

// Copies the BR device id used for site-server claim/status/convergence.
esp_err_t bos_server_registration_get_device_id(char *out, size_t out_len);

// Copies the configured BOS site-server URL.
esp_err_t bos_server_registration_get_server_url(char *out, size_t out_len);

// Heartbeat buffer counters: entries currently held in the RAM ring and
// total entries dropped to overflow since boot. Either out pointer may be
// NULL. Safe to call from any task.
void bos_server_registration_heartbeat_stats(uint32_t *buffered_count, uint32_t *dropped_count);

#ifdef __cplusplus
}
#endif
