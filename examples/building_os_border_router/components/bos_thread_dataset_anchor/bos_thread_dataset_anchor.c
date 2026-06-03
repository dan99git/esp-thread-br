/**
 * Building OS Border Router: Thread dataset anchor.
 * Real NVS read and write. Caller is responsible for handing the loaded
 * dataset to OpenThread at the right time during startup.
 */

#include "bos_thread_dataset_anchor.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

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
