/**
 * Building OS Border Router: local diagnostics web surface.
 * Split part: EUI-64 normalisation, address-scope classification, SRP
 * resolution, and the device/spatial HTTP proxy routes
 * (see bos_diag_internal.h).
 */

#include "bos_diagnostics_server.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bos_commissioning.h"
#include "bos_convergence_aggregator.h"
#include "bos_floating_book.h"
#include "bos_joiner.h"
#include "bos_thread_dataset_anchor.h"
#include "bos_thread_diag.h"
#include "bos_br_ota.h"
#include "bos_ledger_ingress.h"
#include "bos_ledger_mesh_serve.h"
#include "bos_server_registration.h"
#include "cJSON.h"
#include "esp_br_web.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_rcp_update.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "openthread/coap.h"
#include "openthread/dns.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/message.h"
#include "openthread/platform/radio.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

#include "bos_diag_internal.h"

static const char *TAG = "bos_diag";

/* ------------------------------------------------------------------------
 * /bos/device/<eui64>/<path...>: the BR device-UI proxy (control-plane
 * slice 0, docs/scratch/control-plane doc; THE route the over-Thread goal
 * needs). The BR is the only node on both the LAN and the Thread mesh, so it
 * bridges a LAN client to a mesh device's port-80 surface. Keyed by the
 * durable EUI-64: the device's mesh IPv6 is ephemeral and resolved BR-side
 * from its SRP registration (otSrpServerGetNextHost host walk -> TXT
 * hw/hid/eui64 entry match -> host address; same table and TXT contract as
 * bos_convergence_aggregator). Forwarded with esp_http_client preserving
 * method, body, and Content-Type; the device response is streamed back
 * chunk-by-chunk with its status and Content-Type.
 * Auth: GET is open-on-LAN (same model as /bos/status); PUT/POST reach the
 * device CONTROL surface and require the device token / api key
 * (bos_commissioning_request_authorized). Consumer contract:
 * br-device-proxy.mjs + skill building-os:br-device-proxy.
 * ------------------------------------------------------------------------ */

static esp_err_t proxy_http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data &&
        evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Content-Type") == 0) {
        proxy_response_meta_t *meta = (proxy_response_meta_t *)evt->user_data;
        snprintf(meta->content_type, sizeof(meta->content_type), "%s", evt->header_value);
    }
    return ESP_OK;
}

/* Accepts the full 16-hex EUI-64 (br-device-proxy.mjs sends this) and the
 * 12-hex EUI-48 short form (the fffe-elided form used in bench notes, e.g.
 * 9888e07f83b8 for 9888e0fffe7f83b8); the short form expands by re-inserting
 * fffe. Output is the lowercase 16-hex canonical form. */
bool proxy_normalise_eui64(const char *raw, size_t raw_len, char out[17])
{
    char lowered[17];
    if (raw_len != 16 && raw_len != 12) {
        return false;
    }
    for (size_t i = 0; i < raw_len; i++) {
        if (!isxdigit((unsigned char)raw[i])) {
            return false;
        }
        lowered[i] = (char)tolower((unsigned char)raw[i]);
    }
    lowered[raw_len] = '\0';
    if (raw_len == 16) {
        memcpy(out, lowered, 17);
    } else {
        memcpy(out, lowered, 6);
        memcpy(out + 6, "fffe", 4);
        memcpy(out + 10, lowered + 6, 6);
        out[16] = '\0';
    }
    return true;
}

/* True when addr's network prefix (top 64 bits) equals the BR's mesh-local
 * prefix. otMeshLocalPrefix is the 8-byte /64 mesh-local prefix. */
static bool proxy_addr_in_mesh_local(const otIp6Address *addr, const otMeshLocalPrefix *mlp)
{
    return mlp != NULL && memcmp(addr->mFields.m8, mlp->m8, OT_MESH_LOCAL_PREFIX_SIZE) == 0;
}

/* The Thread RLOC IID is fixed: 0000:00ff:fe00:RLOC16 (bytes 8..15). */
static bool proxy_addr_is_rloc_iid(const otIp6Address *addr)
{
    const uint8_t *iid = &addr->mFields.m8[8];
    return iid[0] == 0x00U && iid[1] == 0x00U && iid[2] == 0x00U && iid[3] == 0xffU &&
           iid[4] == 0xfeU && iid[5] == 0x00U;
}

