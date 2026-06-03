/**
 * Building OS Border Router: app firmware OTA over the LAN-side BR HTTP server.
 */

#include "bos_br_ota.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_server_registration.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

static const char *TAG = "bos_br_ota";

#define BOS_BR_OTA_PROJECT_NAME "building-os-border-router"
#define BOS_BR_OTA_SHA256_HEX_LEN 64
#define BOS_BR_OTA_DIGEST_LEN 32
#define BOS_BR_OTA_HEADER_BYTES (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define BOS_BR_OTA_CHUNK_BYTES 2048
#define BOS_BR_OTA_HEADER_VALUE_MAX 128
#define BOS_BR_OTA_URL_MAX 256
#define BOS_BR_OTA_FETCH_BODY_MAX 512

typedef enum {
    BOS_BR_OTA_STATE_IDLE = 0,
    BOS_BR_OTA_STATE_RECEIVING,
    BOS_BR_OTA_STATE_VALIDATING,
    BOS_BR_OTA_STATE_WRITING,
    BOS_BR_OTA_STATE_READY_TO_REBOOT,
    BOS_BR_OTA_STATE_REBOOTING,
    BOS_BR_OTA_STATE_FAILED,
    BOS_BR_OTA_STATE_CONFIRMING,
    BOS_BR_OTA_STATE_ROLLING_BACK,
} bos_br_ota_state_t;

typedef struct {
    bos_br_ota_state_t state;
    bool reboot_pending;
    char source[16];
    char target_partition[17];
    char new_version[32];
    char expected_sha256[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    char actual_sha256[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    char last_error[96];
    uint32_t expected_size;
    uint32_t received;
} bos_br_ota_status_t;

typedef esp_err_t (*bos_br_ota_reader_t)(void *ctx, uint8_t *out, size_t max_len, size_t *out_len);

typedef struct {
    httpd_req_t *req;
    size_t remaining;
} bos_br_ota_upload_ctx_t;

typedef struct {
    esp_http_client_handle_t client;
    size_t remaining;
} bos_br_ota_fetch_ctx_t;

static SemaphoreHandle_t s_operation_lock;
static bos_br_ota_status_t s_status = {
    .state = BOS_BR_OTA_STATE_IDLE,
};

static const char *state_str(bos_br_ota_state_t state)
{
    switch (state) {
    case BOS_BR_OTA_STATE_IDLE:
        return "idle";
    case BOS_BR_OTA_STATE_RECEIVING:
        return "receiving";
    case BOS_BR_OTA_STATE_VALIDATING:
        return "validating";
    case BOS_BR_OTA_STATE_WRITING:
        return "writing";
    case BOS_BR_OTA_STATE_READY_TO_REBOOT:
        return "ready_to_reboot";
    case BOS_BR_OTA_STATE_REBOOTING:
        return "rebooting";
    case BOS_BR_OTA_STATE_FAILED:
        return "failed";
    case BOS_BR_OTA_STATE_CONFIRMING:
        return "confirming";
    case BOS_BR_OTA_STATE_ROLLING_BACK:
        return "rolling_back";
    default:
        return "unknown";
    }
}

static const char *ota_img_state_str(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
        return "undefined";
    default:
        return "unknown";
    }
}

static void copy_app_field(char *out, size_t out_len, const char *field, size_t field_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t copy_len = 0;
    while (copy_len < field_len && field[copy_len] != '\0') {
        copy_len++;
    }
    if (copy_len >= out_len) {
        copy_len = out_len - 1;
    }
    memcpy(out, field, copy_len);
    out[copy_len] = '\0';
}

static int json_string_or_null(char *out, size_t out_len, const char *value)
{
    if (!value || value[0] == '\0') {
        return snprintf(out, out_len, "null");
    }

    size_t pos = 0;
    if (out_len == 0) {
        return -1;
    }
    out[pos++] = '"';
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        const char *escape = NULL;
        char unicode_escape[7];
        switch (*p) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (*p < 0x20) {
                snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", *p);
                escape = unicode_escape;
            }
            break;
        }

        if (escape) {
            size_t len = strlen(escape);
            if (pos + len >= out_len) {
                return -1;
            }
            memcpy(out + pos, escape, len);
            pos += len;
        } else {
            if (pos + 1 >= out_len) {
                return -1;
            }
            out[pos++] = (char)*p;
        }
    }
    if (pos + 1 >= out_len) {
        return -1;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return (int)pos;
}

