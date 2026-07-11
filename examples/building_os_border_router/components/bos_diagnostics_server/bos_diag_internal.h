/**
 * Building OS Border Router diagnostics server: internal shared declarations.
 *
 * Private to the bos_diagnostics_server component. Shared types, defines, and
 * cross-file function declarations for the split source files:
 *   bos_diag_http_util.c, bos_diag_routes_core.c, bos_diag_routes_thread.c,
 *   bos_diag_proxy.c, bos_diag_trigger.c, bos_diag_phonebook_parse.c,
 *   bos_diag_phonebook_store.c, bos_diagnostics_server.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bos_ledger_ingress.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOS_PHONEBOOK_NVS_NAMESPACE "bos_phonebook"
#define BOS_PHONEBOOK_NVS_KEY "book"
#define BOS_PHONEBOOK_BODY_MAX 32768
#define BOS_PHONEBOOK_MAX_ROWS 64
#define BOS_PHONEBOOK_MESH_PATH "mesh/phonebook"
#define BOS_PHONEBOOK_DIGEST_HEX_LEN 64
#define BOS_PHONEBOOK_VERSION_MAX 24
#define BOS_PHONEBOOK_DIGEST_MAX 96
#define BOS_PHONEBOOK_GENERATED_AT_MAX 48
#define BOS_PHONEBOOK_DEVICE_ID_MAX 96
#define BOS_PHONEBOOK_SPATIAL_ID_MAX 128
#define BOS_PHONEBOOK_DEVICE_CLASS_MAX 48
#define BOS_PHONEBOOK_TAGS_MAX 160
#define BOS_PHONEBOOK_COORD_MAX 32
/* v3 operational book (phonebook v3, 17 columns, exact order). This is the
 * ONLY accepted format: a v2 (14-col) or v1 (9-col) document hard-rejects at
 * the magic line and again at the column-header cross-check - no silent
 * fallback. v3 rows carry make/model/model_id/photometry in the stored raw
 * document; only eui64/device_class/spatial_id/tags/x/y/z/eid are parsed into
 * the resolve + spatial-proxy table. The gateway still mints the version
 * (BR-owned version authority remains a follow-up). */
#define BOS_PHONEBOOK_COLUMNS_V3 \
    "eui64,device_id,device_class,make,model,model_id,sku,fw,rated_power_w,rated_lumens,cct,discovered_at,eid,x,y,z,spatial_id,tags"
#define BOS_PHONEBOOK_FIELDS_MAX 18

typedef struct {
    char eui64[17];
    char device_id[BOS_PHONEBOOK_DEVICE_ID_MAX];
    char spatial_id[BOS_PHONEBOOK_SPATIAL_ID_MAX];
    char device_class[BOS_PHONEBOOK_DEVICE_CLASS_MAX];
    char tags[BOS_PHONEBOOK_TAGS_MAX];
    char x[BOS_PHONEBOOK_COORD_MAX];
    char y[BOS_PHONEBOOK_COORD_MAX];
    char z[BOS_PHONEBOOK_COORD_MAX];
    char eid[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6Address address;
} bos_phonebook_row_t;

typedef struct {
    bool loaded;
    int columns_version; /* 3 = 17-col v3 (only accepted format) */
    char version[BOS_PHONEBOOK_VERSION_MAX];
    char digest[BOS_PHONEBOOK_DIGEST_MAX];
    char generated_at[BOS_PHONEBOOK_GENERATED_AT_MAX];
    size_t row_count;
    bos_phonebook_row_t rows[BOS_PHONEBOOK_MAX_ROWS];
} bos_phonebook_t;

typedef struct {
    uint16_t attempted;
    uint16_t sent;
    uint16_t failed;
    uint16_t bad_scope;
    char first_error[64];
} bos_phonebook_mesh_push_t;

#define BOS_PROXY_URI_PREFIX "/bos/device/"
#define BOS_PROXY_BODY_MAX 8192
#define BOS_PROXY_CHUNK_LEN 1024
#define BOS_PROXY_TIMEOUT_MS 8000

typedef struct {
    char content_type[96];
} proxy_response_meta_t;

typedef struct {
    otIp6Address address;
    uint16_t http_port;
} proxy_target_t;

/* Address-selection scope ranking for the SRP host walk. A device registers
 * MULTIPLE addresses with the SRP server (routable OMR/GUA, the stable
 * mesh-local ML-EID, the RLOC, and sometimes a link-local). The esp_http_client
 * forward must target the ROUTABLE (OMR) address; forwarding to a mesh-local,
 * RLOC, or link-local address is what caused the intermittent post-re-form 502
 * (the proxy used to take the first non-zero entry regardless of scope).
 *
 * The classification mirrors the C6's own /api/identity scope labels
 * (firmware web_server.c ip6_scope_label) but works on the bare otIp6Address
 * the SRP server hands back (no otNetifAddress mRloc/mMeshLocal flags here), so
 * it classifies by prefix bytes and the BR's mesh-local prefix instead. */
typedef enum {
    PROXY_SCOPE_UNUSABLE   = 0, /* unspecified / loopback / multicast */
    PROXY_SCOPE_LINK_LOCAL = 1, /* fe80::/10 */
    PROXY_SCOPE_RLOC       = 2, /* mesh-local prefix + RLOC IID (00:00:00ff:fe00:xxxx) */
    PROXY_SCOPE_MESH_LOCAL = 3, /* mesh-local prefix, ML-EID */
    PROXY_SCOPE_ROUTABLE   = 4, /* OMR/GUA: the address the forward must use */
} proxy_addr_scope_t;

/* bos_diag_http_util.c: send/JSON/body helpers shared by every route file. */
const char *ledger_state_str(bos_ledger_state_t state);
void digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1]);
void u64_to_dec(uint64_t value, char out[21]);
int json_string_or_null(char *out, size_t out_len, const char *value);
void send_json(httpd_req_t *req, const char *json);
esp_err_t send_generated_json(httpd_req_t *req,
                              size_t buffer_len,
                              int (*renderer)(char *out, size_t out_len),
                              const char *error_message);
