/**
 * Building OS Border Router: Thread operational dataset persistence.
 *
 * Persists the Thread operational dataset blob to NVS namespace bos_thread.
 * Survives factory reset of the application config so the mesh stays up and
 * leaves stay attached.
 *
 * Spec: docs/08.8-border-router.md sections 3.4, 7 (NVS table), 11.
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_THREAD_NVS_NAMESPACE "bos_thread"
#define BOS_THREAD_KEY_DATASET   "dataset"
#define BOS_THREAD_DATASET_MAX   254  /* OT operational dataset TLV max */

esp_err_t bos_thread_dataset_anchor_init(void);

// Loads the persisted dataset into the caller's buffer.
// Returns ESP_ERR_NVS_NOT_FOUND if no dataset has been stored.
esp_err_t bos_thread_dataset_anchor_load(uint8_t *out, size_t *inout_len);

// Persists the provided dataset bytes. Called by bos_server_registration
// after the site server hands over the dataset during registration.
esp_err_t bos_thread_dataset_anchor_store(const uint8_t *bytes, size_t len);

#ifdef __cplusplus
}
#endif
