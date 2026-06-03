#include "bos_openthread_compat.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/logging.h"

static const char *TAG = "bos_ot_compat";

static esp_openthread_config_t s_config;
static SemaphoreHandle_t       s_start_done;
static esp_err_t               s_start_err;
static bool                    s_started;
static bool                    s_cli_requested;

static void signal_start_result(esp_err_t err)
{
    s_start_err = err;
    if (s_start_done) {
        xSemaphoreGive(s_start_done);
    }
}

static void ot_task_worker(void *ctx)
{
    const esp_openthread_config_t *config = (const esp_openthread_config_t *)ctx;
    esp_netif_t *openthread_netif = esp_netif_new(&config->netif_config);
    if (!openthread_netif) {
        signal_start_result(ESP_ERR_NO_MEM);
        s_started = false;
        vTaskDelete(NULL);
    }

    esp_err_t err = esp_openthread_init(&config->platform_config);
    if (err != ESP_OK) {
        esp_netif_destroy(openthread_netif);
        signal_start_result(err);
        s_started = false;
        vTaskDelete(NULL);
    }

    void *glue = esp_openthread_netif_glue_init(&config->platform_config);
    if (!glue) {
        esp_netif_destroy(openthread_netif);
        signal_start_result(ESP_ERR_NO_MEM);
        s_started = false;
        vTaskDelete(NULL);
    }

    err = esp_netif_attach(openthread_netif, glue);
    if (err != ESP_OK) {
        esp_openthread_netif_glue_deinit();
        esp_netif_destroy(openthread_netif);
        signal_start_result(err);
        s_started = false;
        vTaskDelete(NULL);
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
#if CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
    (void)otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL);
#endif
#if CONFIG_OPENTHREAD_CLI
    if (s_cli_requested) {
        esp_openthread_cli_init();
        esp_openthread_cli_create_task();
    }
#endif
    esp_openthread_lock_release();

    signal_start_result(ESP_OK);

    err = esp_openthread_launch_mainloop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OpenThread mainloop exited: %s", esp_err_to_name(err));
    }

    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);
    esp_vfs_eventfd_unregister();
    s_started = false;
    vTaskDelete(NULL);
}

void ot_console_start(void)
{
    s_cli_requested = true;
}

esp_err_t esp_openthread_start(const esp_openthread_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_start_done = xSemaphoreCreateBinary();
    if (!s_start_done) {
        return ESP_ERR_NO_MEM;
    }

    s_config = *config;
    s_start_err = ESP_OK;
    s_started = true;

    BaseType_t created = xTaskCreate(ot_task_worker, "ot_main", 8192, &s_config, 5, NULL);
    if (created != pdPASS) {
        vSemaphoreDelete(s_start_done);
        s_start_done = NULL;
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_start_done, portMAX_DELAY);
    esp_err_t err = s_start_err;
    vSemaphoreDelete(s_start_done);
    s_start_done = NULL;
    if (err != ESP_OK) {
        s_started = false;
    }
    return err;
}
