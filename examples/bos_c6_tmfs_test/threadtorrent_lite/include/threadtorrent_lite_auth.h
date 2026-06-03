#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t threadtorrent_lite_auth_sign(uint8_t *packet,
                                       size_t packet_len,
                                       const uint8_t *key,
                                       size_t key_len);

esp_err_t threadtorrent_lite_auth_verify(const uint8_t *packet,
                                         size_t packet_len,
                                         const uint8_t *key,
                                         size_t key_len);

#ifdef __cplusplus
}
#endif
