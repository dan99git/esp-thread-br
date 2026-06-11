/**
 * Building OS Border Router: LAN-direct commissioning identity.
 */

#include "bos_commissioning.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_server_registration.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "bos_commission";

#define BOS_COMMISSION_BODY_MAX 2048U
#define BOS_COMMISSION_TOKEN_MAX 128

static esp_err_t header_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return ESP_ERR_NOT_FOUND;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_len);
}

/* Same trust anchor as /bos/ledger/push (bos_ledger_ingress.c
 * push_authorized): the BR's own persisted device_token, with the
 * compile-time CONFIG_BOS_SERVER_API_KEY as the same alternative.
 * Exported so the phase 2b joiner routes (bos_joiner.c) enforce the
 * identical auth boundary. */
bool bos_commissioning_request_authorized(httpd_req_t *req)
{
    char expected[BOS_COMMISSION_TOKEN_MAX];
    char provided[BOS_COMMISSION_TOKEN_MAX];

    if (bos_server_registration_get_token(expected, sizeof(expected)) == ESP_OK && expected[0] != '\0') {
        if (header_value(req, "X-Device-Token", provided, sizeof(provided)) == ESP_OK &&
            strcmp(expected, provided) == 0) {
            return true;
        }
    }

#ifdef CONFIG_BOS_SERVER_API_KEY
    if (CONFIG_BOS_SERVER_API_KEY[0] != '\0') {
        if (header_value(req, "x-api-key", provided, sizeof(provided)) == ESP_OK &&
            strcmp(CONFIG_BOS_SERVER_API_KEY, provided) == 0) {
            return true;
        }
    }
#endif

    return false;
}

/* Splits site-building-level-space-device on '-'. Exactly five non-empty
 * components required. Same algorithm as the mesh-device identity parse
 * (firmware/xiao-esp32c6/components/sdcard/device_config.c
 * parse_spatial_id_components). */
static bool parse_spatial_id_components(const char *spatial_id,
                                        char *site_id, size_t site_id_len,
                                        char *building, size_t building_len,
                                        char *level, size_t level_len,
                                        char *space, size_t space_len,
                                        char *device_index, size_t device_index_len)
{
    if (!spatial_id || spatial_id[0] == '\0') {
        return false;
    }

    char *parts[5] = {site_id, building, level, space, device_index};
    size_t sizes[5] = {site_id_len, building_len, level_len, space_len, device_index_len};
    size_t lens[5] = {0};
    size_t part = 0;

    for (const char *cursor = spatial_id; ; ++cursor) {
        char ch = *cursor;
        if (ch == '-' || ch == '\0') {
            if (lens[part] == 0U) {
                return false;
            }
            parts[part][lens[part]] = '\0';
            if (ch == '\0') {
                return part == 4U;
            }
            part++;
            if (part >= 5U) {
                return false;
            }
            continue;
        }

        if (lens[part] + 1U >= sizes[part]) {
            return false;
        }
        parts[part][lens[part]++] = ch;
    }
}

/* Accepts a JSON array of strings or a single CSV string (the same shapes the
 * mesh-device /api/commission accepts). */
static void request_tags_to_csv(const cJSON *tags, char *out, size_t out_len)
{
    if (!out || out_len == 0U) {
        return;
    }
    out[0] = '\0';

    if (cJSON_IsString(tags) && tags->valuestring) {
        snprintf(out, out_len, "%s", tags->valuestring);
        return;
    }

    if (!cJSON_IsArray(tags)) {
        return;
    }

    size_t pos = 0;
    const cJSON *tag = NULL;
    cJSON_ArrayForEach(tag, tags) {
        if (!cJSON_IsString(tag) || !tag->valuestring) {
            continue;
        }
        size_t tag_len = strlen(tag->valuestring);
        size_t extra = (pos > 0 ? 1U : 0U) + tag_len;
        if (pos + extra >= out_len) {
            break;
        }
        if (pos > 0) {
            out[pos++] = ',';
        }
        memcpy(out + pos, tag->valuestring, tag_len);
        pos += tag_len;
        out[pos] = '\0';
    }
}

