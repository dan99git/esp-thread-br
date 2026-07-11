/**
 * Building OS Border Router: local diagnostics web surface.
 *
 * Route registration lives here; the handlers are split across
 * bos_diag_http_util.c, bos_diag_routes_core.c, bos_diag_routes_thread.c,
 * bos_diag_proxy.c, bos_diag_trigger.c, bos_diag_phonebook_parse.c, and
 * bos_diag_phonebook_store.c (shared declarations: bos_diag_internal.h).
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

static bool s_registered;

esp_err_t bos_diagnostics_server_start(void)
{
    if (s_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(bos_diag_phonebook_init(), TAG, "failed to init BR phonebook state");

    ESP_RETURN_ON_ERROR(bos_br_ota_init(), TAG, "failed to initialize BR OTA routes");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL),
                        TAG,
                        "failed to register IP event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL),
                        TAG,
                        "failed to register Ethernet event handler");

    httpd_uri_t status_uri = {
        .uri = "/bos/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };
    /* Unauthenticated read-only GET, same boundary as /bos/status. */
    httpd_uri_t thread_diag_uri = {
        .uri = "/bos/thread-diag",
        .method = HTTP_GET,
        .handler = thread_diag_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ledger_uri = {
        .uri = "/bos/ledger/active",
        .method = HTTP_GET,
        .handler = ledger_active_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ledger_push_uri = {
        .uri = "/bos/ledger/push",
        .method = HTTP_POST,
        .handler = ledger_push_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t commission_uri = {
        .uri = "/bos/commission",
        .method = HTTP_POST,
        .handler = bos_commissioning_http_post,
        .user_ctx = NULL,
    };
    /* The site server CommissioningService.imprint() hardcodes
     * POST /api/commission (host/runtime/server/src/commissioning.ts); keep
     * that path as an alias of the LAN-direct route so the existing imprint
     * sender reaches the BR unmodified. */
    httpd_uri_t commission_alias_uri = {
        .uri = "/api/commission",
        .method = HTTP_POST,
        .handler = bos_commissioning_http_post,
        .user_ctx = NULL,
    };
    /* Commissioning phase 2b joiner acceptance (docs/06.2 phase 2b); handlers
     * and auth in bos_commissioning/bos_joiner.c. */
    httpd_uri_t joiner_accept_uri = {
        .uri = "/bos/joiner/accept",
        .method = HTTP_POST,
        .handler = bos_joiner_accept_http_post,
        .user_ctx = NULL,
    };
    httpd_uri_t joiner_status_uri = {
        .uri = "/bos/joiner/status",
        .method = HTTP_GET,
        .handler = bos_joiner_status_http_get,
        .user_ctx = NULL,
    };
    httpd_uri_t ledger_torrent_uri = {
        .uri = "/bos/ledger/torrent",
        .method = HTTP_GET,
        .handler = ledger_torrent_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t convergence_uri = {
        .uri = "/bos/convergence",
        .method = HTTP_GET,
        .handler = convergence_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t peers_uri = {
        .uri = "/bos/peers",
        .method = HTTP_GET,
        .handler = peers_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_status_uri = {
        .uri = "/bos/ota/status",
        .method = HTTP_GET,
        .handler = bos_br_ota_http_status,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_upload_uri = {
        .uri = "/bos/ota/app",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_upload,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_fetch_uri = {
        .uri = "/bos/ota/app/fetch",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_fetch,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_confirm_uri = {
        .uri = "/bos/ota/confirm",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_confirm,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_rollback_uri = {
        .uri = "/bos/ota/rollback",
        .method = HTTP_POST,
        .handler = bos_br_ota_http_rollback,
        .user_ctx = NULL,
    };
    /* BR 802.15.4 radio window: open read, token-authed live TX-power set. */
    httpd_uri_t radio_get_uri = {
        .uri = "/bos/radio",
        .method = HTTP_GET,
        .handler = radio_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t radio_post_uri = {
        .uri = "/bos/radio",
        .method = HTTP_POST,
        .handler = radio_post_handler,
        .user_ctx = NULL,
    };
    /* Neighbor/link-quality table; shared bos_thread_diag renderer. */
    httpd_uri_t thread_neighbors_uri = {
        .uri = "/bos/thread-neighbors",
        .method = HTTP_GET,
        .handler = thread_neighbors_get_handler,
        .user_ctx = NULL,
    };
    /* Wedged-RCP recovery lever; token-authed. */
    httpd_uri_t thread_reset_uri = {
        .uri = "/bos/thread-reset",
        .method = HTTP_POST,
        .handler = thread_reset_post_handler,
        .user_ctx = NULL,
    };
    /* Confirm-gated identity reset: erase the NVS device_id override so boot
     * re-derives the EUI-64 id; token-authed, 428 plan without confirm. */
    httpd_uri_t device_id_reset_uri = {
        .uri = "/bos/device-id/reset",
        .method = HTTP_POST,
        .handler = device_id_reset_post_handler,
        .user_ctx = NULL,
    };
    /* Device-UI proxy. Wildcard URIs (the server runs httpd_uri_match_wildcard);
     * these register BEFORE the upstream catch-all slash-star GET handler
     * because external handlers are stored here at init and flushed to httpd
     * ahead of the default route when the server starts on the IP event. One
     * URI slot per method. */
    httpd_uri_t device_proxy_get_uri = {
        .uri = BOS_PROXY_URI_PREFIX "*",
        .method = HTTP_GET,
        .handler = device_proxy_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t device_proxy_put_uri = {
        .uri = BOS_PROXY_URI_PREFIX "*",
        .method = HTTP_PUT,
        .handler = device_proxy_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t device_proxy_post_uri = {
        .uri = BOS_PROXY_URI_PREFIX "*",
        .method = HTTP_POST,
        .handler = device_proxy_handler,
        .user_ctx = NULL,
    };
    /* Token-authed BR phonebook ingest: versioned v3 (18-col) CSV, header
     * eui64,device_id,device_class,make,model,model_id,sku,fw,rated_power_w,
     * rated_lumens,cct,discovered_at,eid,x,y,z,spatial_id,tags. */
    httpd_uri_t phonebook_post_uri = {
        .uri = "/bos/phonebook",
        .method = HTTP_POST,
        .handler = phonebook_post_handler,
        .user_ctx = NULL,
    };
    /* Read-only floating book for the gateway (BR-authored touch log). */
    httpd_uri_t phonebook_floating_get_uri = {
        .uri = "/bos/phonebook/floating",
        .method = HTTP_GET,
        .handler = phonebook_floating_get_handler,
        .user_ctx = NULL,
    };
    /* Conforming LAN->Thread external-trigger on-ramp (B1): token-authed,
     * emits a no-priority trigger envelope as a CoAP NON POST to the target
     * device's mesh/event. */
    httpd_uri_t trigger_post_uri = {
        .uri = "/bos/trigger",
        .method = HTTP_POST,
        .handler = trigger_post_handler,
        .user_ctx = NULL,
    };
    /* Position-keyed device proxy: GET /bos/<spatial_id>/<path...>. Wildcard
     * "/bos/" catch-all, registered LAST so every specific /bos route above
     * (status, ledger, device proxy, phonebook, trigger, ...) matches first;
     * only an unclaimed /bos/<segment>/ falls through here to the spatial
     * resolve. GET only. */
    httpd_uri_t spatial_proxy_get_uri = {
        .uri = "/bos/*",
        .method = HTTP_GET,
        .handler = spatial_proxy_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&status_uri), TAG, "failed to register /bos/status");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&thread_diag_uri), TAG, "failed to register /bos/thread-diag");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_uri), TAG, "failed to register /bos/ledger/active");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_push_uri), TAG, "failed to register /bos/ledger/push");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&commission_uri), TAG, "failed to register /bos/commission");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&commission_alias_uri), TAG, "failed to register /api/commission");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&joiner_accept_uri), TAG, "failed to register /bos/joiner/accept");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&joiner_status_uri), TAG, "failed to register /bos/joiner/status");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ledger_torrent_uri), TAG, "failed to register /bos/ledger/torrent");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&convergence_uri), TAG, "failed to register /bos/convergence");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&peers_uri), TAG, "failed to register /bos/peers");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_status_uri), TAG, "failed to register /bos/ota/status");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_upload_uri), TAG, "failed to register /bos/ota/app");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_fetch_uri), TAG, "failed to register /bos/ota/app/fetch");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_confirm_uri), TAG, "failed to register /bos/ota/confirm");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&ota_rollback_uri), TAG, "failed to register /bos/ota/rollback");
    /* Route-slot budget (esp_br_web.c BOS_EXTERNAL_HANDLER_MAX; route 17 past
     * the old cap of 16 broke the 0.1.4 OTA): this function now performs 28
     * registrations after adding GET /bos/<spatial_id>/ proxy. The cap is
     * raised to 28 in esp_br_web.c with zero spare external slots. */
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&radio_get_uri), TAG, "failed to register GET /bos/radio");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&radio_post_uri), TAG, "failed to register POST /bos/radio");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&thread_neighbors_uri), TAG, "failed to register /bos/thread-neighbors");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&thread_reset_uri), TAG, "failed to register /bos/thread-reset");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&device_id_reset_uri), TAG, "failed to register /bos/device-id/reset");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&device_proxy_get_uri), TAG, "failed to register GET /bos/device/*");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&device_proxy_put_uri), TAG, "failed to register PUT /bos/device/*");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&device_proxy_post_uri), TAG, "failed to register POST /bos/device/*");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&phonebook_post_uri), TAG, "failed to register POST /bos/phonebook");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&phonebook_floating_get_uri), TAG, "failed to register GET /bos/phonebook/floating");
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&trigger_post_uri), TAG, "failed to register POST /bos/trigger");
    /* MUST be the last /bos route registered (wildcard catch-all). */
    ESP_RETURN_ON_ERROR(esp_br_web_register_handler(&spatial_proxy_get_uri), TAG, "failed to register GET /bos/<spatial_id>/*");

    s_registered = true;
    esp_err_t confirm_err = bos_br_ota_confirm_running_app();
    if (confirm_err != ESP_OK) {
        ESP_LOGW(TAG,
                 "failed to confirm running OTA image after diagnostics route registration: %s",
                 esp_err_to_name(confirm_err));
    }
    ESP_LOGI(TAG, "Building OS diagnostics routes registered on BR port 80");
    return ESP_OK;
}