static void bytes_to_hex(const uint8_t bytes[BOS_BR_OTA_DIGEST_LEN],
                         char out[BOS_BR_OTA_SHA256_HEX_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < BOS_BR_OTA_DIGEST_LEN; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[BOS_BR_OTA_SHA256_HEX_LEN] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_sha256_hex(const char *hex, uint8_t out[BOS_BR_OTA_DIGEST_LEN])
{
    if (!hex || strlen(hex) != BOS_BR_OTA_SHA256_HEX_LEN) {
        return false;
    }

    for (size_t i = 0; i < BOS_BR_OTA_DIGEST_LEN; i++) {
        int high = hex_value(hex[i * 2]);
        int low = hex_value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static esp_err_t header_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return ESP_ERR_NOT_FOUND;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_len);
}

static bool request_authorized(httpd_req_t *req)
{
    char expected[BOS_BR_OTA_HEADER_VALUE_MAX];
    char provided[BOS_BR_OTA_HEADER_VALUE_MAX];

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

static void send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}

static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *error)
{
    char error_json[192];
    char error_value[160];

    if (json_string_or_null(error_value, sizeof(error_value), error) < 0) {
        snprintf(error_value, sizeof(error_value), "\"error\"");
    }
    snprintf(error_json, sizeof(error_json), "{\"ok\":false,\"error\":%s}", error_value);

    httpd_resp_set_status(req, status);
    send_json(req, error_json);
    return ESP_FAIL;
}

static bool begin_operation(httpd_req_t *req)
{
    if (s_operation_lock == NULL) {
        if (bos_br_ota_init() != ESP_OK) {
            send_error_json(req, "500 Internal Server Error", "OTA lock unavailable");
            return false;
        }
    }
    if (xSemaphoreTake(s_operation_lock, 0) != pdTRUE) {
        send_error_json(req, "409 Conflict", "OTA operation already in progress");
        return false;
    }
    if (s_status.reboot_pending ||
        s_status.state == BOS_BR_OTA_STATE_READY_TO_REBOOT ||
        s_status.state == BOS_BR_OTA_STATE_REBOOTING ||
        s_status.state == BOS_BR_OTA_STATE_ROLLING_BACK) {
        xSemaphoreGive(s_operation_lock);
        send_error_json(req, "409 Conflict", "OTA reboot already pending");
        return false;
    }
    return true;
}

static void end_operation(void)
{
    if (s_operation_lock != NULL) {
        xSemaphoreGive(s_operation_lock);
    }
}

static void set_last_error(const char *message)
{
    snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", message ? message : "unknown error");
    s_status.state = BOS_BR_OTA_STATE_FAILED;
}

static void set_last_error_err(const char *prefix, esp_err_t err)
{
    snprintf(s_status.last_error,
             sizeof(s_status.last_error),
             "%s: %s",
             prefix ? prefix : "error",
             esp_err_to_name(err));
    s_status.state = BOS_BR_OTA_STATE_FAILED;
}

static esp_err_t parse_semver(const char *version, int out[3])
{
    if (!version || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *p = version;
    for (size_t part = 0; part < 3; part++) {
        if (*p < '0' || *p > '9') {
            return ESP_ERR_INVALID_ARG;
        }
        int value = 0;
        while (*p >= '0' && *p <= '9') {
            value = (value * 10) + (*p - '0');
            if (value > 9999) {
                return ESP_ERR_INVALID_ARG;
            }
            p++;
        }
        out[part] = value;
        if (part < 2) {
            if (*p != '.') {
                return ESP_ERR_INVALID_ARG;
            }
            p++;
        }
    }

    return *p == '\0' ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static int compare_semver(const int left[3], const int right[3])
{
    for (size_t i = 0; i < 3; i++) {
        if (left[i] > right[i]) {
            return 1;
        }
        if (left[i] < right[i]) {
            return -1;
        }
    }
    return 0;
}

static esp_err_t validate_image_descriptor(const uint8_t header[BOS_BR_OTA_HEADER_BYTES],
                                           uint32_t image_size,
                                           const esp_partition_t *target,
                                           esp_app_desc_t *new_desc)
{
    if (!header || !target || !new_desc) {
        return ESP_ERR_INVALID_ARG;
    }
    if (image_size == 0 || image_size > target->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_image_header_t *image_header = (const esp_image_header_t *)header;
    if (image_header->magic != ESP_IMAGE_HEADER_MAGIC) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
#ifdef CONFIG_IDF_FIRMWARE_CHIP_ID
    if ((int)image_header->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
#endif

    memcpy(new_desc,
           header + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(*new_desc));
    if (new_desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (strncmp(new_desc->project_name, BOS_BR_OTA_PROJECT_NAME, sizeof(new_desc->project_name)) != 0) {
        return ESP_ERR_INVALID_VERSION;
    }

    const esp_app_desc_t *running_desc = esp_app_get_description();
    if (!running_desc) {
        return ESP_ERR_INVALID_STATE;
    }
    if (new_desc->secure_version < running_desc->secure_version) {
        return ESP_ERR_OTA_SMALL_SEC_VER;
    }

    char new_version[32];
    char running_version[32];
    copy_app_field(new_version, sizeof(new_version), new_desc->version, sizeof(new_desc->version));
    copy_app_field(running_version, sizeof(running_version), running_desc->version, sizeof(running_desc->version));

    int new_semver[3];
    int running_semver[3];
    if (parse_semver(new_version, new_semver) != ESP_OK ||
        parse_semver(running_version, running_semver) != ESP_OK) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (compare_semver(new_semver, running_semver) <= 0) {
        return ESP_ERR_INVALID_VERSION;
    }

    snprintf(s_status.new_version, sizeof(s_status.new_version), "%s", new_version);
    return ESP_OK;
}

static esp_err_t upload_reader(void *ctx, uint8_t *out, size_t max_len, size_t *out_len)
{
    bos_br_ota_upload_ctx_t *upload = (bos_br_ota_upload_ctx_t *)ctx;
    if (!upload || !out || !out_len || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (upload->remaining == 0) {
        *out_len = 0;
        return ESP_OK;
    }

    size_t to_read = upload->remaining < max_len ? upload->remaining : max_len;
    int ret = httpd_req_recv(upload->req, (char *)out, to_read);
    if (ret <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    upload->remaining -= (size_t)ret;
    *out_len = (size_t)ret;
    return ESP_OK;
}

static esp_err_t fetch_reader(void *ctx, uint8_t *out, size_t max_len, size_t *out_len)
{
    bos_br_ota_fetch_ctx_t *fetch = (bos_br_ota_fetch_ctx_t *)ctx;
    if (!fetch || !out || !out_len || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fetch->remaining == 0) {
        *out_len = 0;
        return ESP_OK;
    }

    size_t to_read = fetch->remaining < max_len ? fetch->remaining : max_len;
    int ret = esp_http_client_read(fetch->client, (char *)out, (int)to_read);
    if (ret <= 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    fetch->remaining -= (size_t)ret;
    *out_len = (size_t)ret;
    return ESP_OK;
}

static esp_err_t read_exact(bos_br_ota_reader_t reader,
                            void *ctx,
                            uint8_t *out,
                            size_t len,
                            mbedtls_sha256_context *sha_ctx)
{
    size_t received = 0;
    while (received < len) {
        size_t read_len = 0;
        esp_err_t err = reader(ctx, out + received, len - received, &read_len);
        if (err != ESP_OK || read_len == 0) {
            return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
        }
        mbedtls_sha256_update(sha_ctx, out + received, read_len);
        received += read_len;
        s_status.received += (uint32_t)read_len;
    }
    return ESP_OK;
}

static esp_err_t perform_ota_stream(const char *source,
                                    uint32_t image_size,
                                    const uint8_t expected_digest[BOS_BR_OTA_DIGEST_LEN],
                                    const char *expected_digest_hex,
                                    bos_br_ota_reader_t reader,
                                    void *reader_ctx)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t handle = 0;
    bool ota_started = false;
    esp_app_desc_t new_desc = {0};
    uint8_t header[BOS_BR_OTA_HEADER_BYTES];
    uint8_t chunk[BOS_BR_OTA_CHUNK_BYTES];
    uint8_t actual_digest[BOS_BR_OTA_DIGEST_LEN];
    mbedtls_sha256_context sha_ctx;

    if (!running || !target || target == running) {
        set_last_error("inactive OTA app partition unavailable");
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size < BOS_BR_OTA_HEADER_BYTES || image_size > target->size) {
        set_last_error("OTA image size outside inactive app partition");
        return ESP_ERR_INVALID_SIZE;
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = BOS_BR_OTA_STATE_RECEIVING;
    s_status.expected_size = image_size;
    snprintf(s_status.source, sizeof(s_status.source), "%s", source ? source : "unknown");
    snprintf(s_status.target_partition, sizeof(s_status.target_partition), "%s", target->label);
    snprintf(s_status.expected_sha256, sizeof(s_status.expected_sha256), "%s", expected_digest_hex);

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    esp_err_t err = read_exact(reader, reader_ctx, header, sizeof(header), &sha_ctx);
    if (err != ESP_OK) {
        set_last_error_err("failed to read OTA image header", err);
        goto fail;
    }

    s_status.state = BOS_BR_OTA_STATE_VALIDATING;
    err = validate_image_descriptor(header, image_size, target, &new_desc);
    if (err != ESP_OK) {
        set_last_error_err("OTA image descriptor rejected", err);
        goto fail;
    }

    s_status.state = BOS_BR_OTA_STATE_WRITING;
    err = esp_ota_begin(target, image_size, &handle);
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_begin failed", err);
        goto fail;
    }
    ota_started = true;

    err = esp_ota_write(handle, header, sizeof(header));
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_write header failed", err);
        goto fail;
    }

    while (s_status.received < image_size) {
        size_t remaining = (size_t)image_size - s_status.received;
        size_t to_read = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        size_t read_len = 0;
        err = reader(reader_ctx, chunk, to_read, &read_len);
        if (err != ESP_OK || read_len == 0) {
            set_last_error_err("failed to receive OTA image body", err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err);
            goto fail;
        }

        mbedtls_sha256_update(&sha_ctx, chunk, read_len);
        s_status.received += (uint32_t)read_len;

        err = esp_ota_write(handle, chunk, read_len);
        if (err != ESP_OK) {
            set_last_error_err("esp_ota_write body failed", err);
            goto fail;
        }
    }

    mbedtls_sha256_finish(&sha_ctx, actual_digest);
    bytes_to_hex(actual_digest, s_status.actual_sha256);
    if (memcmp(actual_digest, expected_digest, BOS_BR_OTA_DIGEST_LEN) != 0) {
        set_last_error("OTA image SHA-256 mismatch");
        err = ESP_ERR_INVALID_CRC;
        goto fail_after_hash;
    }

    err = esp_ota_end(handle);
    ota_started = false;
    handle = 0;
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_end failed", err);
        goto fail_after_hash;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_set_boot_partition failed", err);
        goto fail_after_hash;
    }

    s_status.state = BOS_BR_OTA_STATE_READY_TO_REBOOT;
    s_status.reboot_pending = true;
    return ESP_OK;

fail:
    mbedtls_sha256_finish(&sha_ctx, actual_digest);
    bytes_to_hex(actual_digest, s_status.actual_sha256);
fail_after_hash:
    if (ota_started) {
        esp_ota_abort(handle);
    }
    return err;
}

static void delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_status.state = BOS_BR_OTA_STATE_REBOOTING;
    esp_restart();
    vTaskDelete(NULL);
}

static void delayed_rollback_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    set_last_error_err("rollback reboot failed", err);
    vTaskDelete(NULL);
}

static void schedule_restart(void)
{
    BaseType_t ok = xTaskCreate(delayed_restart_task, "bos_ota_reboot", 2048, NULL, 5, NULL);
    if (ok != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
}

static void schedule_rollback(void)
{
    BaseType_t ok = xTaskCreate(delayed_rollback_task, "bos_ota_rollback", 3072, NULL, 5, NULL);
    if (ok != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static esp_err_t read_json_body(httpd_req_t *req, char **out)
{
    if (!req || !out || req->content_len == 0 || req->content_len > BOS_BR_OTA_FETCH_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = (char *)malloc(req->content_len + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_ERR_INVALID_RESPONSE;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';
    *out = body;
    return ESP_OK;
}

static esp_err_t read_sha_header(httpd_req_t *req,
                                 uint8_t digest[BOS_BR_OTA_DIGEST_LEN],
                                 char digest_hex[BOS_BR_OTA_SHA256_HEX_LEN + 1])
{
    char value[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    if (header_value(req, "X-BOS-OTA-SHA256", value, sizeof(value)) != ESP_OK &&
        header_value(req, "x-bos-ota-sha256", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!parse_sha256_hex(value, digest)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(digest_hex, BOS_BR_OTA_SHA256_HEX_LEN + 1, "%s", value);
    return ESP_OK;
}

esp_err_t bos_br_ota_init(void)
{
    if (s_operation_lock == NULL) {
        s_operation_lock = xSemaphoreCreateMutex();
        if (s_operation_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        char version[32];
        copy_app_field(version, sizeof(version), desc->version, sizeof(desc->version));
        ESP_LOGI(TAG, "BR app OTA ready; running version=%s", version);
    } else {
        ESP_LOGI(TAG, "BR app OTA ready");
    }
    return ESP_OK;
}

esp_err_t bos_br_ota_confirm_running_app(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        set_last_error("running OTA app partition unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_get_state_partition failed", err);
        return err;
    }

    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_mark_app_valid_cancel_rollback failed", err);
        return err;
    }

    s_status.state = BOS_BR_OTA_STATE_IDLE;
    s_status.reboot_pending = false;
    s_status.last_error[0] = '\0';
    ESP_LOGI(TAG, "Running OTA app partition %s confirmed valid; rollback cancelled", running->label);
#endif
    return ESP_OK;
}

esp_err_t bos_br_ota_app_version(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_app_desc_t *desc = esp_app_get_description();
    if (!desc) {
        out[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    copy_app_field(out, out_len, desc->version, sizeof(desc->version));
    return ESP_OK;
}

int bos_br_ota_status_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    bool has_ota_state = running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK;
    char running_version[32] = "";
    char running_version_json[80];
    char target_json[64];
    char source_json[48];
    char expected_sha_json[96];
    char actual_sha_json[96];
    char new_version_json[80];
    char last_error_json[224];
    char running_label_json[64];
    char boot_label_json[64];
    uint32_t progress_percent = 0;

    bos_br_ota_app_version(running_version, sizeof(running_version));
    if (s_status.expected_size > 0) {
        progress_percent = (s_status.received * 100U) / s_status.expected_size;
        if (progress_percent > 100U) {
            progress_percent = 100U;
        }
    }

    if (json_string_or_null(running_version_json, sizeof(running_version_json), running_version) < 0 ||
        json_string_or_null(target_json, sizeof(target_json), s_status.target_partition) < 0 ||
        json_string_or_null(source_json, sizeof(source_json), s_status.source) < 0 ||
        json_string_or_null(expected_sha_json, sizeof(expected_sha_json), s_status.expected_sha256) < 0 ||
        json_string_or_null(actual_sha_json, sizeof(actual_sha_json), s_status.actual_sha256) < 0 ||
        json_string_or_null(new_version_json, sizeof(new_version_json), s_status.new_version) < 0 ||
        json_string_or_null(last_error_json, sizeof(last_error_json), s_status.last_error) < 0 ||
        json_string_or_null(running_label_json, sizeof(running_label_json), running ? running->label : NULL) < 0 ||
        json_string_or_null(boot_label_json, sizeof(boot_label_json), boot ? boot->label : NULL) < 0) {
        return -1;
    }

    return snprintf(out,
                    out_len,
                    "{\"state\":\"%s\",\"busy\":%s,\"running_partition\":%s,"
                    "\"boot_partition\":%s,\"running_version\":%s,"
                    "\"running_ota_state\":\"%s\",\"pending_verify\":%s,"
                    "\"rollback_possible\":%s,\"reboot_pending\":%s,"
                    "\"source\":%s,\"target_partition\":%s,\"expected_size_bytes\":%u,"
                    "\"received_bytes\":%u,\"progress_percent\":%u,"
                    "\"new_version\":%s,\"expected_sha256\":%s,\"actual_sha256\":%s,"
                    "\"last_error\":%s}",
                    state_str(s_status.state),
                    s_status.state != BOS_BR_OTA_STATE_IDLE && s_status.state != BOS_BR_OTA_STATE_FAILED ? "true" : "false",
                    running_label_json,
                    boot_label_json,
                    running_version_json,
                    has_ota_state ? ota_img_state_str(ota_state) : "unknown",
                    has_ota_state && ota_state == ESP_OTA_IMG_PENDING_VERIFY ? "true" : "false",
                    esp_ota_check_rollback_is_possible() ? "true" : "false",
                    s_status.reboot_pending ? "true" : "false",
                    source_json,
                    target_json,
                    (unsigned)s_status.expected_size,
                    (unsigned)s_status.received,
                    (unsigned)progress_percent,
                    new_version_json,
                    expected_sha_json,
                    actual_sha_json,
                    last_error_json);
}

esp_err_t bos_br_ota_http_status(httpd_req_t *req)
{
    char json[1536];
    int written = bos_br_ota_status_json(json, sizeof(json));
    if (written < 0 || written >= (int)sizeof(json)) {
        return send_error_json(req, "500 Internal Server Error", "OTA status too large");
    }
    send_json(req, json);
    return ESP_OK;
}

esp_err_t bos_br_ota_http_upload(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    uint8_t expected_digest[BOS_BR_OTA_DIGEST_LEN];
    char expected_digest_hex[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    esp_err_t err = read_sha_header(req, expected_digest, expected_digest_hex);
    if (err != ESP_OK) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "missing or invalid X-BOS-OTA-SHA256");
    }
    if (req->content_len == 0 || req->content_len > UINT32_MAX) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA image size");
    }

    bos_br_ota_upload_ctx_t ctx = {
        .req = req,
        .remaining = req->content_len,
    };

    err = perform_ota_stream("upload",
                             (uint32_t)req->content_len,
                             expected_digest,
                             expected_digest_hex,
                             upload_reader,
                             &ctx);
    if (err != ESP_OK) {
        char error[160];
        snprintf(error, sizeof(error), "%s", s_status.last_error[0] ? s_status.last_error : esp_err_to_name(err));
        end_operation();
        return send_error_json(req, "400 Bad Request", error);
    }

    char json[384];
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"mode\":\"upload\",\"size_bytes\":%u,"
             "\"version\":\"%s\",\"target_partition\":\"%s\",\"rebooting\":true}",
             (unsigned)s_status.expected_size,
             s_status.new_version,
             s_status.target_partition);
    send_json(req, json);
    end_operation();
    schedule_restart();
    return ESP_OK;
}

esp_err_t bos_br_ota_http_fetch(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    char *body = NULL;
    esp_err_t err = read_json_body(req, &body);
    if (err != ESP_OK) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA fetch JSON body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA fetch JSON");
    }

    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *sha_item = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    const cJSON *size_item = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!cJSON_IsString(url_item) || !url_item->valuestring ||
        strncmp(url_item->valuestring, "http://", 7) != 0 ||
        strlen(url_item->valuestring) >= BOS_BR_OTA_URL_MAX ||
        !cJSON_IsString(sha_item) || !sha_item->valuestring ||
        !cJSON_IsNumber(size_item) || size_item->valuedouble <= 0 ||
        size_item->valuedouble > (double)UINT32_MAX) {
        cJSON_Delete(root);
        end_operation();
        return send_error_json(req, "400 Bad Request", "OTA fetch requires http url, sha256, and size");
    }

    char url[BOS_BR_OTA_URL_MAX];
    char expected_digest_hex[BOS_BR_OTA_SHA256_HEX_LEN + 1];
    uint8_t expected_digest[BOS_BR_OTA_DIGEST_LEN];
    uint32_t image_size = (uint32_t)size_item->valuedouble;
    snprintf(url, sizeof(url), "%s", url_item->valuestring);
    snprintf(expected_digest_hex, sizeof(expected_digest_hex), "%s", sha_item->valuestring);
    cJSON_Delete(root);

    if (!parse_sha256_hex(expected_digest_hex, expected_digest)) {
        end_operation();
        return send_error_json(req, "400 Bad Request", "invalid OTA SHA-256");
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = BOS_BR_OTA_CHUNK_BYTES,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        end_operation();
        return send_error_json(req, "500 Internal Server Error", "failed to initialize OTA fetch client");
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        end_operation();
        return send_error_json(req, "400 Bad Request", "failed to open OTA fetch URL");
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300 ||
        (content_length >= 0 && (uint64_t)content_length != (uint64_t)image_size)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        end_operation();
        return send_error_json(req, "400 Bad Request", "OTA fetch URL returned invalid response");
    }

    bos_br_ota_fetch_ctx_t ctx = {
        .client = client,
        .remaining = image_size,
    };
    err = perform_ota_stream("fetch", image_size, expected_digest, expected_digest_hex, fetch_reader, &ctx);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        char error[160];
        snprintf(error, sizeof(error), "%s", s_status.last_error[0] ? s_status.last_error : esp_err_to_name(err));
        end_operation();
        return send_error_json(req, "400 Bad Request", error);
    }

    char json[384];
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"mode\":\"fetch\",\"size_bytes\":%u,"
             "\"version\":\"%s\",\"target_partition\":\"%s\",\"rebooting\":true}",
             (unsigned)s_status.expected_size,
             s_status.new_version,
             s_status.target_partition);
    send_json(req, json);
    end_operation();
    schedule_restart();
    return ESP_OK;
}

esp_err_t bos_br_ota_http_confirm(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    s_status.state = BOS_BR_OTA_STATE_CONFIRMING;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        set_last_error_err("esp_ota_mark_app_valid_cancel_rollback failed", err);
        end_operation();
        return send_error_json(req, "500 Internal Server Error", s_status.last_error);
    }

    s_status.state = BOS_BR_OTA_STATE_IDLE;
    s_status.reboot_pending = false;
    s_status.last_error[0] = '\0';
    send_json(req, "{\"ok\":true,\"confirmed\":true}");
    end_operation();
    return ESP_OK;
}

esp_err_t bos_br_ota_http_rollback(httpd_req_t *req)
{
    if (!request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }
    if (!begin_operation(req)) {
        return ESP_FAIL;
    }

    if (!esp_ota_check_rollback_is_possible()) {
        set_last_error("rollback app partition unavailable");
        end_operation();
        return send_error_json(req, "409 Conflict", s_status.last_error);
    }

    s_status.state = BOS_BR_OTA_STATE_ROLLING_BACK;
    s_status.reboot_pending = true;
    send_json(req, "{\"ok\":true,\"rollback\":true,\"rebooting\":true}");
    end_operation();
    schedule_rollback();
    return ESP_OK;
}
