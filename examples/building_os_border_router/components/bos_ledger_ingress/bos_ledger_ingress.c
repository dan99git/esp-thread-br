/**
 * Building OS Border Router: ledger ingress.
 */

#include "bos_ledger_ingress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_server_registration.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "bos_ingress";

#define BOS_LEDGER_PARTITION_SUBTYPE 0x99
#define BOS_LEDGER_PUSH_MAX_BYTES (64U * 1024U)
#define BOS_LEDGER_HEADER_TOKEN_MAX 128
#define BOS_LEDGER_ERROR_PHASE_MAX 32
#define BOS_LEDGER_ERROR_MESSAGE_MAX 96
#define BOS_LEDGER_ERROR_PARTITION_MAX 16

typedef struct {
    const uint8_t *body;
    size_t body_len;
    const uint8_t *manifest_digest;
    uint32_t ledger_version;
} bos_ledger_body_view_t;

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

/* In-flight push guard (docs/08.8-border-router.md s12): a push arriving
 * while a previous push is still being received/validated/persisted gets
 * 409 Conflict with the in-flight phase named in the body. esp_http_server
 * runs handlers on a single task today, so two push handlers cannot
 * interleave on the current httpd topology; the guard makes the documented
 * contract real independent of that topology and is the honest source of
 * the rejected push's "phase" field. */
static portMUX_TYPE s_push_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_push_in_flight = false;
static char s_push_phase[12] = "";

static bool s_last_error_present = false;
static esp_err_t s_last_error_code = ESP_OK;
static uint32_t s_last_error_size_bytes = 0;
static char s_last_error_phase[BOS_LEDGER_ERROR_PHASE_MAX] = "";
static char s_last_error_message[BOS_LEDGER_ERROR_MESSAGE_MAX] = "";
static char s_last_error_partition[BOS_LEDGER_ERROR_PARTITION_MAX] = "";

static void clear_last_error(void)
{
    s_last_error_present = false;
    s_last_error_code = ESP_OK;
    s_last_error_size_bytes = 0;
    s_last_error_phase[0] = '\0';
    s_last_error_message[0] = '\0';
    s_last_error_partition[0] = '\0';
}

static void set_last_error(const char *phase,
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

/* Tries to claim the single push slot. Returns false when another push is
 * already in flight. */
static bool push_guard_acquire(const char *initial_phase)
{
    bool acquired = false;
    taskENTER_CRITICAL(&s_push_mux);
    if (!s_push_in_flight) {
        s_push_in_flight = true;
        strlcpy(s_push_phase, initial_phase, sizeof(s_push_phase));
        acquired = true;
    }
    taskEXIT_CRITICAL(&s_push_mux);
    return acquired;
}

static void push_guard_set_phase(const char *phase)
{
    taskENTER_CRITICAL(&s_push_mux);
    strlcpy(s_push_phase, phase, sizeof(s_push_phase));
    taskEXIT_CRITICAL(&s_push_mux);
}

static void push_guard_release(void)
{
    taskENTER_CRITICAL(&s_push_mux);
    s_push_in_flight = false;
    s_push_phase[0] = '\0';
    taskEXIT_CRITICAL(&s_push_mux);
}

/* 409 Conflict for a concurrent push. Sends the response and returns ESP_OK
 * (socket-RST rule, see bos_ledger_ingress_http_push). */
static esp_err_t push_send_conflict(httpd_req_t *req)
{
    char phase[sizeof(s_push_phase)];
    taskENTER_CRITICAL(&s_push_mux);
    strlcpy(phase, s_push_phase, sizeof(phase));
    taskEXIT_CRITICAL(&s_push_mux);

    char json[96];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"ok\":false,\"error\":\"push_in_flight\",\"phase\":\"%s\"}",
                           phase[0] != '\0' ? phase : "unknown");
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"push_in_flight\",\"phase\":\"unknown\"}");
    } else {
        httpd_resp_sendstr(req, json);
    }
    return ESP_OK;
}

static void digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < BOS_LEDGER_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[BOS_LEDGER_DIGEST_LEN * 2] = '\0';
}

static bool read_cbor_arg(const uint8_t *bytes,
                          size_t len,
                          size_t *pos,
                          uint8_t expected_major,
                          uint64_t *value)
{
    if (*pos >= len) {
        return false;
    }

    uint8_t head = bytes[(*pos)++];
    uint8_t major = head >> 5;
    uint8_t ai = head & 0x1fU;
    if (major != expected_major) {
        return false;
    }

    if (ai < 24U) {
        *value = ai;
        return true;
    }

    uint8_t extra = 0;
    if (ai == 24U) {
        extra = 1;
    } else if (ai == 25U) {
        extra = 2;
    } else if (ai == 26U) {
        extra = 4;
    } else if (ai == 27U) {
        extra = 8;
    } else {
        return false;
    }

    if ((len - *pos) < extra) {
        return false;
    }

    uint64_t out = 0;
    for (uint8_t i = 0; i < extra; i++) {
        out = (out << 8) | bytes[(*pos)++];
    }
    *value = out;
    return true;
}