proxy_addr_scope_t proxy_classify_address(const otIp6Address *addr,
                                          const otMeshLocalPrefix *mlp)
{
    static const uint8_t zero[16] = {0};
    if (memcmp(addr->mFields.m8, zero, sizeof(zero)) == 0) {
        return PROXY_SCOPE_UNUSABLE; /* unspecified */
    }
    if (addr->mFields.m8[0] == 0xffU) {
        return PROXY_SCOPE_UNUSABLE; /* multicast (must never be an SRP host addr) */
    }
    /* fe80::/10 link-local: first byte 0xfe, top two bits of second byte = 10. */
    if (addr->mFields.m8[0] == 0xfeU && (addr->mFields.m8[1] & 0xc0U) == 0x80U) {
        return PROXY_SCOPE_LINK_LOCAL;
    }
    if (proxy_addr_in_mesh_local(addr, mlp)) {
        return proxy_addr_is_rloc_iid(addr) ? PROXY_SCOPE_RLOC : PROXY_SCOPE_MESH_LOCAL;
    }
    /* Anything else with a global ULA/GUA prefix (the OMR address). */
    return PROXY_SCOPE_ROUTABLE;
}

static int proxy_http_scope_rank(proxy_addr_scope_t scope)
{
    return (int)scope;
}

static int trigger_coap_scope_rank(proxy_addr_scope_t scope)
{
    switch (scope) {
    case PROXY_SCOPE_MESH_LOCAL:
        return 4;
    default:
        return 0;
    }
}

/* SRP host walk under the held OT lock. The _mesh._udp TXT contract
 * publishes the EUI-64 under hw (aliases hid/eui64/hardware_id; see
 * bos_convergence_aggregator parse_txt_into_peer) and the device web port
 * under http (default 80). The caller supplies the address-scope ranking:
 * HTTP proxy prefers routable OMR; CoAP trigger prefers mesh-local Thread
 * addresses. */
static bool resolve_eui64_srp(otInstance *instance,
                              const char *eui64,
                              proxy_target_t *out,
                              proxy_addr_scope_t *scope_out,
                              int (*scope_rank)(proxy_addr_scope_t scope))
{
    if (otSrpServerGetState(instance) != OT_SRP_SERVER_STATE_RUNNING) {
        return false;
    }

    const otMeshLocalPrefix *mlp = otThreadGetMeshLocalPrefix(instance);

    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL) {
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }

        bool matched = false;
        uint16_t http_port = 80;
        const otSrpServerService *service = NULL;
        while ((service = otSrpServerHostGetNextService(host, service)) != NULL) {
            if (otSrpServerServiceIsDeleted(service)) {
                continue;
            }
            uint16_t txt_len = 0;
            const uint8_t *txt = otSrpServerServiceGetTxtData(service, &txt_len);
            if (!txt || txt_len == 0U) {
                continue;
            }
            otDnsTxtEntryIterator iterator;
            otDnsInitTxtEntryIterator(&iterator, txt, txt_len);
            otDnsTxtEntry entry;
            while (otDnsGetNextTxtEntry(&iterator, &entry) == OT_ERROR_NONE) {
                if (!entry.mKey || !entry.mValue || entry.mValueLength == 0U) {
                    continue;
                }
                char value[40];
                size_t copy_len = entry.mValueLength < sizeof(value) - 1 ? entry.mValueLength : sizeof(value) - 1;
                memcpy(value, entry.mValue, copy_len);
                value[copy_len] = '\0';
                if (strcmp(entry.mKey, "hw") == 0 ||
                    strcmp(entry.mKey, "hid") == 0 ||
                    strcmp(entry.mKey, "eui64") == 0 ||
                    strcmp(entry.mKey, "hardware_id") == 0) {
                    if (strcasecmp(value, eui64) == 0) {
                        matched = true;
                    }
                } else if (strcmp(entry.mKey, "http") == 0) {
                    unsigned long port = strtoul(value, NULL, 10);
                    if (port > 0UL && port <= 65535UL) {
                        http_port = (uint16_t)port;
                    }
                }
            }
        }

        if (!matched) {
            continue;
        }

        /* Pick the best-scoped registered address rather than the first
         * non-zero one. A device registers several addresses; HTTP and CoAP
         * need different scopes, so ranking is supplied by the caller. */
        uint8_t address_count = 0;
        const otIp6Address *addresses = otSrpServerHostGetAddresses(host, &address_count);
        const otIp6Address *best = NULL;
        proxy_addr_scope_t best_scope = PROXY_SCOPE_UNUSABLE;
        int best_rank = 0;
        for (uint8_t i = 0; addresses && i < address_count; i++) {
            proxy_addr_scope_t scope = proxy_classify_address(&addresses[i], mlp);
            int rank = scope_rank(scope);
            if (rank > best_rank) {
                best_rank = rank;
                best_scope = scope;
                best = &addresses[i];
            }
        }
        if (best != NULL && best_rank > 0) {
            out->address = *best;
            out->http_port = http_port;
            if (scope_out) {
                *scope_out = best_scope;
            }
            return true;
        }
    }
    return false;
}

