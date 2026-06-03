/**
 * Building OS Border Router firmware, entry point.
 *
 * Boot sequence per docs/08.8-border-router.md section 8.1:
 *   init -> network_wait -> discovering -> registering -> operating
 *
 * This file wires the upstream esp-thread-br stack and the bos_* components.
 * Stub status of each component is in the README.
 */

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_netif_glue.h"
#include "esp_spiffs.h"
#include "esp_vfs_eventfd.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "border_router_launch.h"
#include "bos_convergence_aggregator.h"
#include "bos_diagnostics_server.h"
#include "bos_ledger_ingress.h"
#include "bos_ledger_mesh_serve.h"
#include "bos_server_registration.h"
#include "bos_thread_dataset_anchor.h"
#include "esp_br_web.h"
#include "esp_ot_config.h"

static const char *TAG = "bos_br";

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t init_spiffs(void)
{
#if CONFIG_AUTO_UPDATE_RCP
    esp_vfs_spiffs_conf_t rcp_fw_conf = {
        .base_path = "/" CONFIG_RCP_PARTITION_NAME,
        .partition_label = CONFIG_RCP_PARTITION_NAME,
        .max_files = 10,
        .format_if_mount_failed = false,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&rcp_fw_conf),
                        TAG,
                        "Failed to mount RCP firmware storage");
#endif

#if CONFIG_OPENTHREAD_BR_START_WEB
    esp_vfs_spiffs_conf_t web_conf = {
        .base_path = "/spiffs",
        .partition_label = "web_storage",
        .max_files = 10,
        .format_if_mount_failed = false,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&web_conf),
                        TAG,
                        "Failed to mount web storage");
#endif

    return ESP_OK;
}

static void log_bos_init_result(const char *name, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s; continuing with upstream border-router bring-up", name, esp_err_to_name(err));
    }
}

static void init_bos_components_log_only(void)
{
    log_bos_init_result("bos_thread_dataset_anchor_init", bos_thread_dataset_anchor_init());
    log_bos_init_result("bos_server_registration_init", bos_server_registration_init());
    log_bos_init_result("bos_ledger_ingress_init", bos_ledger_ingress_init());
    log_bos_init_result("bos_convergence_aggregator_init", bos_convergence_aggregator_init());
}

void app_main(void)
{
    size_t max_eventfd = 3;

#if CONFIG_OPENTHREAD_RADIO_SPINEL_SPI
    max_eventfd++;
#endif
#if CONFIG_OPENTHREAD_RADIO_TREL
    max_eventfd++;
#endif

    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = max_eventfd,
    };

    esp_openthread_config_t openthread_config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
            .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
            .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
        },
    };
    esp_rcp_update_config_t rcp_update_config = ESP_OPENTHREAD_RCP_UPDATE_CONFIG();

    ESP_LOGI(TAG, "Building OS Border Router firmware starting");

    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_spiffs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("esp-ot-br"));

#if CONFIG_OPENTHREAD_BR_START_WEB
    esp_br_web_start("/spiffs");
#endif

    init_bos_components_log_only();
    ESP_ERROR_CHECK(bos_diagnostics_server_start());

    ESP_LOGI(TAG, "Building OS Border Router init complete; launching upstream OpenThread BR");
    launch_openthread_border_router(&openthread_config, &rcp_update_config);
    log_bos_init_result("bos_ledger_mesh_serve_init", bos_ledger_mesh_serve_init());
    log_bos_init_result("bos_convergence_aggregator_start", bos_convergence_aggregator_start());
}
