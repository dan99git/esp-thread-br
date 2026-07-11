/**
 * Building OS Border Router: ledger ingress.
 *
 * Lifecycle state, last-error record, NVS/partition metadata, and the RAM
 * ledger store. The CBOR envelope validation lives in
 * bos_ledger_ingress_cbor.c and the protected push handler in
 * bos_ledger_ingress_push.c.
 */

#include "bos_ledger_ingress.h"
#include "bos_ledger_ingress_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "bos_ingress";

#define BOS_LEDGER_PARTITION_SUBTYPE 0x99
#define BOS_LEDGER_ERROR_PHASE_MAX 32
#define BOS_LEDGER_ERROR_MESSAGE_MAX 96
#define BOS_LEDGER_ERROR_PARTITION_MAX 16

static bos_ledger_state_t s_state = BOS_LEDGER_STATE_NONE;

/* RAM ledger store. An accepted push lands here first and becomes the
 * serving source immediately; the flash partition + NVS metadata are a
 * replaceable delivery cache written afterwards. When the cache write
 * fails the RAM copy keeps serving (degraded cache, lost on reboot).
 * The spinlock guards pointer/metadata swaps against the CoAP serving
 * task; copies inside the critical section are at most one 256-byte
 * chunk. */
static portMUX_TYPE s_ram_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_ram_body = NULL;
static bos_ledger_active_t s_ram_meta;

static bool s_last_error_present = false;
static esp_err_t s_last_error_code = ESP_OK;
static uint32_t s_last_error_size_bytes = 0;
static char s_last_error_phase[BOS_LEDGER_ERROR_PHASE_MAX] = "";
static char s_last_error_message[BOS_LEDGER_ERROR_MESSAGE_MAX] = "";
static char s_last_error_partition[BOS_LEDGER_ERROR_PARTITION_MAX] = "";

/* Internal state setter for the push handler (s_state stays file-local). */
void bos_ledger_ingress_set_state(bos_ledger_state_t state)
{
    s_state = state;
}

void clear_last_error(void)
{
    s_last_error_present = false;
    s_last_error_code = ESP_OK;
    s_last_error_size_bytes = 0;
    s_last_error_phase[0] = '\0';
    s_last_error_message[0] = '\0';
    s_last_error_partition[0] = '\0';
}

void bos_ledger_set_last_error(const char *phase,
                               esp_err_t code,
                               const char *message,
                               uint32_t size_bytes,
                               const char *partition)
{
    s_last_error_present = true;
    s_last_error_code = code;
    s_last_error_size_bytes = size_bytes;
    snprintf(s_last_error_phase, sizeof(s_last_error_phase), "%s", phase ? phase : "unknown");
    snprintf(s_last_error_message, sizeof(s_last_error_message), "%s", message ? message : "unknown");
    snprintf(s_last_error_partition, sizeof(s_last_error_partition), "%s", partition ? partition : "");
}

void bos_ledger_digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < BOS_LEDGER_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[BOS_LEDGER_DIGEST_LEN * 2] = '\0';
}

esp_err_t read_partition_idx(uint8_t *idx)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_LEDGER_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u8(h, BOS_LEDGER_KEY_ACTIVE_PART, idx);
    nvs_close(h);
    return err;
}

const esp_partition_t *partition_for_idx(uint8_t idx)
{
    const char *label = idx == 1 ? BOS_LEDGER_PARTITION_B : BOS_LEDGER_PARTITION_A;
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, BOS_LEDGER_PARTITION_SUBTYPE, label);
}

