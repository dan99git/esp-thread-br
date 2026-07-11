/**
 * Building OS Border Router: mesh SRP seed publishing (_mesh._udp TXT) and
 * post-deploy ledger announce. Split from bos_ledger_mesh_serve.c; behavior
 * unchanged.
 */

#include "bos_ledger_mesh_serve.h"

#include <stdio.h>
#include <string.h>

#include "bos_ledger_ingress.h"
#include "bos_mesh_announce.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "openthread/coap.h"
#include "openthread/dns.h"
#include "openthread/error.h"
#include "openthread/srp_client.h"
#include "openthread/thread.h"
#include "sdkconfig.h"

static const char *TAG = "bos_mesh_serve";

#define BOS_MESH_SERVICE_NAME "_mesh._udp"
#define BOS_MESH_SRP_TXT_ENTRY_COUNT 7U

#if CONFIG_OPENTHREAD_SRP_CLIENT
static bool s_srp_client_ready;
static bool s_srp_service_registered;
static otSrpClientService s_srp_service;
static otDnsTxtEntry s_srp_txt[BOS_MESH_SRP_TXT_ENTRY_COUNT];
static char s_srp_instance_name[32];
static char s_srp_host_name[48];
static char s_txt_role[] = "seed";
static char s_txt_state[] = "active";
static char s_txt_ledger_version[12];
static char s_txt_digest[9];
static char s_txt_have[12];
static char s_txt_chunks[12];

/* Duplicated from bos_ledger_mesh_serve.c: tiny wrapper needed on both sides
 * of the split; duplicating it is smaller than a shared internal header. */
static const char *ot_error_name(otError err)
{
    return otThreadErrorToString(err);
}
#endif

static void digest_prefix_hex(const uint8_t digest_first_4[4], char out[9])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 4U; i++) {
        out[i * 2] = hex[digest_first_4[i] >> 4];
        out[i * 2 + 1] = hex[digest_first_4[i] & 0x0f];
    }
    out[8] = '\0';
}

#if CONFIG_OPENTHREAD_SRP_CLIENT
static void init_srp_labels(void)
{
    if (s_srp_instance_name[0] != '\0') {
        return;
    }

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_ETH);
    if (ret != ESP_OK) {
        ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (ret == ESP_OK) {
        snprintf(s_srp_instance_name,
                 sizeof(s_srp_instance_name),
                 "br-%02x%02x%02x%02x%02x%02x",
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    } else {
        snprintf(s_srp_instance_name, sizeof(s_srp_instance_name), "border-router");
    }
    snprintf(s_srp_host_name, sizeof(s_srp_host_name), "bos-%s", s_srp_instance_name);
}

static void set_txt_entry(size_t index, const char *key, const char *value)
{
    s_srp_txt[index] = (otDnsTxtEntry){
        .mKey = key,
        .mValue = (const uint8_t *)value,
        .mValueLength = (uint16_t)strlen(value),
    };
}

static void populate_srp_txt_entries(void)
{
    set_txt_entry(0, "class", "border_router");
    set_txt_entry(1, "role", s_txt_role);
    set_txt_entry(2, "state", s_txt_state);
    set_txt_entry(3, "lv", s_txt_ledger_version);
    set_txt_entry(4, "ld", s_txt_digest);
    set_txt_entry(5, "have", s_txt_have);
    set_txt_entry(6, "chunks", s_txt_chunks);
}

static esp_err_t ensure_srp_client_locked(otInstance *instance)
{
    if (s_srp_client_ready) {
        return ESP_OK;
    }

    init_srp_labels();
    otError err = otSrpClientSetHostName(instance, s_srp_host_name);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "mesh SRP host name set failed: %s", ot_error_name(err));
        return ESP_FAIL;
    }

    err = otSrpClientEnableAutoHostAddress(instance);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "mesh SRP auto host address failed: %s", ot_error_name(err));
        return ESP_FAIL;
    }

    otSrpClientEnableAutoStartMode(instance, NULL, NULL);
    s_srp_client_ready = true;
    return ESP_OK;
}