static bool proxy_resolve_eui64(otInstance *instance, const char *eui64, proxy_target_t *out)
{
    proxy_addr_scope_t scope = PROXY_SCOPE_UNUSABLE;
    bool resolved = resolve_eui64_srp(instance, eui64, out, &scope, proxy_http_scope_rank);
    if (resolved && scope != PROXY_SCOPE_ROUTABLE) {
        /* Reachable-but-suboptimal: no routable OMR address was registered
         * (e.g. mid-reform before the C6 re-registers). Forward anyway on the
         * best available, but log it so a persistent non-routable selection is
         * visible at the bench. */
        ESP_LOGW(TAG,
                 "proxy resolve %s: no routable address registered, "
                 "forwarding on scope=%d", eui64, (int)scope);
    }
    return resolved;
}

bool trigger_resolve_eui64(otInstance *instance,
                           const char *eui64,
                           proxy_target_t *out,
                           proxy_addr_scope_t *scope_out)
{
    proxy_addr_scope_t scope = PROXY_SCOPE_UNUSABLE;
    bool resolved = resolve_eui64_srp(instance, eui64, out, &scope, trigger_coap_scope_rank);
    if (scope_out) {
        *scope_out = scope;
    }
    return resolved;
}

/* Shared mesh-forward machinery (defined below): resolves eui64 -> routable
 * mesh address via the BR's own SRP table, forwards over the mesh with
 * esp_http_client, streams the device response back. Used by BOTH the eui64
 * proxy (/bos/device/<eui64>/) and the spatial-id proxy (/bos/<spatial_id>/). */
static esp_err_t proxy_forward_to_device(httpd_req_t *req, const char *eui64, const char *device_path);

esp_err_t device_proxy_handler(httpd_req_t *req)
{
    /* GET is open-on-LAN; PUT/POST reach the device control surface and are
     * privileged (same trust anchor as /bos/commission). */
    if (req->method != HTTP_GET && !bos_commissioning_request_authorized(req)) {
        return send_unauthorized(req);
    }

    /* Parse /bos/device/<eui64>/<path...> out of the request URI (query
     * string included in req->uri and forwarded as-is). */
    const size_t prefix_len = strlen(BOS_PROXY_URI_PREFIX);
    if (strncmp(req->uri, BOS_PROXY_URI_PREFIX, prefix_len) != 0) {
        return send_status_json(req, "400 Bad Request", "malformed proxy uri");
    }
    const char *eui_start = req->uri + prefix_len;
    const char *eui_end = strchr(eui_start, '/');
    size_t eui_len = eui_end ? (size_t)(eui_end - eui_start) : strlen(eui_start);
    char eui64[17];
    if (!proxy_normalise_eui64(eui_start, eui_len, eui64)) {
        return send_status_json(req, "400 Bad Request", "eui64 must be 16 (or fffe-elided 12) hex chars");
    }
    const char *device_path = (eui_end && eui_end[0] != '\0') ? eui_end : "/";

    return proxy_forward_to_device(req, eui64, device_path);
}

