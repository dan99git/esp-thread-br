/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start border router web server, which provides REST APIs and GUI
 *
 * @param[in] base_path is the virtual file path of web server
 */
void esp_br_web_start(char *base_path);

/**
 * @brief Register an additional URI handler on the BR web server.
 *
 * The handler may be registered before the server starts. In that case it is
 * queued and installed when the web server starts after IP acquisition.
 *
 * @param[in] uri HTTP URI handler to register. The uri string and user_ctx
 *                must remain valid for the lifetime of the server.
 */
esp_err_t esp_br_web_register_handler(const httpd_uri_t *uri);

#ifdef __cplusplus
}
#endif