esp_err_t commit_active_metadata(uint8_t partition_idx,
                                 uint32_t version,
                                 const uint8_t digest[BOS_LEDGER_DIGEST_LEN],
                                 uint32_t size_bytes)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_LEDGER_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t chunks = (size_bytes + BOS_LEDGER_CHUNK_SIZE - 1U) / BOS_LEDGER_CHUNK_SIZE;
    if (chunks == 0) {
        chunks = 1;
    }

    err = nvs_set_u8(h, BOS_LEDGER_KEY_ACTIVE_PART, partition_idx);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, BOS_LEDGER_KEY_VERSION, version);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(h, BOS_LEDGER_KEY_DIGEST, digest, BOS_LEDGER_DIGEST_LEN);
    }
    if (err == ESP_OK) {
        err = nvs_set_u64(h, BOS_LEDGER_KEY_COMMITTED, (uint64_t)(esp_timer_get_time() / 1000ULL));
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, BOS_LEDGER_KEY_SIZE, size_bytes);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, BOS_LEDGER_KEY_CHUNK_SIZE, BOS_LEDGER_CHUNK_SIZE);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(h, BOS_LEDGER_KEY_CHUNKS, chunks);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* Installs an accepted ledger as the RAM serving source. Takes ownership of
 * body (heap allocation from the push handler). */
void ram_ledger_set(uint8_t *body, size_t len, const bos_ledger_body_view_t *view)
{
    bos_ledger_active_t meta = {0};
    meta.version = view->ledger_version;
    memcpy(meta.digest, view->manifest_digest, BOS_LEDGER_DIGEST_LEN);
    meta.committed_at = (uint64_t)(esp_timer_get_time() / 1000ULL);
    meta.size_bytes = (uint32_t)len;
    meta.chunk_size = BOS_LEDGER_CHUNK_SIZE;
    meta.chunk_count = (uint32_t)((len + BOS_LEDGER_CHUNK_SIZE - 1U) / BOS_LEDGER_CHUNK_SIZE);
    if (meta.chunk_count == 0) {
        meta.chunk_count = 1;
    }
    meta.active_partition = 0xFF; /* not partition-backed */
    meta.present = true;

    taskENTER_CRITICAL(&s_ram_mux);
    uint8_t *old = s_ram_body;
    s_ram_body = body;
    s_ram_meta = meta;
    taskEXIT_CRITICAL(&s_ram_mux);
    free(old);
}

/* Drops the RAM serving copy once the flash cache holds the same ledger. */
void ram_ledger_clear(void)
{
    taskENTER_CRITICAL(&s_ram_mux);
    uint8_t *old = s_ram_body;
    s_ram_body = NULL;
    taskEXIT_CRITICAL(&s_ram_mux);
    free(old);
}

esp_err_t bos_ledger_ingress_init(void)
{
    bos_ledger_active_t active;
    if (bos_ledger_ingress_get_active(&active) == ESP_OK && active.present) {
        char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
        bos_ledger_digest_hex(active.digest, digest);
        ESP_LOGI(TAG,
                 "active ledger persisted: version=%u bytes=%u chunks=%u digest=%s",
                 (unsigned)active.version,
                 (unsigned)active.size_bytes,
                 (unsigned)active.chunk_count,
                 digest);
        s_state = BOS_LEDGER_STATE_ACTIVE;
    } else {
        ESP_LOGI(TAG, "no active ledger persisted");
        s_state = BOS_LEDGER_STATE_NONE;
    }
    return ESP_OK;
}

bos_ledger_state_t bos_ledger_ingress_state(void)
{
    return s_state;
}

int bos_ledger_ingress_last_error_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }
    if (!s_last_error_present) {
        return snprintf(out, out_len, "{\"present\":false}");
    }
    return snprintf(out,
                    out_len,
                    "{\"present\":true,\"phase\":\"%s\",\"code\":%d,\"name\":\"%s\","
                    "\"message\":\"%s\",\"size_bytes\":%u,\"partition\":\"%s\"}",
                    s_last_error_phase,
                    (int)s_last_error_code,
                    esp_err_to_name(s_last_error_code),
                    s_last_error_message,
                    (unsigned)s_last_error_size_bytes,
                    s_last_error_partition);
}