static esp_err_t proxy_forward_to_device(httpd_req_t *req, const char *eui64, const char *device_path)
{
    /* Request body (PUT/POST), preserved verbatim. */
    char *body = NULL;
    int body_len = 0;
    if (req->method != HTTP_GET && req->content_len > 0) {
        if (req->content_len > BOS_PROXY_BODY_MAX) {
            return send_status_json(req, "413 Payload Too Large", "proxy body over 8192 bytes");
        }
        body = (char *)malloc(req->content_len + 1);
        if (!body) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            return ESP_OK;
        }
        body_len = read_request_body(req, body, req->content_len + 1);
        if (body_len < 0) {
            free(body);
            return send_status_json(req, "400 Bad Request", "failed to read request body");
        }
    }

    /* Resolve the EUI-64 to a mesh address via the SRP server table. */
    proxy_target_t target = {.http_port = 80};
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        free(body);
        return thread_diag_send_unavailable(req, "openthread lock timeout");
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        free(body);
        return thread_diag_send_unavailable(req, "openthread not running");
    }
    bool resolved = proxy_resolve_eui64(instance, eui64, &target);
    esp_openthread_lock_release();
    if (!resolved) {
        free(body);
        return send_status_json(req, "404 Not Found", "device not found in SRP registrations");
    }

    char address_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&target.address, address_str, sizeof(address_str));

    char url[256];
    int url_len = snprintf(url, sizeof(url), "http://[%s]:%u%s",
                           address_str, (unsigned)target.http_port, device_path);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        free(body);
        return send_status_json(req, "400 Bad Request", "proxied url too long");
    }

    /* Forward over the mesh. Manual open/fetch_headers mode so the response
     * streams back without buffering the whole body. */
    proxy_response_meta_t meta = {.content_type = ""};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = BOS_PROXY_TIMEOUT_MS,
        .event_handler = proxy_http_event_cb,
        .user_data = &meta,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        return send_status_json(req, "502 Bad Gateway", "proxy client init failed");
    }
    esp_http_client_set_method(client,
                               (req->method == HTTP_PUT)  ? HTTP_METHOD_PUT
                               : (req->method == HTTP_POST) ? HTTP_METHOD_POST
                                                            : HTTP_METHOD_GET);
    if (body_len > 0) {
        char content_type[96];
        if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
            snprintf(content_type, sizeof(content_type), "application/json");
        }
        esp_http_client_set_header(client, "Content-Type", content_type);
    }

    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(body);
        ESP_LOGW(TAG, "proxy open to %s failed: %s", url, esp_err_to_name(err));
        return send_status_json(req, "502 Bad Gateway", "device unreachable over mesh");
    }
    if (body_len > 0 && esp_http_client_write(client, body, body_len) != body_len) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(body);
        return send_status_json(req, "502 Bad Gateway", "failed to forward request body");
    }
    free(body);
    body = NULL;

    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return send_status_json(req, "502 Bad Gateway", "no response from device");
    }

    int status = esp_http_client_get_status_code(client);
    char status_line[48];
    snprintf(status_line, sizeof(status_line), "%d %s", status, http_status_reason(status));
    httpd_resp_set_status(req, status_line);
    httpd_resp_set_type(req, meta.content_type[0] != '\0' ? meta.content_type : "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char chunk[BOS_PROXY_CHUNK_LEN];
    for (;;) {
        int read_len = esp_http_client_read(client, chunk, sizeof(chunk));
        if (read_len < 0) {
            /* Mid-stream failure: abort the chunked transfer honestly rather
             * than presenting a truncated body as complete. */
            httpd_resp_send_chunk(req, NULL, 0);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_OK;
        }
        if (read_len == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, read_len) != ESP_OK) {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

/* ------------------------------------------------------------------------
 * GET /bos/<spatial_id>/<path...>: the position-keyed sibling of the eui64
 * device proxy. Resolves spatial_id -> eui64 from the BR's OWN operational
 * phonebook (gateway-independent at request time), then reuses the exact same
 * mesh-forward machinery as /bos/device/<eui64>/. PAIRED devices only: a
 * spatial_id exists only on an operational row (published by the gateway on
 * pairing). Unknown spatial_id -> 404; floating rows have no spatial_id so
 * they are never reachable here. The operational book must be CURRENT for this
 * to resolve - the gateway's author-and-publish-on-pairing step is the
 * load-bearing prerequisite.
 * ------------------------------------------------------------------------ */

esp_err_t spatial_proxy_handler(httpd_req_t *req)
{
    /* Registered as a GET-only catch-all under /bos/ (after all specific /bos
     * routes), so it only ever sees GET here: open-on-LAN, same as the eui64
     * proxy GET. */
    const char *prefix = "/bos/";
    const size_t prefix_len = strlen(prefix);
    if (strncmp(req->uri, prefix, prefix_len) != 0) {
        return send_status_json(req, "400 Bad Request", "malformed spatial proxy uri");
    }
    const char *sid_start = req->uri + prefix_len;
    const char *sid_end = strchr(sid_start, '/');
    size_t sid_len = sid_end ? (size_t)(sid_end - sid_start) : strlen(sid_start);
    if (sid_len == 0U || sid_len >= BOS_PHONEBOOK_SPATIAL_ID_MAX) {
        return send_status_json(req, "404 Not Found", "unknown spatial_id");
    }
    char spatial_id[BOS_PHONEBOOK_SPATIAL_ID_MAX];
    memcpy(spatial_id, sid_start, sid_len);
    spatial_id[sid_len] = '\0';

    char eui64[17];
    if (!phonebook_resolve_spatial_to_eui64(spatial_id, eui64)) {
        return send_status_json(req, "404 Not Found", "spatial_id not in operational phonebook");
    }

    const char *device_path = (sid_end && sid_end[0] != '\0') ? sid_end : "/";
    ESP_LOGI(TAG, "spatial proxy: spatial_id=%s -> eui64=%s path=%s", spatial_id, eui64, device_path);
    return proxy_forward_to_device(req, eui64, device_path);
}
