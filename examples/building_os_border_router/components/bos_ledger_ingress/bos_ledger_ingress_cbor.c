/**
 * Building OS Border Router: ledger ingress CBOR envelope parsing.
 *
 * Locates and digest-validates the ledger body inside the pushed CBOR
 * envelope. The push handler lives in bos_ledger_ingress_push.c.
 */

#include "bos_ledger_ingress.h"
#include "bos_ledger_ingress_internal.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

static const char *TAG = "bos_ingress";

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
    if (!read_cbor_arg(bytes, len, &pos, 0, &ledger_version) ||
        ledger_version == 0U || ledger_version > UINT32_MAX) {
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

esp_err_t validate_ledger_envelope(const uint8_t *bytes,
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
