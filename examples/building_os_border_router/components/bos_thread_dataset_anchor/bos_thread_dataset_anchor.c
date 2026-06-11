/**
 * Building OS Border Router: Thread dataset anchor.
 * Real NVS read and write, plus the boot lifecycle that hands the dataset to
 * OpenThread (apply persisted dataset, or form a new network when none
 * exists).
 */

#include "bos_thread_dataset_anchor.h"

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/* OT API citations (ESP-IDF bundled OpenThread headers,
 * components/openthread/openthread/include/openthread/):
 *   otDatasetGetActiveTlvs      dataset.h:350
 *   otDatasetSetActiveTlvs      dataset.h:402 (via esp_openthread_auto_start)
 *   otDatasetConvertToTlvs      dataset.h:579
 *   otDatasetCreateNewNetwork   dataset_ftd.h:60
 *   otThreadGetDeviceRole       thread.h:747
 *   otThreadDeviceRoleToString  thread.h:756
 *   otThreadGetNetworkName      thread.h:571
 *   otThreadGetRloc16           thread.h:803
 *   otLinkGetChannel            link.h:453
 *   otLinkGetPanId              link.h:534
 * esp_openthread_auto_start: esp_openthread.h:47 (sets the active dataset
 * TLVs, then brings the interface and Thread protocol up). */
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/link.h"
#include "openthread/thread.h"

static const char *TAG = "bos_dataset";

esp_err_t bos_thread_dataset_anchor_init(void)
{
    size_t len = 0;
    esp_err_t err = bos_thread_dataset_anchor_load(NULL, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Thread dataset persisted, %u bytes", (unsigned)len);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no persisted Thread dataset; first boot path");
    } else {
        ESP_LOGW(TAG, "dataset load probe returned 0x%x", err);
    }
    return ESP_OK;
}

esp_err_t bos_thread_dataset_anchor_load(uint8_t *out, size_t *inout_len)
{
    if (!inout_len) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_THREAD_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(h, BOS_THREAD_KEY_DATASET, out, inout_len);
    nvs_close(h);
    return err;
}

