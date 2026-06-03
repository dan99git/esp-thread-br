/**
 * Building OS Border Router: site server registration and heartbeat.
 */

#include "bos_server_registration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "bos_convergence_aggregator.h"

static const char *TAG = "bos_reg";

#define BOS_REG_READY_BIT BIT0
#define BOS_REG_RESPONSE_MAX 2048
#define BOS_REG_CONVERGENCE_JSON_MAX 12288
#define BOS_REG_URL_MAX 160
#define BOS_REG_REQUEST_URL_MAX 320
#define BOS_REG_DEVICE_ID_MAX 64
#define BOS_REG_TOKEN_MAX 96

typedef struct {
    char data[BOS_REG_RESPONSE_MAX];
    int len;
} bos_http_response_t;

static EventGroupHandle_t s_events;
static bool s_task_started;
static char s_server_url[BOS_REG_URL_MAX];
static char s_device_id[BOS_REG_DEVICE_ID_MAX];
static char s_backbone_ipv4[16];
static char s_backbone_ipv6[40];

static esp_err_t nvs_get_string(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_REG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_set_string(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_REG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t load_server_url(void)
{
    esp_err_t err = nvs_get_string(BOS_REG_KEY_SERVER_URL, s_server_url, sizeof(s_server_url));
    if (err == ESP_OK && s_server_url[0] != '\0') {
        return ESP_OK;
    }

#ifdef CONFIG_BOS_SERVER_DEFAULT_URL
    snprintf(s_server_url, sizeof(s_server_url), "%s", CONFIG_BOS_SERVER_DEFAULT_URL);
#else
    s_server_url[0] = '\0';
#endif

    return s_server_url[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t load_device_id(void)
{
    esp_err_t err = nvs_get_string(BOS_REG_KEY_DEVICE_ID, s_device_id, sizeof(s_device_id));
    if (err == ESP_OK && s_device_id[0] != '\0') {
        return ESP_OK;
    }

    uint8_t mac[6];
    err = esp_read_mac(mac, ESP_MAC_ETH);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (err != ESP_OK) {
        return err;
    }

    snprintf(s_device_id,
             sizeof(s_device_id),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
    return nvs_set_string(BOS_REG_KEY_DEVICE_ID, s_device_id);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0 || evt->user_data == NULL) {
        return ESP_OK;
    }

    bos_http_response_t *response = (bos_http_response_t *)evt->user_data;
    int remaining = (int)sizeof(response->data) - response->len - 1;
    int copy_len = evt->data_len < remaining ? evt->data_len : remaining;
    if (copy_len > 0) {
        memcpy(response->data + response->len, evt->data, copy_len);
        response->len += copy_len;
        response->data[response->len] = '\0';
    }
    return ESP_OK;
}

static const char *registration_address(void)
{
    if (s_backbone_ipv6[0] != '\0') {
        return s_backbone_ipv6;
    }
    if (s_backbone_ipv4[0] != '\0') {
        return s_backbone_ipv4;
    }
    return "";
}

static esp_err_t set_auth_headers(esp_http_client_handle_t client, bool include_device_token)
{
#ifdef CONFIG_BOS_SERVER_API_KEY
    if (CONFIG_BOS_SERVER_API_KEY[0] != '\0') {
        esp_err_t err = esp_http_client_set_header(client, "x-api-key", CONFIG_BOS_SERVER_API_KEY);
        if (err != ESP_OK) {
            return err;
        }
    }
#endif

    if (!include_device_token) {
        return ESP_OK;
    }

    char token[BOS_REG_TOKEN_MAX];
    esp_err_t err = bos_server_registration_get_token(token, sizeof(token));
    if (err != ESP_OK) {
        return err;
    }

    return esp_http_client_set_header(client, "X-Device-Token", token);
}

static esp_err_t perform_json_request(const char *method,
                                      const char *path,
                                      const char *body,
                                      bool include_device_token,
                                      bos_http_response_t *response)
{
    char url[BOS_REG_REQUEST_URL_MAX];
    int written = snprintf(url, sizeof(url), "%s%s", s_server_url, path);
    if (written < 0 || written >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = strcmp(method, "PUT") == 0 ? HTTP_METHOD_PUT : HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .event_handler = http_event_handler,
        .user_data = response,
    };

    response->len = 0;
    response->data[0] = '\0';

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = set_auth_headers(client, include_device_token);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_set_post_field(client, body, strlen(body));

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "%s %s returned HTTP %d: %s", method, path, status, response->data);
        return ESP_FAIL;
    }
    return ESP_OK;
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
                snprintf(s_device_id, sizeof(s_device_id), "%s", device_id->valuestring);
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
                           s_device_id,
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

    ESP_LOGI(TAG, "registered border router %s with BOS server %s", s_device_id, s_server_url);
    return ESP_OK;
}

static esp_err_t send_heartbeat(void)
{
    char path[96];
    char body[] = "{\"online\":true}";
    bos_http_response_t response;

    int written = snprintf(path, sizeof(path), "/api/devices/%s/status", s_device_id);
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

    int written = snprintf(path, sizeof(path), "/api/border-routers/%s/convergence", s_device_id);
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
    esp_err_t err = send_heartbeat();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOS heartbeat failed: %s", esp_err_to_name(err));
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
        ESP_LOGW(TAG, "no BOS server URL configured; registration disabled");
        s_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    if (load_device_id() != ESP_OK) {
        ESP_LOGW(TAG, "failed to derive border-router device id");
        s_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        xEventGroupWaitBits(s_events, BOS_REG_READY_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

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
    if (load_device_id() != ESP_OK || s_device_id[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    int written = snprintf(out, out_len, "%s", s_device_id);
    return written < 0 || written >= (int)out_len ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t bos_server_registration_get_server_url(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (load_server_url() != ESP_OK || s_server_url[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    int written = snprintf(out, out_len, "%s", s_server_url);
    return written < 0 || written >= (int)out_len ? ESP_ERR_INVALID_SIZE : ESP_OK;
}
