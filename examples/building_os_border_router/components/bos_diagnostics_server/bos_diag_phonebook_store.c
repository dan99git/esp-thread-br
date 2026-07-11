/**
 * Building OS Border Router: local diagnostics web surface.
 * Split part: phonebook state (NVS + RAM), mesh push, phonebook routes, and
 * the eui64/spatial resolvers (see bos_diag_internal.h).
 */

#include "bos_diagnostics_server.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bos_commissioning.h"
#include "bos_convergence_aggregator.h"
#include "bos_floating_book.h"
#include "bos_joiner.h"
#include "bos_thread_dataset_anchor.h"
#include "bos_thread_diag.h"
#include "bos_br_ota.h"
#include "bos_ledger_ingress.h"
#include "bos_ledger_mesh_serve.h"
#include "bos_server_registration.h"
#include "cJSON.h"
#include "esp_br_web.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_rcp_update.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "openthread/coap.h"
#include "openthread/dns.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/platform/radio.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

#include "bos_diag_internal.h"

static const char *TAG = "bos_diag";

static SemaphoreHandle_t s_phonebook_lock;
static bos_phonebook_t s_phonebook;

static esp_err_t phonebook_apply(const bos_phonebook_t *book)
{
    if (!book || !s_phonebook_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_phonebook_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_phonebook = *book;
    xSemaphoreGive(s_phonebook_lock);
    return ESP_OK;
}

static void phonebook_mesh_push_set_error(bos_phonebook_mesh_push_t *push, const char *error)
{
    if (!push || !error || push->first_error[0] != '\0') {
        return;
    }
    snprintf(push->first_error, sizeof(push->first_error), "%s", error);
}

static otError phonebook_emit_coap(otInstance *instance,
                                   const otIp6Address *dest,
                                   const char *payload,
                                   size_t payload_len)
{
    if (!instance || !dest || !payload || payload_len == 0U || payload_len > UINT16_MAX) {
        return OT_ERROR_INVALID_ARGS;
    }

    otMessage *request = otCoapNewMessage(instance, NULL);
    if (!request) {
        return OT_ERROR_NO_BUFS;
    }

    otCoapMessageInit(request, OT_COAP_TYPE_NON_CONFIRMABLE, OT_COAP_CODE_POST);
    otError err = otCoapMessageAppendUriPathOptions(request, BOS_PHONEBOOK_MESH_PATH);
    if (err == OT_ERROR_NONE) {
        err = otCoapMessageAppendContentFormatOption(request, OT_COAP_OPTION_CONTENT_FORMAT_TEXT_PLAIN);
    }
    if (err == OT_ERROR_NONE) {
        err = otCoapMessageSetPayloadMarker(request);
    }
    if (err == OT_ERROR_NONE) {
        err = otMessageAppend(request, payload, (uint16_t)payload_len);
    }
    if (err == OT_ERROR_NONE) {
        otMessageInfo info;
        memset(&info, 0, sizeof(info));
        info.mPeerAddr = *dest;
        info.mPeerPort = OT_DEFAULT_COAP_PORT;
        err = otCoapSendRequest(instance, request, &info, NULL, NULL);
    }
    if (err != OT_ERROR_NONE) {
        otMessageFree(request);
    }
    return err;
}

static void phonebook_push_to_mesh(const bos_phonebook_t *book,
                                   const char *raw,
                                   bos_phonebook_mesh_push_t *push)
{
    if (!push) {
        return;
    }
    memset(push, 0, sizeof(*push));
    if (!book || !book->loaded || !raw || raw[0] == '\0') {
        phonebook_mesh_push_set_error(push, "phonebook not loaded");
        return;
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        phonebook_mesh_push_set_error(push, "openthread lock timeout");
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        phonebook_mesh_push_set_error(push, "openthread not running");
        return;
    }
    const otMeshLocalPrefix *mlp = otThreadGetMeshLocalPrefix(instance);
    size_t payload_len = strlen(raw);

    for (size_t i = 0; i < book->row_count; i++) {
        push->attempted++;
        proxy_addr_scope_t scope = proxy_classify_address(&book->rows[i].address, mlp);
        if (scope != PROXY_SCOPE_MESH_LOCAL) {
            push->bad_scope++;
            push->failed++;
            phonebook_mesh_push_set_error(push, "phonebook row eid is not mesh-local");
            continue;
        }

        otError err = phonebook_emit_coap(instance, &book->rows[i].address, raw, payload_len);
        if (err == OT_ERROR_NONE) {
            push->sent++;
        } else {
            push->failed++;
            phonebook_mesh_push_set_error(push, otThreadErrorToString(err));
        }
    }

    esp_openthread_lock_release();
}

static esp_err_t phonebook_store_raw(const char *raw)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BOS_PHONEBOOK_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, BOS_PHONEBOOK_NVS_KEY, raw, strlen(raw) + 1U);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t phonebook_load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(BOS_PHONEBOOK_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no BR phonebook stored in NVS");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = 0;
    err = nvs_get_blob(handle, BOS_PHONEBOOK_NVS_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        ESP_LOGI(TAG, "no BR phonebook stored in NVS");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (len == 0U || len > BOS_PHONEBOOK_BODY_MAX) {
        nvs_close(handle);
        ESP_LOGW(TAG, "stored BR phonebook ignored: invalid size=%u", (unsigned)len);
        return ESP_OK;
    }

    char *raw = malloc(len);
    if (!raw) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(handle, BOS_PHONEBOOK_NVS_KEY, raw, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }
    raw[len - 1U] = '\0';

    /* Heap-allocated: bos_phonebook_t is ~39 KB and overflows task stacks. */
    bos_phonebook_t *book = calloc(1, sizeof(*book));
    if (!book) {
        free(raw);
        return ESP_ERR_NO_MEM;
    }
    char computed_digest[BOS_PHONEBOOK_DIGEST_HEX_LEN + 1];
    char parse_error[96];
    err = phonebook_parse_csv(raw, book, computed_digest, parse_error, sizeof(parse_error));
    if (err == ESP_OK) {
        /* Advertise the stored phonebook version in the /mesh/have target on
         * boot. announce=false: a re-energised node self-heals via its own
         * boot-time neighbour compare, so the BR does not nudge at boot. */
        esp_err_t seed = bos_ledger_mesh_serve_publish_phonebook(raw, strlen(raw), book->version, book->digest, false);
        if (seed != ESP_OK) {
            ESP_LOGW(TAG, "phonebook mesh target seed (boot) failed: %s", esp_err_to_name(seed));
        }
    }
    free(raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stored BR phonebook ignored: %s", parse_error);
        free(book);
        return ESP_OK;
    }
    err = phonebook_apply(book);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "loaded BR phonebook from NVS: version=%s digest=%s rows=%u",
                 book->version, book->digest, (unsigned)book->row_count);
    }
    free(book);
    return err;
}

