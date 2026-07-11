/**
 * Building OS Border Router: app firmware OTA internals.
 *
 * Shared between bos_br_ota.c (HTTP surface) and bos_br_ota_stream.c
 * (image validation + OTA stream + reboot/rollback scheduling). Not a
 * public component surface.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* Shared OTA state; bos_br_ota_stream.c owns the definitions. */
extern SemaphoreHandle_t bos_br_ota_operation_lock;
extern bos_br_ota_status_t bos_br_ota_status;

/* bos_br_ota_stream.c */
const char *state_str(bos_br_ota_state_t state);
const char *ota_img_state_str(esp_ota_img_states_t state);
void set_last_error(const char *message);
void set_last_error_err(const char *prefix, esp_err_t err);
bool parse_sha256_hex(const char *hex, uint8_t out[BOS_BR_OTA_DIGEST_LEN]);
esp_err_t perform_ota_stream(const char *source,
                             uint32_t image_size,
                             const uint8_t expected_digest[BOS_BR_OTA_DIGEST_LEN],
                             const char *expected_digest_hex,
                             bos_br_ota_reader_t reader,
                             void *reader_ctx);
void schedule_restart(void);
void schedule_rollback(void);
esp_err_t upload_reader(void *ctx, uint8_t *out, size_t max_len, size_t *out_len);
esp_err_t fetch_reader(void *ctx, uint8_t *out, size_t max_len, size_t *out_len);

#ifdef __cplusplus
}
#endif
