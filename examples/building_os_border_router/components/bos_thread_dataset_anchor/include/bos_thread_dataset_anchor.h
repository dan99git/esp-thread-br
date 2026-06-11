/**
 * Building OS Border Router: Thread operational dataset persistence and
 * network formation.
 *
 * Persists the Thread operational dataset blob to NVS namespace bos_thread.
 * Survives factory reset of the application config so the mesh stays up and
 * leaves stay attached.
 *
 * bos_thread_dataset_anchor_apply_or_form() is the boot lifecycle hook: it
 * applies the persisted dataset when one exists and forms a new network when
 * none does, so the BR always brings the mesh up and the dataset survives
 * reboot. This is the dataset half of the docs/06.2-commissioning-workflow.md
 * phase 2b model (the BR anchors the Operational Dataset that the
 * commissioner pushes to accepted joiners).
 *
 * Spec: docs/08.8-border-router.md sections 3.4, 7 (NVS table), 11.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVS namespace and key are both <= 15 chars (NVS name limit). */
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

/**
 * Applies the persisted Operational Dataset, or forms a new network when no
 * dataset exists anywhere, then enables the Thread interface.
 *
 * Resolution order (documented in the implementation):
 *   1. OpenThread's own settings store (the live copy once the stack has run)
 *   2. the NVS anchor (recovery when OT settings were wiped)
 *   3. formation of a new network (first boot)
 * Every path leaves the anchor in sync with the active dataset.
 *
 * Must be called with the OpenThread lock held (esp_openthread_lock_acquire)
 * after esp_openthread_border_router_init().
 */
esp_err_t bos_thread_dataset_anchor_apply_or_form(void);

/** Snapshot of the live Thread runtime for /bos/status. Never carries the
 *  network key. */
typedef struct {
    char role[16];          /* otThreadDeviceRoleToString: disabled/detached/child/router/leader */
    char network_name[17];  /* OT_NETWORK_NAME_MAX_SIZE + 1 */
    uint16_t panid;
    uint8_t channel;
    uint16_t rloc16;
} bos_thread_runtime_status_t;

/**
 * Fills out from the live OpenThread instance.
 * Returns ESP_ERR_INVALID_STATE when OpenThread is not initialized and
 * ESP_ERR_TIMEOUT when the OpenThread lock cannot be taken; callers render
 * those as an honest null, never as fabricated values.
 */
esp_err_t bos_thread_runtime_status(bos_thread_runtime_status_t *out);

#ifdef __cplusplus
}
#endif