/* One-time phonebook state init: create the lock, then hydrate from NVS.
 * Extracted verbatim from the top of bos_diagnostics_server_start(). */
esp_err_t bos_diag_phonebook_init(void)
{
    if (!s_phonebook_lock) {
        s_phonebook_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_phonebook_lock != NULL, ESP_ERR_NO_MEM, TAG, "failed to create phonebook lock");
        ESP_RETURN_ON_ERROR(phonebook_load_from_nvs(), TAG, "failed to load BR phonebook from NVS");
    }
    return ESP_OK;
}

static cJSON *phonebook_summary_json(const bos_phonebook_t *book)
{
    cJSON *phonebook = cJSON_CreateObject();
    if (!phonebook) {
        return NULL;
    }
    cJSON_AddStringToObject(phonebook, "storage", "nvs");
    cJSON_AddBoolToObject(phonebook, "loaded", book && book->loaded);
    cJSON_AddStringToObject(phonebook, "version", book && book->version[0] ? book->version : "");
    cJSON_AddStringToObject(phonebook, "digest", book && book->digest[0] ? book->digest : "");
    cJSON_AddStringToObject(phonebook, "generated_at", book && book->generated_at[0] ? book->generated_at : "");
    cJSON_AddNumberToObject(phonebook, "row_count", book ? (double)book->row_count : 0.0);
    cJSON_AddNumberToObject(phonebook, "columns_version", book ? (double)book->columns_version : 0.0);
    return phonebook;
}

