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

esp_err_t bos_ledger_ingress_get_active(bos_ledger_active_t *out)
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

esp_err_t bos_ledger_ingress_read_active(size_t offset, uint8_t *out, size_t out_len, size_t *read_len)
{
    if (!out || !read_len) {
        return ESP_ERR_INVALID_ARG;
    }

    bos_ledger_active_t active;
    esp_err_t err = bos_ledger_ingress_get_active(&active);
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
    clear_last_error();

    if (req->content_len == 0 || req->content_len > BOS_LEDGER_PUSH_MAX_BYTES) {
        set_last_error("body_size", ESP_ERR_INVALID_SIZE, "ledger body must be 1..65536 bytes", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ledger body must be 1..65536 bytes");
        return ESP_OK;
    }

    uint8_t *body = (uint8_t *)malloc(req->content_len);
    if (!body) {
        set_last_error("receive", ESP_ERR_NO_MEM, "out of memory allocating ledger body", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
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
            /* Socket-level receive failure: connection is unusable, close it. */
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    s_state = BOS_LEDGER_STATE_VALIDATING;
    bos_ledger_body_view_t view = {0};
    esp_err_t err = validate_ledger_envelope(body, req->content_len, &view);
    if (err != ESP_OK) {
        free(body);
        s_state = prior_state;
        set_last_error("validate", err, "ledger envelope digest validation failed", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ledger envelope digest validation failed");
        return ESP_OK;
    }

    uint8_t active_idx = 0;
    if (read_partition_idx(&active_idx) != ESP_OK) {
        active_idx = 1;
    }
    uint8_t target_idx = active_idx == 0 ? 1 : 0;
    const esp_partition_t *target = partition_for_idx(target_idx);
    if (!target) {
        free(body);
        s_state = prior_state;
        set_last_error("partition_lookup", ESP_ERR_NOT_FOUND, "inactive ledger partition missing", (uint32_t)req->content_len, target_idx == 0 ? BOS_LEDGER_PARTITION_A : BOS_LEDGER_PARTITION_B);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "inactive ledger partition missing");
        return ESP_OK;
    }

    s_state = BOS_LEDGER_STATE_COMMITTING;
    err = write_staged_partition(target, body, req->content_len);
    if (err == ESP_OK) {
        err = commit_active_metadata(target_idx,
                                     view.ledger_version,
                                     view.manifest_digest,
                                     (uint32_t)req->content_len);
        if (err != ESP_OK) {
            set_last_error("metadata_commit", err, "failed to commit active ledger metadata", (uint32_t)req->content_len, target->label);
        }
    } else {
        set_last_error("partition_write", err, "failed to write staged ledger partition", (uint32_t)req->content_len, target->label);
    }

    if (err != ESP_OK) {
        free(body);
        s_state = prior_state;
        ESP_LOGW(TAG, "ledger push failed: %s", esp_err_to_name(err));
        char last_error_json[256];
        char error_json[384];
        int last_error_written = bos_ledger_ingress_last_error_json(last_error_json, sizeof(last_error_json));
        if (last_error_written < 0 || last_error_written >= (int)sizeof(last_error_json)) {
            snprintf(last_error_json, sizeof(last_error_json), "{\"present\":false}");
        }
        int written = snprintf(error_json,
                               sizeof(error_json),
                               "{\"ok\":false,\"error\":\"ledger_commit_failed\",\"last_error\":%s}",
                               last_error_json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        if (written < 0 || written >= (int)sizeof(error_json)) {
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"ledger_commit_failed\",\"last_error\":{\"present\":false}}");
        } else {
            httpd_resp_sendstr(req, error_json);
        }
        return ESP_OK;
    }

    char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
    digest_hex(view.manifest_digest, digest);
    uint32_t chunks = ((uint32_t)req->content_len + BOS_LEDGER_CHUNK_SIZE - 1U) / BOS_LEDGER_CHUNK_SIZE;
    char json[256];
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"ledger_version\":%u,\"manifest_digest\":\"%s\","
             "\"size_bytes\":%u,\"chunk_size\":%u,\"chunk_count\":%u,\"partition\":\"%s\"}",
             (unsigned)view.ledger_version,
             digest,
             (unsigned)req->content_len,
             (unsigned)BOS_LEDGER_CHUNK_SIZE,
             (unsigned)chunks,
             target_idx == 0 ? BOS_LEDGER_PARTITION_A : BOS_LEDGER_PARTITION_B);

    free(body);
    s_state = BOS_LEDGER_STATE_ACTIVE;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}