esp_err_t send_status_json(httpd_req_t *req, const char *status_line, const char *error);
esp_err_t send_unauthorized(httpd_req_t *req);
esp_err_t send_cjson(httpd_req_t *req, cJSON *root);
int read_request_body(httpd_req_t *req, char *buf, size_t buf_len);
esp_err_t thread_diag_send_unavailable(httpd_req_t *req, const char *error);
const char *http_status_reason(int status);

/* bos_diag_routes_core.c: ledger/convergence/status routes + backbone events. */
esp_err_t ledger_active_get_handler(httpd_req_t *req);
esp_err_t ledger_push_handler(httpd_req_t *req);
esp_err_t ledger_torrent_get_handler(httpd_req_t *req);
esp_err_t convergence_get_handler(httpd_req_t *req);
esp_err_t peers_get_handler(httpd_req_t *req);
esp_err_t status_get_handler(httpd_req_t *req);
void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

/* bos_diag_routes_thread.c: Thread diagnostics/radio/reset routes. */
esp_err_t thread_diag_get_handler(httpd_req_t *req);
esp_err_t radio_get_handler(httpd_req_t *req);
esp_err_t radio_post_handler(httpd_req_t *req);
esp_err_t thread_neighbors_get_handler(httpd_req_t *req);
esp_err_t thread_reset_post_handler(httpd_req_t *req);
esp_err_t device_id_reset_post_handler(httpd_req_t *req);

/* bos_diag_proxy.c: EUI-64 normalisation, address classification, SRP resolve,
 * device/spatial HTTP proxy routes. */
bool proxy_normalise_eui64(const char *raw, size_t raw_len, char out[17]);
proxy_addr_scope_t proxy_classify_address(const otIp6Address *addr, const otMeshLocalPrefix *mlp);
bool trigger_resolve_eui64(otInstance *instance,
                           const char *eui64,
                           proxy_target_t *out,
                           proxy_addr_scope_t *scope_out);
esp_err_t device_proxy_handler(httpd_req_t *req);
esp_err_t spatial_proxy_handler(httpd_req_t *req);

/* bos_diag_trigger.c: conforming LAN->Thread external-trigger on-ramp. */
esp_err_t trigger_post_handler(httpd_req_t *req);

/* bos_diag_phonebook_parse.c: v3 CSV parse + push-header validation. */
esp_err_t phonebook_parse_csv(const char *raw,
                              bos_phonebook_t *out,
                              char computed_digest[BOS_PHONEBOOK_DIGEST_HEX_LEN + 1],
                              char *error,
                              size_t error_len);
esp_err_t phonebook_validate_push_headers(httpd_req_t *req,
                                          const bos_phonebook_t *book,
                                          const char *computed_digest,
                                          char *error,
                                          size_t error_len);

/* bos_diag_phonebook_store.c: phonebook state (NVS + RAM), routes, resolvers. */
esp_err_t bos_diag_phonebook_init(void);
esp_err_t phonebook_floating_get_handler(httpd_req_t *req);
esp_err_t phonebook_post_handler(httpd_req_t *req);
bool phonebook_resolve_eui64(otInstance *instance,
                             const char *eui64,
                             proxy_target_t *out,
                             bool *matched_out,
                             proxy_addr_scope_t *scope_out);
bool phonebook_resolve_spatial_to_eui64(const char *spatial_id, char out_eui64[17]);

#ifdef __cplusplus
}
#endif