/* If the body carries a broken-out component it must match the value derived
 * from spatial_id; a contradiction is an honest 400, never a silent pick. */
static bool optional_field_conflicts(const cJSON *root, const char *key, const char *derived)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) {
        return false;
    }
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return false;
    }
    return strcmp(item->valuestring, derived) != 0;
}

static esp_err_t persist_identity(const bos_commissioning_identity_t *id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_IDENTITY_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, BOS_IDENTITY_KEY_SPATIAL, id->spatial_id);
    if (err == ESP_OK) {
        err = nvs_set_str(h, BOS_IDENTITY_KEY_SITE, id->site_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, BOS_IDENTITY_KEY_BUILDING, id->building);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, BOS_IDENTITY_KEY_LEVEL, id->level);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, BOS_IDENTITY_KEY_SPACE, id->space);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, BOS_IDENTITY_KEY_DEV_INDEX, id->device_index);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, BOS_IDENTITY_KEY_CLASS, id->device_class);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, BOS_IDENTITY_KEY_TAGS, id->tags_csv);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t read_identity_str(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

esp_err_t bos_commissioning_get_identity(bos_commissioning_identity_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_IDENTITY_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* never commissioned: namespace does not exist yet */
    }
    if (err != ESP_OK) {
        return err;
    }

    /* spatial_id is the commissioning marker: absent means not commissioned. */
    if (read_identity_str(h, BOS_IDENTITY_KEY_SPATIAL, out->spatial_id, sizeof(out->spatial_id)) != ESP_OK) {
        nvs_close(h);
        return ESP_OK;
    }

    read_identity_str(h, BOS_IDENTITY_KEY_SITE, out->site_id, sizeof(out->site_id));
    read_identity_str(h, BOS_IDENTITY_KEY_BUILDING, out->building, sizeof(out->building));
    read_identity_str(h, BOS_IDENTITY_KEY_LEVEL, out->level, sizeof(out->level));
    read_identity_str(h, BOS_IDENTITY_KEY_SPACE, out->space, sizeof(out->space));
    read_identity_str(h, BOS_IDENTITY_KEY_DEV_INDEX, out->device_index, sizeof(out->device_index));
    read_identity_str(h, BOS_IDENTITY_KEY_CLASS, out->device_class, sizeof(out->device_class));
    read_identity_str(h, BOS_IDENTITY_KEY_TAGS, out->tags_csv, sizeof(out->tags_csv));
    out->commissioned = true;
    nvs_close(h);
    return ESP_OK;
}

/* Builds the JSON through cJSON so string escaping is handled in one place. */
static cJSON *identity_to_json_obj(const bos_commissioning_identity_t *id, bool include_ok)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    bool added = true;
    if (include_ok) {
        added = added && cJSON_AddBoolToObject(obj, "ok", true) != NULL;
    }
    added = added && cJSON_AddBoolToObject(obj, "commissioned", true) != NULL;
    added = added && cJSON_AddStringToObject(obj, "spatial_id", id->spatial_id) != NULL;
    added = added && cJSON_AddStringToObject(obj, "site_id", id->site_id) != NULL;
    added = added && cJSON_AddStringToObject(obj, "building", id->building) != NULL;
    added = added && cJSON_AddStringToObject(obj, "level", id->level) != NULL;
    added = added && cJSON_AddStringToObject(obj, "space", id->space) != NULL;
    added = added && cJSON_AddStringToObject(obj, "device_index", id->device_index) != NULL;
    added = added && cJSON_AddStringToObject(obj, "device_class", id->device_class) != NULL;
    added = added && cJSON_AddStringToObject(obj, "tags", id->tags_csv) != NULL;
    if (!added) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