/* GET /bos/phonebook/floating: the BR-authored floating book (v2 14-col CSV,
 * spec section 2.1). Read-only, open-on-LAN (same boundary as /bos/status):
 * the floating channel carries no commands and the gateway polls it. Never
 * fanned out to the mesh - there is deliberately no CoAP path here. */
esp_err_t phonebook_floating_get_handler(httpd_req_t *req)
{
    char *csv = (char *)malloc(BOS_FLOATING_RENDER_MAX);
    if (!csv) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    int written = bos_floating_book_render_csv(csv, BOS_FLOATING_RENDER_MAX);
    if (written < 0) {
        free(csv);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to render floating book");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, csv);
    free(csv);
    return ESP_OK;
}

static cJSON *phonebook_mesh_push_json(const bos_phonebook_mesh_push_t *push)
{
    cJSON *mesh = cJSON_CreateObject();
    if (!mesh) {
        return NULL;
    }
    cJSON_AddNumberToObject(mesh, "attempted", push ? (double)push->attempted : 0.0);
    cJSON_AddNumberToObject(mesh, "sent", push ? (double)push->sent : 0.0);
    cJSON_AddNumberToObject(mesh, "failed", push ? (double)push->failed : 0.0);
    cJSON_AddNumberToObject(mesh, "bad_scope", push ? (double)push->bad_scope : 0.0);
    if (push && push->first_error[0] != '\0') {
        cJSON_AddStringToObject(mesh, "first_error", push->first_error);
    }
    return mesh;
}

esp_err_t phonebook_post_handler(httpd_req_t *req)
{
    if (!bos_commissioning_request_authorized(req)) {
        return send_unauthorized(req);
    }

    char *body = malloc(BOS_PHONEBOOK_BODY_MAX);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    if (read_request_body(req, body, BOS_PHONEBOOK_BODY_MAX) < 0) {
        free(body);
        return send_status_json(req, "400 Bad Request", "empty or oversized body");
    }

    /* Heap-allocated: bos_phonebook_t is ~39 KB and overflows task stacks. */
    bos_phonebook_t *next = calloc(1, sizeof(*next));
    if (!next) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    char computed_digest[BOS_PHONEBOOK_DIGEST_HEX_LEN + 1];
    char parse_error[96];
    esp_err_t err = phonebook_parse_csv(body, next, computed_digest, parse_error, sizeof(parse_error));
    if (err != ESP_OK) {
        free(body);
        free(next);
        return send_status_json(req, "400 Bad Request", parse_error);
    }
    err = phonebook_validate_push_headers(req, next, computed_digest, parse_error, sizeof(parse_error));
    if (err != ESP_OK) {
        free(body);
        free(next);
        return send_status_json(req, "400 Bad Request", parse_error);
    }

    err = phonebook_store_raw(body);
    if (err != ESP_OK) {
        free(body);
        free(next);
        ESP_LOGE(TAG, "failed to store BR phonebook in NVS: %s", esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", "failed to persist phonebook");
    }
    err = phonebook_apply(next);
    if (err != ESP_OK) {
        free(body);
        free(next);
        ESP_LOGE(TAG, "failed to apply BR phonebook: %s", esp_err_to_name(err));
        return send_status_json(req, "500 Internal Server Error", "failed to apply phonebook");
    }
    bos_phonebook_mesh_push_t mesh_push;
    phonebook_push_to_mesh(next, body, &mesh_push);

    /* Additive to the PUSH above (not a replacement): advertise the new version
     * in the /mesh/have phonebook target and nudge the live mesh so nodes that
     * already have an older book wake their pull/gossip and leech the new one
     * viral. announce=true only on this fresh gateway ingest. */
    esp_err_t seed = bos_ledger_mesh_serve_publish_phonebook(body, strlen(body), next->version, next->digest, true);
    if (seed != ESP_OK) {
        ESP_LOGE(TAG, "phonebook mesh target/announce failed: %s", esp_err_to_name(seed));
    }
    free(body);

    ESP_LOGI(TAG,
             "stored BR phonebook: version=%s digest=%s generated_at=%s rows=%u mesh_sent=%u mesh_failed=%u",
             next->version, next->digest, next->generated_at, (unsigned)next->row_count,
             (unsigned)mesh_push.sent, (unsigned)mesh_push.failed);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(next);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON *phonebook = phonebook_summary_json(next);
    free(next);
    if (!phonebook) {
        cJSON_Delete(resp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    cJSON_AddItemToObject(resp, "phonebook", phonebook);
    cJSON *mesh = phonebook_mesh_push_json(&mesh_push);
    if (!mesh) {
        cJSON_Delete(resp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    cJSON_AddItemToObject(resp, "mesh_push", mesh);
    esp_err_t ret = send_cjson(req, resp);
    cJSON_Delete(resp);
    return ret;
}

bool phonebook_resolve_eui64(otInstance *instance,
                             const char *eui64,
                             proxy_target_t *out,
                             bool *matched_out,
                             proxy_addr_scope_t *scope_out)
{
    if (matched_out) {
        *matched_out = false;
    }
    if (scope_out) {
        *scope_out = PROXY_SCOPE_UNUSABLE;
    }
    if (!instance || !eui64 || !out || !s_phonebook_lock) {
        return false;
    }

    bos_phonebook_row_t row = {0};
    char version[BOS_PHONEBOOK_VERSION_MAX] = "";
    bool found = false;
    if (xSemaphoreTake(s_phonebook_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "phonebook resolve %s: lock timeout", eui64);
        return false;
    }
    if (s_phonebook.loaded) {
        for (size_t i = 0; i < s_phonebook.row_count; i++) {
            if (strcmp(s_phonebook.rows[i].eui64, eui64) == 0) {
                row = s_phonebook.rows[i];
                memcpy(version, s_phonebook.version, sizeof(version));
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_phonebook_lock);

    if (!found) {
        return false;
    }
    if (matched_out) {
        *matched_out = true;
    }

    const otMeshLocalPrefix *mlp = otThreadGetMeshLocalPrefix(instance);
    proxy_addr_scope_t scope = proxy_classify_address(&row.address, mlp);
    if (scope_out) {
        *scope_out = scope;
    }
    if (scope != PROXY_SCOPE_MESH_LOCAL) {
        ESP_LOGW(TAG,
                 "phonebook resolve %s: eid=[%s] is not mesh-local scope=%d",
                 eui64, row.eid, (int)scope);
        return false;
    }

    out->address = row.address;
    out->http_port = 80;
    ESP_LOGI(TAG,
             "phonebook resolved trigger target: eui64=%s version=%s eid=[%s]",
             eui64, version, row.eid);
    return true;
}

/* Scan the operational phonebook for a row whose spatial_id matches; copies its
 * eui64 into out_eui64 (17 bytes). Returns true on a paired match. */
bool phonebook_resolve_spatial_to_eui64(const char *spatial_id, char out_eui64[17])
{
    if (!spatial_id || spatial_id[0] == '\0' || !out_eui64 || !s_phonebook_lock) {
        return false;
    }
    bool found = false;
    if (xSemaphoreTake(s_phonebook_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "spatial resolve %s: phonebook lock timeout", spatial_id);
        return false;
    }
    if (s_phonebook.loaded) {
        for (size_t i = 0; i < s_phonebook.row_count; i++) {
            if (s_phonebook.rows[i].spatial_id[0] != '\0' &&
                strcmp(s_phonebook.rows[i].spatial_id, spatial_id) == 0) {
                memcpy(out_eui64, s_phonebook.rows[i].eui64, sizeof(s_phonebook.rows[i].eui64));
                found = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_phonebook_lock);
    return found;
}