esp_err_t bos_thread_dataset_anchor_store(const uint8_t *bytes, size_t len)
{
    if (!bytes || len == 0 || len > BOS_THREAD_DATASET_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_THREAD_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, BOS_THREAD_KEY_DATASET, bytes, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Re-persists the active dataset into the anchor when the anchor is missing
 * or stale. OT's own settings store is the live copy; the anchor mirrors it. */
static void anchor_sync_from_tlvs(const otOperationalDatasetTlvs *tlvs)
{
    uint8_t anchored[BOS_THREAD_DATASET_MAX];
    size_t anchored_len = sizeof(anchored);
    esp_err_t load_err = bos_thread_dataset_anchor_load(anchored, &anchored_len);

    if (load_err == ESP_OK && anchored_len == tlvs->mLength &&
        memcmp(anchored, tlvs->mTlvs, tlvs->mLength) == 0) {
        return; /* anchor already current */
    }

    esp_err_t store_err = bos_thread_dataset_anchor_store(tlvs->mTlvs, tlvs->mLength);
    if (store_err != ESP_OK) {
        ESP_LOGE(TAG, "failed to sync dataset anchor: %s", esp_err_to_name(store_err));
    } else {
        ESP_LOGI(TAG, "dataset anchor synced (%u bytes)", (unsigned)tlvs->mLength);
    }
}

/* Boot lifecycle. Reconciles the upstream BR web UI's raw scan/join/form
 * controls with the documented commissioning model: per
 * docs/06.2-commissioning-workflow.md phase 2b and docs/08.8-border-router.md
 * section 11, the BR (not an operator-driven web form) anchors the Thread
 * Operational Dataset. The BR forms the network once, persists the dataset,
 * and reloads it on every subsequent boot; joiner acceptance stays
 * operator-driven through /bos/joiner/accept (bos_joiner.c). */
esp_err_t bos_thread_dataset_anchor_apply_or_form(void)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        return ESP_ERR_INVALID_STATE;
    }

    otOperationalDatasetTlvs tlvs;
    bool have_dataset = false;

    /* 1. OpenThread's own settings store. Present on every boot after the
     *    first formation (OT persists the active dataset itself); also the
     *    live bench path for a BR that formed under the previous upstream
     *    auto-start build. Adopting it instead of forming keeps already
     *    attached leaves on the mesh. */
    if (otDatasetGetActiveTlvs(instance, &tlvs) == OT_ERROR_NONE && tlvs.mLength > 0) {
        have_dataset = true;
        anchor_sync_from_tlvs(&tlvs);
        ESP_LOGI(TAG, "using OpenThread-persisted dataset (%u bytes)", (unsigned)tlvs.mLength);
    } else {
        /* 2. NVS anchor: recovery path when OT settings were wiped but the
         *    bos_thread namespace survived. */
        uint8_t anchored[BOS_THREAD_DATASET_MAX];
        size_t anchored_len = sizeof(anchored);
        if (bos_thread_dataset_anchor_load(anchored, &anchored_len) == ESP_OK && anchored_len > 0) {
            memcpy(tlvs.mTlvs, anchored, anchored_len);
            tlvs.mLength = (uint8_t)anchored_len;
            have_dataset = true;
            ESP_LOGI(TAG, "restoring anchored dataset (%u bytes)", (unsigned)anchored_len);
        }
    }

    if (!have_dataset) {
        /* 3. First boot: form a new network. otDatasetCreateNewNetwork
         *    (dataset_ftd.h:60) generates a random network key, PSKc, PANID,
         *    extended PANID and mesh-local prefix. The channel is pinned to
         *    CONFIG_OPENTHREAD_NETWORK_CHANNEL (15 in this project's
         *    sdkconfig) instead of the random channel the generator picks, so
         *    the bench RF environment is deterministic. */
        otOperationalDataset dataset;
        otError error = otDatasetCreateNewNetwork(instance, &dataset);
        if (error != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "otDatasetCreateNewNetwork failed: %d", (int)error);
            return ESP_FAIL;
        }

        dataset.mChannel = CONFIG_OPENTHREAD_NETWORK_CHANNEL;
        dataset.mComponents.mIsChannelPresent = true;

        /* Network name ESP-BR-<last two MAC bytes>, the upstream esp-thread-br
         * naming pattern (examples/common/thread_border_router). */
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_BASE) == ESP_OK) {
            char network_name[OT_NETWORK_NAME_MAX_SIZE + 1];
            snprintf(network_name, sizeof(network_name), "ESP-BR-%02X%02X", mac[4], mac[5]);
            memcpy(dataset.mNetworkName.m8, network_name, strlen(network_name) + 1);
            dataset.mComponents.mIsNetworkNamePresent = true;
        }

        otDatasetConvertToTlvs(&dataset, &tlvs);

        /* Persist before starting: an unpersistable dataset is a visible
         * failure, not a network that silently vanishes on reboot. */
        esp_err_t store_err = bos_thread_dataset_anchor_store(tlvs.mTlvs, tlvs.mLength);
        if (store_err != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist newly formed dataset: %s", esp_err_to_name(store_err));
            return store_err;
        }

        /* Log the formation parameters. Never the network key. */
        ESP_LOGI(TAG,
                 "formed new Thread network \"%s\" panid=0x%04x channel=%u",
                 dataset.mNetworkName.m8,
                 (unsigned)dataset.mPanId,
                 (unsigned)dataset.mChannel);
    }

    return esp_openthread_auto_start(&tlvs);
}

esp_err_t bos_thread_runtime_status(bos_thread_runtime_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    /* The lock exists only after esp_openthread_platform_init; acquiring it
     * fails (returns false) before OpenThread is up. esp_openthread_get_instance
     * never returns NULL (it returns the static singleton address,
     * esp_openthread_platform.cpp:163), so the initialized check below is the
     * real gate (otInstanceIsInitialized, instance.h:150). */
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(out->role, sizeof(out->role), "%s",
             otThreadDeviceRoleToString(otThreadGetDeviceRole(instance)));
    snprintf(out->network_name, sizeof(out->network_name), "%s",
             otThreadGetNetworkName(instance));
    out->panid = otLinkGetPanId(instance);
    out->channel = otLinkGetChannel(instance);
    out->rloc16 = otThreadGetRloc16(instance);

    esp_openthread_lock_release();
    return ESP_OK;
}
