#include "threadtorrent_lite_auth.h"

#include <string.h>

#include "mbedtls/md.h"
#include "threadtorrent_lite_wire.h"

static const uint8_t ZERO_TAG[THREADTORRENT_LITE_TAG_LEN] = {0};

static esp_err_t compute_tag(const uint8_t *packet,
                             size_t packet_len,
                             const uint8_t *key,
                             size_t key_len,
                             uint8_t tag[THREADTORRENT_LITE_TAG_LEN])
{
    if (!packet || !key || !tag ||
        key_len == 0U ||
        packet_len < THREADTORRENT_LITE_HEADER_LEN ||
        packet_len > THREADTORRENT_LITE_MAX_PACKET_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) {
        return ESP_FAIL;
    }

    uint8_t full_tag[32] = {0};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int ret = mbedtls_md_setup(&ctx, md, 1);
    if (ret == 0) {
        ret = mbedtls_md_hmac_starts(&ctx, key, key_len);
    }
    if (ret == 0) {
        ret = mbedtls_md_hmac_update(&ctx, packet, THREADTORRENT_LITE_TAG_OFFSET);
    }
    if (ret == 0) {
        ret = mbedtls_md_hmac_update(&ctx, ZERO_TAG, sizeof(ZERO_TAG));
    }
    if (ret == 0 && packet_len > THREADTORRENT_LITE_HEADER_LEN) {
        ret = mbedtls_md_hmac_update(&ctx,
                                     packet + THREADTORRENT_LITE_HEADER_LEN,
                                     packet_len - THREADTORRENT_LITE_HEADER_LEN);
    }
    if (ret == 0) {
        ret = mbedtls_md_hmac_finish(&ctx, full_tag);
    }
    mbedtls_md_free(&ctx);

    if (ret != 0) {
        return ESP_FAIL;
    }
    memcpy(tag, full_tag, THREADTORRENT_LITE_TAG_LEN);
    return ESP_OK;
}

esp_err_t threadtorrent_lite_auth_sign(uint8_t *packet,
                                       size_t packet_len,
                                       const uint8_t *key,
                                       size_t key_len)
{
    uint8_t tag[THREADTORRENT_LITE_TAG_LEN] = {0};
    esp_err_t ret = compute_tag(packet, packet_len, key, key_len, tag);
    if (ret != ESP_OK) {
        return ret;
    }
    memcpy(packet + THREADTORRENT_LITE_TAG_OFFSET, tag, sizeof(tag));
    return ESP_OK;
}

esp_err_t threadtorrent_lite_auth_verify(const uint8_t *packet,
                                         size_t packet_len,
                                         const uint8_t *key,
                                         size_t key_len)
{
    uint8_t expected[THREADTORRENT_LITE_TAG_LEN] = {0};
    esp_err_t ret = compute_tag(packet, packet_len, key, key_len, expected);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t diff = 0;
    for (size_t i = 0; i < THREADTORRENT_LITE_TAG_LEN; ++i) {
        diff |= (uint8_t)(expected[i] ^ packet[THREADTORRENT_LITE_TAG_OFFSET + i]);
    }
    return diff == 0U ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
