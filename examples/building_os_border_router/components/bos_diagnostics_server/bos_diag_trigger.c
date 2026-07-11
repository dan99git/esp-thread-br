/**
 * Building OS Border Router: local diagnostics web surface.
 * Split part: POST /bos/trigger, the conforming LAN->Thread external-trigger
 * on-ramp (see bos_diag_internal.h).
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
 * POST /bos/trigger: the CONFORMING LAN->Thread external-trigger on-ramp
 * (platform work item B1). This is the conforming REPLACEMENT for the
 * non-conforming POST /bos/control that was stripped (it carried a
 * sender-asserted priority and shipped an arbitrary immediate effect onto
 * mesh/event - a policy bypass).
 *
 * The body is a CONFORMING external trigger envelope: source_id, the target
 * selectors, action / event_key / control_id, and parameters only. It carries
 * NO priority and NO inline command/effect. The receiving device handles it as
 * a hook/control event and firmware arbitrates priority by source/type - never
 * by a priority from the wire.
 *
 * Transport: a CoAP NON-confirmable POST to coap://[<device ML-EID>]/mesh/event,
 * Content-Format JSON, mirroring the device's own emitter
 * (firmware/.../mesh_coap.c mesh_coap_emit_control_event, the shape that
 * parse_event_envelope() reads) EXACTLY: NON+POST, AppendUriPathOptions
 * "mesh/event", AppendContentFormatOption JSON, SetPayloadMarker,
 * otMessageAppend, otCoapSendRequest(...,NULL,NULL) under the held OT lock.
 * otCoapStart is already called in bos_ledger_mesh_serve_init; this reuses
 * that CoAP context.
 *
 * UNICAST, not multicast: the target EUI-64 is resolved by the same SRP host
 * walk as the HTTP proxy, but with trigger-specific address ranking that
 * prefers the device's mesh-local EID/RLOC instead of the OMR address needed
 * by esp_http_client. A target that cannot be resolved to a Thread-routable
 * unicast address is a clear 4xx; the BR does NOT fall back to multicast.
 * spatial_id->EUI-64 resolution is a site-server responsibility (group
 * selectors resolved before Thread fan-out, docs/09.2 "Runtime Control Over
 * Thread"); the device-facing BR call is unicast-by-address resolved from the
 * durable EUI-64.
 *
 * Auth: token-gated, same trust anchor as the other /bos write routes
 * (bos_commissioning_request_authorized).
 * ------------------------------------------------------------------------ */
#define BOS_TRIGGER_BODY_MAX 1024
#define BOS_TRIGGER_PATH_EVENT "mesh/event"
#define BOS_TRIGGER_SOURCE_ID_MAX 96

/* Build the conforming mesh/event envelope from the validated request fields.
 * Mirrors the device emitter field set (mesh_event_emit.c): source_id
 * (required), action / event_key / control_id (optional), a target object with
 * any of eui64 / spatial_id / tag / device_class / selector_ref, and the
 * parameters object echoed verbatim. NO priority, NO command. Returns a
 * serialized string the caller must cJSON_free, or NULL on allocation
 * failure. */