static esp_err_t get_active_nvs(bos_ledger_active_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_LEDGER_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_u8(h, BOS_LEDGER_KEY_ACTIVE_PART, &out->active_partition);
    if (err != ESP_OK) {
        nvs_close(h);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    err = nvs_get_u32(h, BOS_LEDGER_KEY_VERSION, &out->version);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }

    size_t digest_len = BOS_LEDGER_DIGEST_LEN;
    err = nvs_get_blob(h, BOS_LEDGER_KEY_DIGEST, out->digest, &digest_len);
    if (err != ESP_OK || digest_len != BOS_LEDGER_DIGEST_LEN) {
        nvs_close(h);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    err = nvs_get_u64(h, BOS_LEDGER_KEY_COMMITTED, &out->committed_at);
    if (err != ESP_OK) {
        out->committed_at = 0;
    }
    err = nvs_get_u32(h, BOS_LEDGER_KEY_SIZE, &out->size_bytes);
    if (err != ESP_OK) {
        out->size_bytes = 0;
    }
    err = nvs_get_u32(h, BOS_LEDGER_KEY_CHUNK_SIZE, &out->chunk_size);
    if (err != ESP_OK || out->chunk_size == 0) {
        out->chunk_size = BOS_LEDGER_CHUNK_SIZE;
    }
    err = nvs_get_u32(h, BOS_LEDGER_KEY_CHUNKS, &out->chunk_count);
    if (err != ESP_OK && out->size_bytes > 0) {
        out->chunk_count = (out->size_bytes + out->chunk_size - 1U) / out->chunk_size;
    }

    out->present = true;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t bos_ledger_ingress_get_active(bos_ledger_active_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    /* RAM-first: a held RAM copy is the serving source (degraded cache). */
    taskENTER_CRITICAL(&s_ram_mux);
    if (s_ram_body) {
        *out = s_ram_meta;
        taskEXIT_CRITICAL(&s_ram_mux);
        return ESP_OK;
    }
    taskEXIT_CRITICAL(&s_ram_mux);

    return get_active_nvs(out);
}

bos_ledger_persist_t bos_ledger_ingress_persist_state(void)
{
    taskENTER_CRITICAL(&s_ram_mux);
    bool ram_held = s_ram_body != NULL;
    taskEXIT_CRITICAL(&s_ram_mux);
    if (ram_held) {
        return BOS_LEDGER_PERSIST_RAM_ONLY;
    }

    bos_ledger_active_t active;
    if (get_active_nvs(&active) == ESP_OK && active.present) {
        return BOS_LEDGER_PERSIST_PERSISTED;
    }
    return BOS_LEDGER_PERSIST_NONE;
}

const char *bos_ledger_ingress_persist_state_str(void)
{
    switch (bos_ledger_ingress_persist_state()) {
    case BOS_LEDGER_PERSIST_PERSISTED:
        return "persisted";
    case BOS_LEDGER_PERSIST_RAM_ONLY:
        return "ram_only";
    default:
        return "none";
    }
}

esp_err_t bos_ledger_ingress_read_active(size_t offset, uint8_t *out, size_t out_len, size_t *read_len)
{
    if (!out || !read_len) {
        return ESP_ERR_INVALID_ARG;
    }

    /* RAM-first: serve the accepted RAM copy when the cache is degraded. */
    taskENTER_CRITICAL(&s_ram_mux);
    if (s_ram_body) {
        if (offset >= s_ram_meta.size_bytes) {
            taskEXIT_CRITICAL(&s_ram_mux);
            return ESP_ERR_NOT_FOUND;
        }
        size_t ram_available = s_ram_meta.size_bytes - offset;
        size_t ram_to_read = ram_available < out_len ? ram_available : out_len;
        memcpy(out, s_ram_body + offset, ram_to_read);
        taskEXIT_CRITICAL(&s_ram_mux);
        *read_len = ram_to_read;
        return ESP_OK;
    }
    taskEXIT_CRITICAL(&s_ram_mux);

    bos_ledger_active_t active;
    esp_err_t err = get_active_nvs(&active);
    if (err != ESP_OK || !active.present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset >= active.size_bytes) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t available = active.size_bytes - offset;
    size_t to_read = available < out_len ? available : out_len;
    const esp_partition_t *partition = partition_for_idx(active.active_partition);
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }

    err = esp_partition_read(partition, offset, out, to_read);
    if (err == ESP_OK) {
        *read_len = to_read;
    }
    return err;
}
