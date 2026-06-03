/*
 * Compatibility surface for esp-thread-br v1.3 when the local ESP-IDF
 * OpenThread headers do not provide esp_openthread_start().
 */

#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_openthread_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_netif_config_t               netif_config;
    esp_openthread_platform_config_t platform_config;
} esp_openthread_config_t;

esp_err_t esp_openthread_start(const esp_openthread_config_t *config);
void ot_console_start(void);

#ifdef __cplusplus
}
#endif
