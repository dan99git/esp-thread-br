/**
 * Building OS Border Router: app firmware OTA over the LAN-side BR HTTP server.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bos_br_ota_init(void);
esp_err_t bos_br_ota_confirm_running_app(void);

esp_err_t bos_br_ota_http_status(httpd_req_t *req);
esp_err_t bos_br_ota_http_upload(httpd_req_t *req);
esp_err_t bos_br_ota_http_fetch(httpd_req_t *req);
esp_err_t bos_br_ota_http_confirm(httpd_req_t *req);
esp_err_t bos_br_ota_http_rollback(httpd_req_t *req);

esp_err_t bos_br_ota_app_version(char *out, size_t out_len);
int bos_br_ota_status_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
