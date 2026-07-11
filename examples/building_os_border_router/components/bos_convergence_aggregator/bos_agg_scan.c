/**
 * Building OS Border Router: mesh convergence aggregator SRP server scan.
 *
 * Owns the periodic SRP server scan task, the floating-book touch path,
 * and the aggregator start entry point.
 */

#include "bos_agg_internal.h"

#include "bos_floating_book.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "openthread/ip6.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "bos_agg";

bool bos_agg_started = false;
static TaskHandle_t s_task;

static bool is_zero_addr(const otIp6Address *addr)
{
    static const uint8_t zero[16] = {0};
    return !addr || memcmp(addr->mFields.m8, zero, sizeof(zero)) == 0;
}

static bool service_name_matches_type(const char *service_name, const char *service_type)
{
    return service_name && service_type && strstr(service_name, service_type) != NULL;
}

/* True when addr is inside the mesh-local /64 but is NOT the RLOC
 * (IID 0000:00ff:fe00:xxxx): i.e. the stable ML-EID the floating book
 * records (spec column 9: eid is the ML-EID, never the OMR). */
static bool addr_is_ml_eid(const otIp6Address *addr, const otMeshLocalPrefix *mlp)
{
    if (!addr || !mlp || memcmp(addr->mFields.m8, mlp->m8, OT_MESH_LOCAL_PREFIX_SIZE) != 0) {
        return false;
    }
    const uint8_t *iid = &addr->mFields.m8[8];
    bool is_rloc_iid = iid[0] == 0x00U && iid[1] == 0x00U && iid[2] == 0x00U &&
                       iid[3] == 0xffU && iid[4] == 0xfeU && iid[5] == 0x00U;
    return !is_rloc_iid;
}

/* Floating-book touch (spec docs/scratch/phonebook-v2-two-book-spec.md
 * section 3 step 2): an SRP-registered _mesh._udp peer that is not
 * commissioned (TXT cm absent or != 1) gets a BR-authored floating row.
 * Runs under the held OT lock; bos_floating_book_touch is RAM-only, the NVS
 * persist happens off the lock in browse_task. */
static void floating_book_note_peer(const otSrpServerHost *host, const bos_peer_t *update)
{
    if (!update || update->hardware_id[0] == '\0') {
        return; /* no durable EUI-64 key: nothing to record */
    }
    if (update->has_commissioned && update->commissioned) {
        return; /* paired devices belong to the operational book */
    }

    char eid_str[46] = "";
    otInstance *instance = esp_openthread_get_instance();
    if (instance) {
        const otMeshLocalPrefix *mlp = otThreadGetMeshLocalPrefix(instance);
        uint8_t address_count = 0;
        const otIp6Address *addresses = otSrpServerHostGetAddresses(host, &address_count);
        for (uint8_t i = 0; addresses && i < address_count; i++) {
            if (addr_is_ml_eid(&addresses[i], mlp)) {
                otIp6AddressToString(&addresses[i], eid_str, sizeof(eid_str));
                break;
            }
        }
    }

    /* Photometric columns (phonebook v3). Ints render to strings; absent
     * lumens/cct stay empty so the floating row records blank, per spec. */
    char rated_lumens_str[12] = "";
    char cct_str[8] = "";
    if (update->has_rated_lumens) {
        snprintf(rated_lumens_str, sizeof(rated_lumens_str), "%d", update->rated_lumens);
    }
    if (update->has_cct) {
        snprintf(cct_str, sizeof(cct_str), "%d", update->cct);
    }

    bos_floating_touch_t touch = {
        .eui64 = update->hardware_id,
        .device_id = update->id,
        .device_class = update->device_class,
        .model_id = update->model_id,
        .fw = update->firmware_version,
        .eid = eid_str,
        .sku = update->sku,
        .rated_power_w = update->rated_power_w,
        .rated_lumens = rated_lumens_str,
        .cct = cct_str,
    };
    esp_err_t err = bos_floating_book_touch(&touch);
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "floating-book touch failed for %s: %s",
                 update->hardware_id, esp_err_to_name(err));
    }
}

