/**
 * Building OS Border Router: app firmware OTA stream engine.
 *
 * Image descriptor validation, the streamed OTA write path, restart and
 * rollback scheduling, and the OTA lifecycle entry points. The HTTP
 * surface lives in bos_br_ota.c.
 */

#include "bos_br_ota.h"
#include "bos_br_ota_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
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

SemaphoreHandle_t bos_br_ota_operation_lock;
bos_br_ota_status_t bos_br_ota_status = {
    .state = BOS_BR_OTA_STATE_IDLE,
};

const char *state_str(bos_br_ota_state_t state)
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

const char *ota_img_state_str(esp_ota_img_states_t state)
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

bool parse_sha256_hex(const char *hex, uint8_t out[BOS_BR_OTA_DIGEST_LEN])
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

void set_last_error(const char *message)
{
    snprintf(bos_br_ota_status.last_error, sizeof(bos_br_ota_status.last_error), "%s", message ? message : "unknown error");
    bos_br_ota_status.state = BOS_BR_OTA_STATE_FAILED;
}

void set_last_error_err(const char *prefix, esp_err_t err)
{
    snprintf(bos_br_ota_status.last_error,
             sizeof(bos_br_ota_status.last_error),
             "%s: %s",
             prefix ? prefix : "error",
             esp_err_to_name(err));
    bos_br_ota_status.state = BOS_BR_OTA_STATE_FAILED;
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

    snprintf(bos_br_ota_status.new_version, sizeof(bos_br_ota_status.new_version), "%s", new_version);
    return ESP_OK;
}

esp_err_t upload_reader(void *ctx, uint8_t *out, size_t max_len, size_t *out_len)
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

esp_err_t fetch_reader(void *ctx, uint8_t *out, size_t max_len, size_t *out_len)
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
        bos_br_ota_status.received += (uint32_t)read_len;
    }
    return ESP_OK;
}

esp_err_t perform_ota_stream(const char *source,
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

    memset(&bos_br_ota_status, 0, sizeof(bos_br_ota_status));
    bos_br_ota_status.state = BOS_BR_OTA_STATE_RECEIVING;
    bos_br_ota_status.expected_size = image_size;
    snprintf(bos_br_ota_status.source, sizeof(bos_br_ota_status.source), "%s", source ? source : "unknown");
    snprintf(bos_br_ota_status.target_partition, sizeof(bos_br_ota_status.target_partition), "%s", target->label);
    snprintf(bos_br_ota_status.expected_sha256, sizeof(bos_br_ota_status.expected_sha256), "%s", expected_digest_hex);

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    esp_err_t err = read_exact(reader, reader_ctx, header, sizeof(header), &sha_ctx);
    if (err != ESP_OK) {
        set_last_error_err("failed to read OTA image header", err);
        goto fail;
    }

    bos_br_ota_status.state = BOS_BR_OTA_STATE_VALIDATING;
    err = validate_image_descriptor(header, image_size, target, &new_desc);
    if (err != ESP_OK) {
        set_last_error_err("OTA image descriptor rejected", err);
        goto fail;
    }

    bos_br_ota_status.state = BOS_BR_OTA_STATE_WRITING;
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

    while (bos_br_ota_status.received < image_size) {
        size_t remaining = (size_t)image_size - bos_br_ota_status.received;
        size_t to_read = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        size_t read_len = 0;
        err = reader(reader_ctx, chunk, to_read, &read_len);
        if (err != ESP_OK || read_len == 0) {
            set_last_error_err("failed to receive OTA image body", err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err);
            goto fail;
        }

        mbedtls_sha256_update(&sha_ctx, chunk, read_len);
        bos_br_ota_status.received += (uint32_t)read_len;

        err = esp_ota_write(handle, chunk, read_len);
        if (err != ESP_OK) {
            set_last_error_err("esp_ota_write body failed", err);
            goto fail;
        }
    }

    mbedtls_sha256_finish(&sha_ctx, actual_digest);
    bytes_to_hex(actual_digest, bos_br_ota_status.actual_sha256);
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

    bos_br_ota_status.state = BOS_BR_OTA_STATE_READY_TO_REBOOT;
    bos_br_ota_status.reboot_pending = true;
    return ESP_OK;

fail:
    mbedtls_sha256_finish(&sha_ctx, actual_digest);
    bytes_to_hex(actual_digest, bos_br_ota_status.actual_sha256);
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
    bos_br_ota_status.state = BOS_BR_OTA_STATE_REBOOTING;
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

void schedule_restart(void)
{
    BaseType_t ok = xTaskCreate(delayed_restart_task, "bos_ota_reboot", 2048, NULL, 5, NULL);
    if (ok != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
}

void schedule_rollback(void)
{
    BaseType_t ok = xTaskCreate(delayed_rollback_task, "bos_ota_rollback", 3072, NULL, 5, NULL);
    if (ok != pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

esp_err_t bos_br_ota_init(void)
{
    if (bos_br_ota_operation_lock == NULL) {
        bos_br_ota_operation_lock = xSemaphoreCreateMutex();
        if (bos_br_ota_operation_lock == NULL) {
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

    bos_br_ota_status.state = BOS_BR_OTA_STATE_IDLE;
    bos_br_ota_status.reboot_pending = false;
    bos_br_ota_status.last_error[0] = '\0';
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