int bos_commissioning_identity_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }

    bos_commissioning_identity_t id;
    if (bos_commissioning_get_identity(&id) != ESP_OK || !id.commissioned) {
        int written = snprintf(out, out_len, "null");
        return (written < 0 || written >= (int)out_len) ? -1 : written;
    }

    cJSON *obj = identity_to_json_obj(&id, false);
    if (!obj) {
        return -1;
    }
    char *printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!printed) {
        return -1;
    }
    int written = snprintf(out, out_len, "%s", printed);
    cJSON_free(printed);
    return (written < 0 || written >= (int)out_len) ? -1 : written;
}

/* Error paths return ESP_OK once a response has been sent: a non-ESP_OK
 * handler return makes esp_http_server close the socket immediately and skip
 * the unread-body purge, so the close emits a TCP RST that destroys the
 * in-flight response (observed on hardware; same rule as
 * bos_ledger_ingress_http_push / bos_diagnostics_server send paths).
 * ESP_FAIL only when the socket itself is unusable. */
static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *error)
{
    char json[160];
    int written = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"internal error\"}");
    } else {
        httpd_resp_sendstr(req, json);
    }
    return ESP_OK;
}

esp_err_t bos_commissioning_http_post(httpd_req_t *req)
{
    if (!bos_commissioning_request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }

    if (req->content_len == 0 || req->content_len > BOS_COMMISSION_BODY_MAX) {
        return send_error_json(req, "400 Bad Request", "body must be 1..2048 bytes");
    }

    char *body = (char *)malloc(req->content_len + 1);
    if (!body) {
        return send_error_json(req, "500 Internal Server Error", "out of memory");
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            /* Socket-level receive failure: connection is unusable, close it. */
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    body[req->content_len] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_error_json(req, "400 Bad Request", "invalid JSON");
    }

    const cJSON *spatial = cJSON_GetObjectItemCaseSensitive(root, "spatial_id");
    const cJSON *device_class = cJSON_GetObjectItemCaseSensitive(root, "device_class");
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(root, "tags");
    if (!cJSON_IsString(spatial) || !spatial->valuestring ||
        !cJSON_IsString(device_class) || !device_class->valuestring || device_class->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "spatial_id and device_class are required strings");
    }

    bos_commissioning_identity_t id;
    memset(&id, 0, sizeof(id));
    if (strlen(spatial->valuestring) >= sizeof(id.spatial_id)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "spatial_id too long");
    }
    snprintf(id.spatial_id, sizeof(id.spatial_id), "%s", spatial->valuestring);

    if (!parse_spatial_id_components(id.spatial_id,
                                     id.site_id, sizeof(id.site_id),
                                     id.building, sizeof(id.building),
                                     id.level, sizeof(id.level),
                                     id.space, sizeof(id.space),
                                     id.device_index, sizeof(id.device_index))) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "spatial_id must be site-building-level-space-device");
    }

    if (optional_field_conflicts(root, "site_id", id.site_id) ||
        optional_field_conflicts(root, "building", id.building) ||
        optional_field_conflicts(root, "level", id.level) ||
        optional_field_conflicts(root, "space", id.space) ||
        optional_field_conflicts(root, "device_index", id.device_index)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "identity fields do not match spatial_id");
    }

    if (strlen(device_class->valuestring) >= sizeof(id.device_class)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "device_class too long");
    }
    snprintf(id.device_class, sizeof(id.device_class), "%s", device_class->valuestring);
    request_tags_to_csv(tags, id.tags_csv, sizeof(id.tags_csv));
    cJSON_Delete(root);

    esp_err_t err = persist_identity(&id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist commissioned identity: %s", esp_err_to_name(err));
        return send_error_json(req, "500 Internal Server Error", "failed to persist device identity");
    }

    ESP_LOGI(TAG, "commissioned as %s (class=%s)", id.spatial_id, id.device_class);

    id.commissioned = true;
    cJSON *resp = identity_to_json_obj(&id, true);
    char *printed = resp ? cJSON_PrintUnformatted(resp) : NULL;
    cJSON_Delete(resp);
    if (!printed) {
        /* Identity is persisted; report the response failure honestly. */
        return send_error_json(req, "500 Internal Server Error", "identity persisted but response render failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, printed);
    cJSON_free(printed);
    return ESP_OK;
}
