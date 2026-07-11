/**
 * Building OS Border Router: site server registration and heartbeat.
 *
 * HTTP transport + gateway mDNS discovery live in bos_server_reg_transport.c;
 * the heartbeat RAM ring lives in bos_server_reg_heartbeat.c (shared
 * internals: bos_server_reg_internal.h).
 */

#include "bos_server_registration.h"
#include "bos_server_reg_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "bos_convergence_aggregator.h"

static const char *TAG = "bos_reg";

#define BOS_REG_READY_BIT BIT0

static EventGroupHandle_t s_events;
static bool s_task_started;
char bos_reg_device_id[BOS_REG_DEVICE_ID_MAX];
static char s_backbone_ipv4[16];
static char s_backbone_ipv6[40];

/* Same 16-lowercase-hex EUI-64 format as the LC
 * (firmware/xiao-esp32c6/components/thread_runtime/thread_runtime_identity.c
 * thread_runtime_identity_format_eui64). */
static void format_eui64_hex(char out[17], const uint8_t eui64[8])
{
    snprintf(out,
             17,
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             eui64[0],
             eui64[1],
             eui64[2],
             eui64[3],
             eui64[4],
             eui64[5],
             eui64[6],
             eui64[7]);
}

/* EUI-64 device identity (spec docs/scratch/phonebook-v2-two-book-spec.md
 * section 6): mirror the LC fill_eui64 (thread_runtime.c, ESP_MAC_IEEE802154)
 * first. On this BR host (ESP32-S3, SOC_IEEE802154_SUPPORTED not set) the
 * IEEE802154 MAC type is not in the esp_mac table and esp_read_mac fails, so
 * fall back to the standard EUI-48 -> EUI-64 expansion of the base MAC
 * (insert ff:fe, the same fffe-elision convention already coded into the
 * BR's proxy_normalise_eui64 and the C6 efuse MAC_EXT default). */
static esp_err_t fill_device_eui64(char out[17])
{
    uint8_t eui64[8] = {0};
    if (esp_read_mac(eui64, ESP_MAC_IEEE802154) == ESP_OK) {
        format_eui64_hex(out, eui64);
        return ESP_OK;
    }

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_ETH);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (err != ESP_OK) {
        return err;
    }
    eui64[0] = mac[0];
    eui64[1] = mac[1];
    eui64[2] = mac[2];
    eui64[3] = 0xff;
    eui64[4] = 0xfe;
    eui64[5] = mac[3];
    eui64[6] = mac[4];
    eui64[7] = mac[5];
    format_eui64_hex(out, eui64);
    return ESP_OK;
}

static esp_err_t load_device_id(void)
{
    /* NVS override first (unchanged behaviour); derived EUI-64 otherwise,
     * replacing the previous colon-separated MAC-format defect. */
    esp_err_t err = nvs_get_string(BOS_REG_KEY_DEVICE_ID, bos_reg_device_id, sizeof(bos_reg_device_id));
    if (err == ESP_OK && bos_reg_device_id[0] != '\0') {
        return ESP_OK;
    }

    char eui64[17];
    err = fill_device_eui64(eui64);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(bos_reg_device_id, sizeof(bos_reg_device_id), "%s", eui64);
    return nvs_set_string(BOS_REG_KEY_DEVICE_ID, bos_reg_device_id);
}

/* Diagnosis fix (br-diagnosis.md finding #5): prefer the backbone IPv4 for
 * registration; only fall back to IPv6 when it is a backbone ULA/global
 * address (s_backbone_ipv6 is only ever populated with one, see
 * ip_event_handler). Previously any IPv6 event - including Thread-side or
 * link-local - could win and the site server would point at a non-routable
 * address: BR on the LAN but invisible to the server. */
static const char *registration_address(void)
{
    if (s_backbone_ipv4[0] != '\0') {
        return s_backbone_ipv4;
    }
    if (s_backbone_ipv6[0] != '\0') {
        return s_backbone_ipv6;
    }
    return "";
}