static void merge_srp_server_service(const otSrpServerHost *host, const otSrpServerService *service)
{
    const char *service_name = otSrpServerServiceGetServiceName(service);
    bool is_tmfs = service_name_matches_type(service_name, BOS_AGG_SERVICE_TMFS);
    bool is_mesh = service_name_matches_type(service_name, BOS_AGG_SERVICE_MESH);

    if (!is_tmfs && !is_mesh) {
        return;
    }

    const char *instance = otSrpServerServiceGetInstanceLabel(service);
    if (!instance || instance[0] == '\0') {
        instance = otSrpServerServiceGetInstanceName(service);
    }
    if (!instance || instance[0] == '\0') {
        return;
    }

    bos_peer_t update = {0};
    copy_str(update.id, sizeof(update.id), instance);
    copy_str(update.host_name, sizeof(update.host_name), otSrpServerHostGetFullName(host));

    uint16_t txt_len = 0;
    const uint8_t *txt = otSrpServerServiceGetTxtData(service, &txt_len);
    parse_txt_into_peer(&update, txt, txt_len);
    if (update.id[0] == '\0') {
        copy_str(update.id, sizeof(update.id), instance);
    }

    if (is_tmfs) {
        update.has_tmfs = true;
        update.tmfs_port = otSrpServerServiceGetPort(service);
        copy_str(update.tmfs_instance, sizeof(update.tmfs_instance), instance);
    }
    if (is_mesh) {
        update.has_mesh = true;
        update.coap_port = otSrpServerServiceGetPort(service);
        copy_str(update.mesh_instance, sizeof(update.mesh_instance), instance);
    }

    uint8_t address_count = 0;
    const otIp6Address *addresses = otSrpServerHostGetAddresses(host, &address_count);
    for (uint8_t i = 0; addresses && i < address_count; i++) {
        if (!is_zero_addr(&addresses[i])) {
            memcpy(update.address, addresses[i].mFields.m8, sizeof(update.address));
            update.has_address = true;
            break;
        }
    }

    if (!is_self_srp_record(&update)) {
        merge_peer(&update);
        if (is_mesh) {
            floating_book_note_peer(host, &update);
        }
    }
}

static void scan_srp_server_services(otInstance *instance)
{
    if (otSrpServerGetState(instance) != OT_SRP_SERVER_STATE_RUNNING) {
        return;
    }

    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL) {
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }

        const otSrpServerService *service = NULL;
        while ((service = otSrpServerHostGetNextService(host, service)) != NULL) {
            if (otSrpServerServiceIsDeleted(service)) {
                continue;
            }
            merge_srp_server_service(host, service);
        }
    }
}

static bool thread_role_attached(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_CHILD ||
           role == OT_DEVICE_ROLE_ROUTER ||
           role == OT_DEVICE_ROLE_LEADER;
}

static void browse_task(void *arg)
{
    (void)arg;
    bool wait_after_attach = true;

    for (;;) {
        otInstance *instance = esp_openthread_get_instance();
        if (!instance) {
            vTaskDelay(pdMS_TO_TICKS(BOS_AGG_ATTACH_POLL_MS));
            continue;
        }
        esp_openthread_lock_acquire(portMAX_DELAY);
        otDeviceRole role = otThreadGetDeviceRole(instance);
        if (!thread_role_attached(role)) {
            wait_after_attach = true;
            esp_openthread_lock_release();
            vTaskDelay(pdMS_TO_TICKS(BOS_AGG_ATTACH_POLL_MS));
            continue;
        }
        if (wait_after_attach) {
            wait_after_attach = false;
            esp_openthread_lock_release();
            vTaskDelay(pdMS_TO_TICKS(BOS_AGG_FIRST_BROWSE_DELAY_MS));
            continue;
        }
        scan_srp_server_services(instance);
        esp_openthread_lock_release();
        /* NVS commit for any floating-book rows touched during the scan;
         * deliberately off the OT lock. */
        bos_floating_book_persist_if_dirty();
        vTaskDelay(pdMS_TO_TICKS(BOS_AGG_BROWSE_PERIOD_MS));
    }
}

esp_err_t bos_convergence_aggregator_start(void)
{
    if (bos_agg_started) {
        return ESP_OK;
    }
    if (!esp_openthread_get_instance()) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t created = xTaskCreate(browse_task, "bos_srp_browse", 6144, NULL, 4, &s_task);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    bos_agg_started = true;
    ESP_LOGI(TAG, "SRP server scan task started for %s; %s scan deferred during bench recovery", BOS_AGG_SERVICE_TMFS, BOS_AGG_SERVICE_MESH);
    return ESP_OK;
}