static char *trigger_build_envelope(const cJSON *req_root,
                                    const char *source_id,
                                    cJSON **echo_out)
{
    cJSON *env = cJSON_CreateObject();
    if (!env) {
        return NULL;
    }

    /* source_id is required and always present. */
    if (!cJSON_AddStringToObject(env, "source_id", source_id)) {
        cJSON_Delete(env);
        return NULL;
    }

    /* Optional top-level string fields, copied only when present and string. */
    static const char *const opt_keys[] = {"action", "event_key", "control_id"};
    for (size_t i = 0; i < sizeof(opt_keys) / sizeof(opt_keys[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(req_root, opt_keys[i]);
        if (cJSON_IsString(v) && v->valuestring && v->valuestring[0] != '\0') {
            if (!cJSON_AddStringToObject(env, opt_keys[i], v->valuestring)) {
                cJSON_Delete(env);
                return NULL;
            }
        }
    }

    /* target object: copy any present selector. The device reads these via
     * parse_event_envelope (eui64 / spatial_id / tag / device_class /
     * selector_ref) and may reject an envelope that does not apply to it. */
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(req_root, "target");
    if (cJSON_IsObject(target)) {
        cJSON *t_out = cJSON_CreateObject();
        if (!t_out) {
            cJSON_Delete(env);
            return NULL;
        }
        static const char *const sel_keys[] = {
            "eui64", "spatial_id", "tag", "device_class", "selector_ref"};
        bool any = false;
        for (size_t i = 0; i < sizeof(sel_keys) / sizeof(sel_keys[0]); i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(target, sel_keys[i]);
            if (cJSON_IsString(v) && v->valuestring && v->valuestring[0] != '\0') {
                if (!cJSON_AddStringToObject(t_out, sel_keys[i], v->valuestring)) {
                    cJSON_Delete(t_out);
                    cJSON_Delete(env);
                    return NULL;
                }
                any = true;
            }
        }
        if (any) {
            cJSON_AddItemToObject(env, "target", t_out);
        } else {
            cJSON_Delete(t_out);
        }
    }

    /* parameters: echoed verbatim. They are hook parameters, not a raw
     * priority/effect payload. The receiver decides whether and how they map
     * into a local control event; the C6 hook path reads level_milli
     * (0..1000) / level (0..100). */
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req_root, "parameters");
    if (cJSON_IsObject(params)) {
        cJSON *p_out = cJSON_Duplicate(params, true);
        if (!p_out) {
            cJSON_Delete(env);
            return NULL;
        }
        cJSON_AddItemToObject(env, "parameters", p_out);
    }

    char *text = cJSON_PrintUnformatted(env);
    if (echo_out) {
        *echo_out = env; /* ownership transferred to caller for the response echo */
    } else {
        cJSON_Delete(env);
    }
    if (!text && echo_out) {
        cJSON_Delete(env);
        *echo_out = NULL;
    }
    return text;
}

/* Emit the envelope as a CoAP NON POST to coap://[dest]/mesh/event under the
 * held OT lock, framed EXACTLY like the device emitter. Returns OT_ERROR_NONE
 * on a successful send (NON has no ack; success means the request was queued).
 * The caller holds the OT lock; instance is the live OT instance. */
static otError trigger_emit_coap(otInstance *instance,
                                 const otIp6Address *dest,
                                 const char *payload,
                                 size_t payload_len)
{
    otMessage *request = otCoapNewMessage(instance, NULL);
    if (!request) {
        return OT_ERROR_NO_BUFS;
    }

    /* NON-confirmable POST, mirroring mesh_coap_emit_control_event. */
    otCoapMessageInit(request, OT_COAP_TYPE_NON_CONFIRMABLE, OT_COAP_CODE_POST);
    otError err = otCoapMessageAppendUriPathOptions(request, BOS_TRIGGER_PATH_EVENT);
    if (err == OT_ERROR_NONE) {
        err = otCoapMessageAppendContentFormatOption(request, OT_COAP_OPTION_CONTENT_FORMAT_JSON);
    }
    if (err == OT_ERROR_NONE) {
        err = otCoapMessageSetPayloadMarker(request);
    }
    if (err == OT_ERROR_NONE) {
        err = otMessageAppend(request, payload, (uint16_t)payload_len);
    }
    if (err == OT_ERROR_NONE) {
        otMessageInfo info;
        memset(&info, 0, sizeof(info));
        info.mPeerAddr = *dest;
        info.mPeerPort = OT_DEFAULT_COAP_PORT;
        /* No response handler: NON has no single ack-bearing responder. */
        err = otCoapSendRequest(instance, request, &info, NULL, NULL);
    }
    if (err != OT_ERROR_NONE) {
        otMessageFree(request);
    }
    return err;
}

esp_err_t trigger_post_handler(httpd_req_t *req)
{
    if (!bos_commissioning_request_authorized(req)) {
        return send_unauthorized(req);
    }

    char body[BOS_TRIGGER_BODY_MAX];
    if (read_request_body(req, body, sizeof(body)) < 0) {
        return send_status_json(req, "400 Bad Request", "empty or oversized body");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_status_json(req, "400 Bad Request", "invalid JSON");
    }

    /* source_id is the only hard-required envelope field (matches the device
     * parse_event_envelope contract). */
    const cJSON *source_j = cJSON_GetObjectItemCaseSensitive(root, "source_id");
    if (!cJSON_IsString(source_j) || !source_j->valuestring || source_j->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_status_json(req, "400 Bad Request", "source_id is required");
    }
    char source_id[BOS_TRIGGER_SOURCE_ID_MAX];
    size_t source_id_len = strlen(source_j->valuestring);
    if (source_id_len >= sizeof(source_id)) {
        cJSON_Delete(root);
        return send_status_json(req, "400 Bad Request", "source_id too long");
    }
    memcpy(source_id, source_j->valuestring, source_id_len + 1);

    /* The wire must NOT assert priority. Reject loudly rather than silently
     * dropping it - a sender attempting to claim authority is a contract
     * violation, not a field to ignore. */
    if (cJSON_GetObjectItemCaseSensitive(root, "priority") != NULL) {
        cJSON_Delete(root);
        return send_status_json(req, "400 Bad Request",
                                "priority is not permitted on the trigger envelope");
    }

    /* Unicast target resolution. The BR resolves by durable EUI-64 via the SRP
     * host walk, using the trigger-specific mesh-local address selection.
     * spatial_id alone cannot be resolved to a unicast address here - that is
     * a site-server responsibility - so it is a clear 4xx, never a multicast
     * fallback. */
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
    const cJSON *eui_j = cJSON_IsObject(target)
                             ? cJSON_GetObjectItemCaseSensitive(target, "eui64")
                             : NULL;
    if (!cJSON_IsString(eui_j) || !eui_j->valuestring || eui_j->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_status_json(req, "400 Bad Request",
                                "target.eui64 is required to resolve a unicast device address");
    }
    char eui64[17];
    if (!proxy_normalise_eui64(eui_j->valuestring, strlen(eui_j->valuestring), eui64)) {
        cJSON_Delete(root);
        return send_status_json(req, "400 Bad Request",
                                "target.eui64 must be 16 (or fffe-elided 12) hex chars");
    }

    /* Build the conforming envelope before taking the OT lock. */
    cJSON *echo = NULL;
    char *payload = trigger_build_envelope(root, source_id, &echo);
    cJSON_Delete(root);
    if (!payload) {
        cJSON_Delete(echo);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to build envelope");
        return ESP_OK;
    }
    size_t payload_len = strlen(payload);

    /* Resolve + emit under the held OT lock. The BR phonebook is the primary
     * logical EUI-64 -> ML-EID map; SRP remains a mesh-local diagnostic
     * fallback only. */
    proxy_target_t tgt = {.http_port = 80};
    const char *resolver = "";
    bool phonebook_matched = false;
    bool phonebook_bad_scope = false;
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        cJSON_free(payload);
        cJSON_Delete(echo);
        return thread_diag_send_unavailable(req, "openthread lock timeout");
    }
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        cJSON_free(payload);
        cJSON_Delete(echo);
        return thread_diag_send_unavailable(req, "openthread not running");
    }
    bool resolved = phonebook_resolve_eui64(instance, eui64, &tgt, &phonebook_matched, NULL);
    if (resolved) {
        resolver = "phonebook";
    } else if (phonebook_matched) {
        phonebook_bad_scope = true;
    } else {
        proxy_addr_scope_t srp_scope = PROXY_SCOPE_UNUSABLE;
        resolved = trigger_resolve_eui64(instance, eui64, &tgt, &srp_scope);
        if (resolved) {
            resolver = "srp_mesh_local_fallback";
            ESP_LOGW(TAG,
                     "trigger resolve %s: phonebook miss, using SRP ML-EID fallback",
                     eui64);
        }
    }
    otError emit_err = OT_ERROR_NONE;
    if (resolved) {
        emit_err = trigger_emit_coap(instance, &tgt.address, payload, payload_len);
    }
    esp_openthread_lock_release();

    cJSON_free(payload);

    if (phonebook_bad_scope) {
        cJSON_Delete(echo);
        return send_status_json(req, "409 Conflict",
                                "phonebook eid for target.eui64 is not mesh-local");
    }
    if (!resolved) {
        cJSON_Delete(echo);
        return send_status_json(req, "404 Not Found",
                                "device not found in phonebook or SRP with mesh-local EID");
    }

    char dest_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&tgt.address, dest_str, sizeof(dest_str));

    if (emit_err != OT_ERROR_NONE) {
        char err_json[160];
        snprintf(err_json, sizeof(err_json),
                 "coap emit failed: %s", otThreadErrorToString(emit_err));
        cJSON_Delete(echo);
        return send_status_json(req, "502 Bad Gateway", err_json);
    }

    ESP_LOGI(TAG,
             "trigger envelope emitted: src=%s eui64=%s resolver=%s dest=[%s] uri=%s len=%u",
             source_id, eui64, resolver, dest_str, BOS_TRIGGER_PATH_EVENT,
             (unsigned)payload_len);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        cJSON_Delete(echo);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "emitted", true);
    cJSON_AddStringToObject(resp, "resolver", resolver);
    cJSON_AddStringToObject(resp, "uri", BOS_TRIGGER_PATH_EVENT);
    cJSON_AddStringToObject(resp, "dest", dest_str);
    if (echo) {
        cJSON_AddItemToObject(resp, "envelope", echo); /* takes ownership of echo */
        echo = NULL;
    }
    esp_err_t ret = send_cjson(req, resp);
    cJSON_Delete(resp);
    cJSON_Delete(echo);
    return ret;
}