static esp_err_t persist_registration_response(const char *json)
{
    esp_err_t err = ESP_FAIL;
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "device_token");
    if (!cJSON_IsString(token) || token->valuestring == NULL || token->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = nvs_set_string(BOS_REG_KEY_TOKEN, token->valuestring);
    if (err == ESP_OK) {
        const cJSON *registration = cJSON_GetObjectItemCaseSensitive(root, "registration");
        const cJSON *device_id = registration ? cJSON_GetObjectItemCaseSensitive(registration, "device_id") : NULL;
        if (cJSON_IsString(device_id) && device_id->valuestring && device_id->valuestring[0] != '\0') {
            err = nvs_set_string(BOS_REG_KEY_DEVICE_ID, device_id->valuestring);
            if (err == ESP_OK) {
                snprintf(bos_reg_device_id, sizeof(bos_reg_device_id), "%s", device_id->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t register_with_server(void)
{
    char body[384];
    bos_http_response_t response;
    int written = snprintf(body,
                           sizeof(body),
                           "{\"device_id\":\"%s\","
                           "\"firmware_version\":\"building-os-border-router\","
                           "\"capabilities\":{\"thread_address\":\"%s\",\"http_port\":%u,"
                           "\"space\":\"infrastructure\",\"upstream\":\"ethernet\","
                           "\"role\":\"border-router\"}}",
                           bos_reg_device_id,
                           registration_address(),
                           80U);
    if (written < 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = perform_json_request("POST", "/api/border-routers/claim", body, false, &response);
    if (err != ESP_OK) {
        return err;
    }

    err = persist_registration_response(response.data);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registration response parse/persist failed: %s", esp_err_to_name(err));
        return err;
    }

    char base_url[BOS_REG_URL_MAX] = "";
    (void)active_server_url(base_url, sizeof(base_url));
    ESP_LOGI(TAG, "registered border router %s with BOS server %s", bos_reg_device_id, base_url);
    return ESP_OK;
}

static esp_err_t send_heartbeat(void)
{
    char path[96];
    char body[] = "{\"online\":true}";
    bos_http_response_t response;

    int written = snprintf(path, sizeof(path), "/api/devices/%s/status", bos_reg_device_id);
    if (written < 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return perform_json_request("PUT", path, body, true, &response);
}

static esp_err_t send_convergence(void)
{
    char path[128];
    bos_http_response_t response;
    char *body = (char *)malloc(BOS_REG_CONVERGENCE_JSON_MAX);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int rendered = bos_convergence_aggregator_snapshot_json(body, BOS_REG_CONVERGENCE_JSON_MAX);
    if (rendered < 0) {
        free(body);
        return ESP_FAIL;
    }

    int written = snprintf(path, sizeof(path), "/api/border-routers/%s/convergence", bos_reg_device_id);
    if (written < 0 || written >= (int)sizeof(path)) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = perform_json_request("POST", path, body, true, &response);
    free(body);
    return err;
}

static void send_operating_updates(void)
{
    /* Buffered heartbeats flush oldest-first before the live one so the
     * server receives them in capture order. Any failure (flush or live)
     * buffers the current heartbeat. */
    esp_err_t err = flush_heartbeat_buffer();
    if (err == ESP_OK) {
        err = send_heartbeat();
    }
    if (err != ESP_OK) {
        buffer_heartbeat();
        uint32_t buffered = 0;
        uint32_t dropped = 0;
        bos_server_registration_heartbeat_stats(&buffered, &dropped);
        ESP_LOGW(TAG,
                 "BOS heartbeat failed: %s; buffered in RAM (%u held, %u dropped)",
                 esp_err_to_name(err),
                 (unsigned)buffered,
                 (unsigned)dropped);
    }
    err = send_convergence();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOS convergence post failed: %s", esp_err_to_name(err));
    }
}

static void registration_task(void *arg)
{
    (void)arg;

    if (load_server_url() != ESP_OK) {
        /* No longer fatal: mDNS browse is the primary discovery path and the
         * NVS/Kconfig URL is fallback only (spec section 6). */
        ESP_LOGW(TAG, "no NVS/Kconfig BOS server URL; relying on mDNS gateway discovery");
    }

    if (load_device_id() != ESP_OK) {
        ESP_LOGW(TAG, "failed to derive border-router device id");
        s_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        xEventGroupWaitBits(s_events, BOS_REG_READY_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

        refresh_gateway_discovery();

        char base_url[BOS_REG_URL_MAX];
        if (!active_server_url(base_url, sizeof(base_url))) {
            ESP_LOGW(TAG, "no gateway URL (mDNS empty, no fallback); retrying browse");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        if (!bos_server_registration_is_registered()) {
            esp_err_t err = register_with_server();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "BOS registration failed: %s; retrying", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            send_operating_updates();
        } else {
            send_operating_updates();
        }

        vTaskDelay(pdMS_TO_TICKS(BOS_REG_HEARTBEAT_PERIOD_S * 1000));
    }
}

/* Mirrors is_backbone_netif() in bos_diagnostics_server.c: only the
 * protocol_examples_common backbone interface counts (br-diagnosis.md
 * finding #5 - the old handler accepted ANY IPv6 event, including the
 * OpenThread netif's). */
static bool is_backbone_netif(esp_netif_t *netif)
{
    const char *desc = esp_netif_get_desc(netif);
    if (!desc) {
        return false;
    }

#if CONFIG_EXAMPLE_CONNECT_ETHERNET
    if (strcmp(desc, EXAMPLE_NETIF_DESC_ETH) == 0) {
        return true;
    }
#endif
#if CONFIG_EXAMPLE_CONNECT_WIFI
    if (strcmp(desc, EXAMPLE_NETIF_DESC_STA) == 0) {
        return true;
    }
#endif
    return false;
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != IP_EVENT) {
        return;
    }

    if (event_id == IP_EVENT_ETH_GOT_IP || event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_backbone_ipv4, sizeof(s_backbone_ipv4), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, BOS_REG_READY_BIT);
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        if (!is_backbone_netif(event->esp_netif)) {
            return;
        }
        esp_ip6_addr_type_t addr_type = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
        if (addr_type != ESP_IP6_ADDR_IS_UNIQUE_LOCAL && addr_type != ESP_IP6_ADDR_IS_GLOBAL) {
            /* Link-local and other scopes are not LAN-routable registration
             * addresses; ignore them (the server could not reach us there). */
            return;
        }
        snprintf(s_backbone_ipv6,
                 sizeof(s_backbone_ipv6),
                 IPV6STR,
                 IPV62STR(event->ip6_info.ip));
        xEventGroupSetBits(s_events, BOS_REG_READY_BIT);
    }
}

esp_err_t bos_server_registration_init(void)
{
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
        if (s_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_task_started) {
        BaseType_t ok = xTaskCreate(registration_task, "bos_reg", 6144, NULL, 5, NULL);
        if (ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        s_task_started = true;
    }

    return ESP_OK;
}

bool bos_server_registration_is_registered(void)
{
    nvs_handle_t h;
    if (nvs_open(BOS_REG_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, BOS_REG_KEY_TOKEN, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len > 0;
}

esp_err_t bos_server_registration_get_token(char *out, size_t out_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_REG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_str(h, BOS_REG_KEY_TOKEN, out, &out_len);
    nvs_close(h);
    return err;
}

esp_err_t bos_server_registration_get_device_id(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (load_device_id() != ESP_OK || bos_reg_device_id[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    int written = snprintf(out, out_len, "%s", bos_reg_device_id);
    return written < 0 || written >= (int)out_len ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t bos_server_registration_get_server_url(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Reports the URL actually in use: mDNS-discovered when present, else
     * the NVS/Kconfig fallback. */
    (void)load_server_url();
    char base_url[BOS_REG_URL_MAX];
    if (!active_server_url(base_url, sizeof(base_url))) {
        return ESP_ERR_NOT_FOUND;
    }
    int written = snprintf(out, out_len, "%s", base_url);
    return written < 0 || written >= (int)out_len ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