static esp_err_t publish_mesh_srp_locked(otInstance *instance,
                                         uint32_t version,
                                         const char digest[9],
                                         uint32_t chunk_count)
{
    if (ensure_srp_client_locked(instance) != ESP_OK) {
        return ESP_FAIL;
    }

    if (s_srp_service_registered) {
        otError clear_err = otSrpClientClearService(instance, &s_srp_service);
        if (clear_err != OT_ERROR_NONE && clear_err != OT_ERROR_NOT_FOUND) {
            ESP_LOGW(TAG, "mesh SRP clear failed: %s", ot_error_name(clear_err));
            return ESP_FAIL;
        }
        s_srp_service_registered = false;
    }

    snprintf(s_txt_ledger_version, sizeof(s_txt_ledger_version), "%u", (unsigned)version);
    snprintf(s_txt_digest, sizeof(s_txt_digest), "%s", digest);
    snprintf(s_txt_have, sizeof(s_txt_have), "%u", (unsigned)chunk_count);
    snprintf(s_txt_chunks, sizeof(s_txt_chunks), "%u", (unsigned)chunk_count);
    populate_srp_txt_entries();

    memset(&s_srp_service, 0, sizeof(s_srp_service));
    s_srp_service.mName = BOS_MESH_SERVICE_NAME;
    s_srp_service.mInstanceName = s_srp_instance_name;
    s_srp_service.mTxtEntries = s_srp_txt;
    s_srp_service.mNumTxtEntries = (uint8_t)BOS_MESH_SRP_TXT_ENTRY_COUNT;
    s_srp_service.mPort = OT_DEFAULT_COAP_PORT;

    otError err = otSrpClientAddService(instance, &s_srp_service);
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGW(TAG, "mesh SRP add failed: %s", ot_error_name(err));
        return ESP_FAIL;
    }

    s_srp_service_registered = true;
    ESP_LOGI(TAG,
             "mesh SRP seed published: %s.%s lv=%s ld=%s have=%s/%s",
             s_srp_instance_name,
             BOS_MESH_SERVICE_NAME,
             s_txt_ledger_version,
             s_txt_digest,
             s_txt_have,
             s_txt_chunks);
    return ESP_OK;
}
#endif

esp_err_t bos_ledger_mesh_serve_publish_active(uint32_t version, const uint8_t *digest_first_4)
{
    if (!digest_first_4) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_OPENTHREAD_SRP_CLIENT
    bos_ledger_active_t active;
    uint32_t chunk_count = 0;
    if (bos_ledger_ingress_get_active(&active) == ESP_OK && active.present && active.version == version) {
        chunk_count = active.chunk_count;
    }

    char digest[9];
    digest_prefix_hex(digest_first_4, digest);

    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_err_t ret = publish_mesh_srp_locked(instance, version, digest, chunk_count);
    esp_openthread_lock_release();
    return ret;
#else
    ESP_LOGW(TAG, "SRP client disabled; cannot publish mesh seed for ledger version=%u", (unsigned)version);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t bos_ledger_mesh_serve_announce_ledger(uint32_t version, const uint8_t *digest_first_4)
{
    if (!digest_first_4) {
        return ESP_ERR_INVALID_ARG;
    }
    // Refresh the SRP lv/ld advertisement for the freshly-deployed ledger, then
    // nudge the live mesh so nodes poll /mesh/have and pull now instead of on
    // their next backoff cycle. The nudge carries no authority: each node still
    // version-compares and pulls through the existing manifest/chunk path.
    esp_err_t ret = bos_ledger_mesh_serve_publish_active(version, digest_first_4);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ledger deploy SRP refresh failed: %s", esp_err_to_name(ret));
    }
    char prefix[9];
    digest_prefix_hex(digest_first_4, prefix);
    esp_err_t announced = bos_mesh_announce_fire("ledger", version, prefix);
    if (announced != ESP_OK) {
        ESP_LOGW(TAG, "ledger deploy announce nudge failed: %s", esp_err_to_name(announced));
        if (ret == ESP_OK) {
            ret = announced;
        }
    }
    return ret;
}