static bool cbor_expect_uint(const uint8_t *bytes, size_t len, size_t *pos, uint64_t expected)
{
    uint64_t value = 0;
    return read_cbor_arg(bytes, len, pos, 0, &value) && value == expected;
}

static bool cbor_read_bstr(const uint8_t *bytes,
                           size_t len,
                           size_t *pos,
                           const uint8_t **data,
                           size_t *data_len)
{
    uint64_t value_len = 0;
    if (!read_cbor_arg(bytes, len, pos, 2, &value_len)) {
        return false;
    }
    if (value_len > (uint64_t)(len - *pos)) {
        return false;
    }
    *data = &bytes[*pos];
    *data_len = (size_t)value_len;
    *pos += *data_len;
    return true;
}

static bool cbor_skip_value(const uint8_t *bytes, size_t len, size_t *pos, uint8_t depth)
{
    if (depth > 32U || *pos >= len) {
        return false;
    }

    uint8_t head = bytes[*pos];
    uint8_t major = head >> 5;
    uint8_t ai = head & 0x1fU;
    uint64_t value = 0;

    if (major <= 6U) {
        if (!read_cbor_arg(bytes, len, pos, major, &value)) {
            return false;
        }
    } else {
        (*pos)++;
    }

    switch (major) {
    case 0U:
    case 1U:
        return true;
    case 2U:
    case 3U:
        if (value > (uint64_t)(len - *pos)) {
            return false;
        }
        *pos += (size_t)value;
        return true;
    case 4U:
        for (uint64_t i = 0; i < value; i++) {
            if (!cbor_skip_value(bytes, len, pos, (uint8_t)(depth + 1U))) {
                return false;
            }
        }
        return true;
    case 5U:
        for (uint64_t i = 0; i < value; i++) {
            if (!cbor_skip_value(bytes, len, pos, (uint8_t)(depth + 1U)) ||
                !cbor_skip_value(bytes, len, pos, (uint8_t)(depth + 1U))) {
                return false;
            }
        }
        return true;
    case 6U:
        return cbor_skip_value(bytes, len, pos, (uint8_t)(depth + 1U));
    case 7U:
        if (ai < 24U) {
            return true;
        }
        if (ai == 24U) {
            value = 1;
        } else if (ai == 25U) {
            value = 2;
        } else if (ai == 26U) {
            value = 4;
        } else if (ai == 27U) {
            value = 8;
        } else {
            return false;
        }
        if (value > (uint64_t)(len - *pos)) {
            return false;
        }
        *pos += (size_t)value;
        return true;
    default:
        return false;
    }
}

static bool ledger_locate_body(const uint8_t *bytes, size_t len, bos_ledger_body_view_t *view)
{
    size_t pos = 0;
    uint64_t map_len = 0;
    if (!bytes || !view || !read_cbor_arg(bytes, len, &pos, 5, &map_len) || map_len != 4) {
        return false;
    }

    if (!cbor_expect_uint(bytes, len, &pos, 1) ||
        !cbor_expect_uint(bytes, len, &pos, 3) ||
        !cbor_expect_uint(bytes, len, &pos, 2)) {
        return false;
    }

    uint64_t ledger_version = 0;
    if (!read_cbor_arg(bytes, len, &pos, 0, &ledger_version) || ledger_version > UINT32_MAX) {
        return false;
    }

    const uint8_t *digest = NULL;
    size_t digest_len = 0;
    if (!cbor_expect_uint(bytes, len, &pos, 3) ||
        !cbor_read_bstr(bytes, len, &pos, &digest, &digest_len) ||
        digest_len != BOS_LEDGER_DIGEST_LEN ||
        !cbor_expect_uint(bytes, len, &pos, 4)) {
        return false;
    }

    size_t body_start = pos;
    if (!cbor_skip_value(bytes, len, &pos, 0) || pos != len) {
        return false;
    }

    view->body = &bytes[body_start];
    view->body_len = pos - body_start;
    view->manifest_digest = digest;
    view->ledger_version = (uint32_t)ledger_version;
    return true;
}

