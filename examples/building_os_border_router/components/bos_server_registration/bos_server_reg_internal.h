/**
 * Building OS Border Router: server registration internals shared between
 * bos_server_registration.c, bos_server_reg_transport.c and
 * bos_server_reg_heartbeat.c. Not part of the public component API.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_REG_RESPONSE_MAX 2048
#define BOS_REG_CONVERGENCE_JSON_MAX 12288
#define BOS_REG_URL_MAX 160
#define BOS_REG_REQUEST_URL_MAX 320
#define BOS_REG_DEVICE_ID_MAX 64
#define BOS_REG_TOKEN_MAX 96

/* Gateway discovery via mDNS browse of _bos-server._tcp (the gateway
 * self-advertises it: host/runtime/server/src/mdns-advertiser.ts). The
 * discovered URL is used for ALL server requests; the NVS/Kconfig
 * server_url is fallback only, when browse yields nothing (spec
 * docs/scratch/phonebook-v2-two-book-spec.md section 6; decision note
 * jobs/26-06-26/br-server-feed-ip-independence-2026-06-26.md). Repeated
 * send failures trigger a re-browse so a gateway IP change re-resolves.
 * Never written to NVS. */
#define BOS_REG_MDNS_SERVICE "_bos-server"
#define BOS_REG_MDNS_PROTO "_tcp"
#define BOS_REG_MDNS_BROWSE_TIMEOUT_MS 3000
#define BOS_REG_MDNS_BROWSE_MAX_RESULTS 8
#define BOS_REG_REBROWSE_FAILURE_THRESHOLD 3

typedef struct {
    char data[BOS_REG_RESPONSE_MAX];
    int len;
} bos_http_response_t;

/* BR device id used for site-server claim/status/convergence. Defined in
 * bos_server_registration.c (NVS override or derived EUI-64). */
extern char bos_reg_device_id[BOS_REG_DEVICE_ID_MAX];

/* Transport + gateway discovery (bos_server_reg_transport.c). */
esp_err_t nvs_get_string(const char *key, char *out, size_t out_len);
esp_err_t nvs_set_string(const char *key, const char *value);
esp_err_t load_server_url(void);
bool active_server_url(char *out, size_t out_len);
void refresh_gateway_discovery(void);
void note_send_result(esp_err_t err);
esp_err_t perform_json_request(const char *method,
                               const char *path,
                               const char *body,
                               bool include_device_token,
                               bos_http_response_t *response);

/* Heartbeat RAM ring (bos_server_reg_heartbeat.c). */
void bos_reg_u64_to_dec(uint64_t value, char out[21]);
void buffer_heartbeat(void);
esp_err_t flush_heartbeat_buffer(void);

#ifdef __cplusplus
}
#endif
