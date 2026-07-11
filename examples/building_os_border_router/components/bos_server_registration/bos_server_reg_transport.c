/**
 * Building OS Border Router: site-server HTTP transport, NVS string helpers
 * and gateway mDNS discovery. Split from bos_server_registration.c;
 * behavior unchanged.
 */

#include "bos_server_registration.h"
#include "bos_server_reg_internal.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bos_time.h"
#include "mdns.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bos_reg";

static char s_server_url[BOS_REG_URL_MAX];

static portMUX_TYPE s_url_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_mdns_url[BOS_REG_URL_MAX];
static uint32_t s_consecutive_send_failures;

esp_err_t nvs_get_string(const char *key, char *out, size_t out_len)
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

esp_err_t nvs_set_string(const char *key, const char *value)
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

esp_err_t load_server_url(void)
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

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    /* Gateway time fallback (spec section 5 source #2): the gateway has no
     * time endpoint (host/runtime/server/src exposes only /api/health), so
     * the HTTP Date header on every gateway response is the documented
     * interim source. bos_time applies it only while SNTP has not synced. */
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Date") == 0) {
        bos_time_note_http_date(evt->header_value);
        return ESP_OK;
    }

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

static void set_mdns_url(const char *url)
{
    taskENTER_CRITICAL(&s_url_mux);
    snprintf(s_mdns_url, sizeof(s_mdns_url), "%s", url ? url : "");
    taskEXIT_CRITICAL(&s_url_mux);
}

/* Copies the active server base URL: mDNS-discovered when present, else the
 * NVS/Kconfig fallback. Returns false when neither is available. */
bool active_server_url(char *out, size_t out_len)
{
    taskENTER_CRITICAL(&s_url_mux);
    snprintf(out, out_len, "%s", s_mdns_url);
    taskEXIT_CRITICAL(&s_url_mux);
    if (out[0] != '\0') {
        return true;
    }
    snprintf(out, out_len, "%s", s_server_url);
    return out[0] != '\0';
}

/* One mDNS browse pass for _bos-server._tcp. Takes the first result carrying
 * an IPv4 address and a port and builds "http://<ip>:<port>". Returns true
 * when a URL was discovered and stored. */
static bool discover_gateway_mdns(void)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(BOS_REG_MDNS_SERVICE,
                                   BOS_REG_MDNS_PROTO,
                                   BOS_REG_MDNS_BROWSE_TIMEOUT_MS,
                                   BOS_REG_MDNS_BROWSE_MAX_RESULTS,
                                   &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS browse %s.%s failed: %s",
                 BOS_REG_MDNS_SERVICE, BOS_REG_MDNS_PROTO, esp_err_to_name(err));
        return false;
    }
    if (results == NULL) {
        ESP_LOGD(TAG, "mDNS browse %s.%s: no results", BOS_REG_MDNS_SERVICE, BOS_REG_MDNS_PROTO);
        return false;
    }

    bool found = false;
    for (mdns_result_t *r = results; r != NULL && !found; r = r->next) {
        if (r->port == 0U) {
            continue;
        }
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) {
                continue;
            }
            char url[BOS_REG_URL_MAX];
            int written = snprintf(url,
                                   sizeof(url),
                                   "http://" IPSTR ":%u",
                                   IP2STR(&a->addr.u_addr.ip4),
                                   (unsigned)r->port);
            if (written < 0 || written >= (int)sizeof(url)) {
                continue;
            }
            set_mdns_url(url);
            ESP_LOGI(TAG, "gateway discovered via mDNS: %s (instance=%s)",
                     url, r->instance_name ? r->instance_name : "");
            found = true;
            break;
        }
    }
    mdns_query_results_free(results);
    return found;
}

/* Browse when nothing is discovered yet or after repeated send failures
 * (gateway IP change re-resolves). Called from the registration task only. */
void refresh_gateway_discovery(void)
{
    char current[BOS_REG_URL_MAX];
    taskENTER_CRITICAL(&s_url_mux);
    snprintf(current, sizeof(current), "%s", s_mdns_url);
    uint32_t failures = s_consecutive_send_failures;
    taskEXIT_CRITICAL(&s_url_mux);

    if (current[0] != '\0' && failures < BOS_REG_REBROWSE_FAILURE_THRESHOLD) {
        return;
    }
    if (discover_gateway_mdns()) {
        taskENTER_CRITICAL(&s_url_mux);
        s_consecutive_send_failures = 0;
        taskEXIT_CRITICAL(&s_url_mux);
    } else if (current[0] != '\0' && failures >= BOS_REG_REBROWSE_FAILURE_THRESHOLD) {
        /* The previously discovered gateway stopped answering the browse:
         * drop the stale URL so the NVS/Kconfig fallback applies. */
        ESP_LOGW(TAG, "gateway mDNS re-browse yielded nothing; falling back to configured URL");
        set_mdns_url("");
    }
}

void note_send_result(esp_err_t err)
{
    taskENTER_CRITICAL(&s_url_mux);
    if (err == ESP_OK) {
        s_consecutive_send_failures = 0;
    } else if (s_consecutive_send_failures < UINT32_MAX) {
        s_consecutive_send_failures++;
    }
    taskEXIT_CRITICAL(&s_url_mux);
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

esp_err_t perform_json_request(const char *method,
                               const char *path,
                               const char *body,
                               bool include_device_token,
                               bos_http_response_t *response)
{
    char base_url[BOS_REG_URL_MAX];
    if (!active_server_url(base_url, sizeof(base_url))) {
        return ESP_ERR_NOT_FOUND;
    }

    char url[BOS_REG_REQUEST_URL_MAX];
    int written = snprintf(url, sizeof(url), "%s%s", base_url, path);
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
        /* Transport-level failure: counts toward the re-browse threshold
         * (an HTTP status from the server proves the URL still reaches it). */
        note_send_result(err);
        return err;
    }
    note_send_result(ESP_OK);
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "%s %s returned HTTP %d: %s", method, path, status, response->data);
        return ESP_FAIL;
    }
    return ESP_OK;
}