static esp_err_t validate_ledger_envelope(const uint8_t *bytes,
                                          size_t len,
                                          bos_ledger_body_view_t *view)
{
    if (!bytes || len == 0 || !view) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ledger_locate_body(bytes, len, view)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t actual_digest[BOS_LEDGER_DIGEST_LEN];
    int sha_ret = mbedtls_sha256(view->body, view->body_len, actual_digest, 0);
    if (sha_ret != 0) {
        ESP_LOGE(TAG, "ledger body SHA-256 failed: %d", sha_ret);
        return ESP_FAIL;
    }
    if (memcmp(actual_digest, view->manifest_digest, BOS_LEDGER_DIGEST_LEN) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t read_partition_idx(uint8_t *idx)
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

static const esp_partition_t *partition_for_idx(uint8_t idx)
{
    const char *label = idx == 1 ? BOS_LEDGER_PARTITION_B : BOS_LEDGER_PARTITION_A;
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, BOS_LEDGER_PARTITION_SUBTYPE, label);
}

static esp_err_t commit_active_metadata(uint8_t partition_idx,
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

static esp_err_t header_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return ESP_ERR_NOT_FOUND;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_len);
}

static bool push_authorized(httpd_req_t *req)
{
    char expected[BOS_LEDGER_HEADER_TOKEN_MAX];
    char provided[BOS_LEDGER_HEADER_TOKEN_MAX];

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

static esp_err_t write_staged_partition(const esp_partition_t *partition,
                                        const uint8_t *bytes,
                                        size_t len)
{
    if (!partition || !bytes || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        return err;
    }
    return esp_partition_write(partition, 0, bytes, len);
}

/* Installs an accepted ledger as the RAM serving source. Takes ownership of
 * body (heap allocation from the push handler). */
static void ram_ledger_set(uint8_t *body, size_t len, const bos_ledger_body_view_t *view)
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
static void ram_ledger_clear(void)
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
        digest_hex(active.digest, digest);
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

/* Error paths must return ESP_OK once a response has been sent. A non-ESP_OK
 * handler return makes esp_http_server close the socket immediately
 * (httpd_uri.c: "Handler returns error, this socket should be closed") and
 * skips the httpd_req_delete() purge of any unread POST body, so close() on
 * the socket with unreceived data emits a TCP RST that destroys the in-flight
 * response body (observed on hardware: 401 headers then connection reset).
 * Return ESP_FAIL only when the socket itself is unusable. */
esp_err_t bos_ledger_ingress_http_push(httpd_req_t *req)
{
    if (!push_authorized(req)) {
        set_last_error("auth", ESP_FAIL, "unauthorized ledger push", (uint32_t)req->content_len, "");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
        return ESP_OK;
    }

    /* Conflict guard (docs/08.8 s12): one push at a time. A second push
     * while this one is receiving/validating/persisting gets 409 with the
     * in-flight phase named; the in-flight push's last_error context is
     * left untouched. */
    if (!push_guard_acquire("receiving")) {
        return push_send_conflict(req);
    }
    clear_last_error();

    if (req->content_len == 0 || req->content_len > BOS_LEDGER_PUSH_MAX_BYTES) {
        set_last_error("body_size", ESP_ERR_INVALID_SIZE, "ledger body must be 1..65536 bytes", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ledger body must be 1..65536 bytes");
        push_guard_release();
        return ESP_OK;
    }

    uint8_t *body = (uint8_t *)malloc(req->content_len);
    if (!body) {
        set_last_error("receive", ESP_ERR_NO_MEM, "out of memory allocating ledger body", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        push_guard_release();
        return ESP_OK;
    }

    bos_ledger_state_t prior_state = s_state;
    s_state = BOS_LEDGER_STATE_RECEIVING;
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, (char *)body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            s_state = prior_state;
            set_last_error("receive", ESP_FAIL, "failed to receive ledger body", (uint32_t)req->content_len, "");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to receive ledger body");
            push_guard_release();
            /* Socket-level receive failure: connection is unusable, close it. */
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    s_state = BOS_LEDGER_STATE_VALIDATING;
    push_guard_set_phase("validating");
    bos_ledger_body_view_t view = {0};
    esp_err_t err = validate_ledger_envelope(body, req->content_len, &view);
    if (err != ESP_OK) {
        free(body);
        s_state = prior_state;
        set_last_error("validate", err, "ledger envelope digest validation failed", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ledger envelope digest validation failed");
        push_guard_release();
        return ESP_OK;
    }

    /* ACCEPT: the validated ledger becomes the serving source immediately.
     * Capture metadata before the RAM store takes ownership of body (view
     * pointers reference the body buffer and may dangle after the RAM copy
     * is cleared on persistence success). */
    uint32_t ledger_version = view.ledger_version;
    uint32_t size_bytes = (uint32_t)req->content_len;
    uint8_t digest_bytes[BOS_LEDGER_DIGEST_LEN];
    memcpy(digest_bytes, view.manifest_digest, BOS_LEDGER_DIGEST_LEN);
    uint32_t chunks = (size_bytes + BOS_LEDGER_CHUNK_SIZE - 1U) / BOS_LEDGER_CHUNK_SIZE;

    uint8_t *accepted = body;
    ram_ledger_set(accepted, req->content_len, &view);
    body = NULL; /* owned by the RAM ledger store now */
    s_state = BOS_LEDGER_STATE_ACTIVE;
    push_guard_set_phase("persisting");

    /* CACHE WRITE: flash partition + NVS metadata are a replaceable
     * delivery cache (server is the source of truth). Any failure here is
     * a logged warning and a degraded cache, never a push failure. Push
     * handling is serialized on the single httpd task, so the RAM copy
     * installed above cannot be swapped out under this write. */
    uint8_t active_idx = 0;
    if (read_partition_idx(&active_idx) != ESP_OK) {
        active_idx = 1;
    }
    uint8_t target_idx = active_idx == 0 ? 1 : 0;
    const char *target_label = target_idx == 0 ? BOS_LEDGER_PARTITION_A : BOS_LEDGER_PARTITION_B;
    const esp_partition_t *target = partition_for_idx(target_idx);

    if (!target) {
        err = ESP_ERR_NOT_FOUND;
        set_last_error("partition_lookup", err, "inactive ledger partition missing", size_bytes, target_label);
    } else {
        err = write_staged_partition(target, accepted, size_bytes);
        if (err == ESP_OK) {
            err = commit_active_metadata(target_idx, ledger_version, digest_bytes, size_bytes);
            if (err != ESP_OK) {
                set_last_error("metadata_commit", err, "failed to commit active ledger metadata", size_bytes, target->label);
            }
        } else {
            set_last_error("partition_write", err, "failed to write staged ledger partition", size_bytes, target->label);
        }
    }

    char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
    digest_hex(digest_bytes, digest);

    if (err != ESP_OK) {
        /* Degraded cache: the RAM copy keeps serving; the ledger is lost on
         * reboot (persist state then reads "none" until the server pushes
         * again -- a BR pull-from-server refetch path needs a server-side
         * artifact route that does not exist yet; recorded follow-up). */
        ESP_LOGW(TAG,
                 "ledger v%u accepted; cache persistence failed (%s); serving RAM copy, lost on reboot",
                 (unsigned)ledger_version,
                 esp_err_to_name(err));
        char persist_warning_json[256];
        int warning_written = bos_ledger_ingress_last_error_json(persist_warning_json, sizeof(persist_warning_json));
        if (warning_written < 0 || warning_written >= (int)sizeof(persist_warning_json)) {
            snprintf(persist_warning_json, sizeof(persist_warning_json), "{\"present\":false}");
        }
        char json[576];
        int written = snprintf(json,
                               sizeof(json),
                               "{\"ok\":true,\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                               "\"size_bytes\":%u,\"chunk_size\":%u,\"chunk_count\":%u,"
                               "\"persist\":\"failed\",\"serving\":\"ram\",\"persist_warning\":%s}",
                               (unsigned)ledger_version,
                               digest,
                               (unsigned)size_bytes,
                               (unsigned)BOS_LEDGER_CHUNK_SIZE,
                               (unsigned)chunks,
                               persist_warning_json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        if (written < 0 || written >= (int)sizeof(json)) {
            httpd_resp_sendstr(req, "{\"ok\":true,\"persist\":\"failed\",\"serving\":\"ram\",\"persist_warning\":{\"present\":false}}");
        } else {
            httpd_resp_sendstr(req, json);
        }
        push_guard_release();
        return ESP_OK;
    }

    /* Cache write succeeded: flash is the serving source again, drop RAM. */
    ram_ledger_clear();

    char json[320];
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"ledger_version\":%u,\"manifest_digest\":\"%s\","
             "\"size_bytes\":%u,\"chunk_size\":%u,\"chunk_count\":%u,"
             "\"partition\":\"%s\",\"persist\":\"persisted\"}",
             (unsigned)ledger_version,
             digest,
             (unsigned)size_bytes,
             (unsigned)BOS_LEDGER_CHUNK_SIZE,
             (unsigned)chunks,
             target_label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    push_guard_release();
    return ESP_OK;
}
