/**
 * Building OS Border Router: Thread joiner acceptance (commissioning
 * phase 2b).
 *
 * docs/06.2-commissioning-workflow.md phase 2b: "The commissioner
 * application, running against the border router or the site server, accepts
 * the EUI-64 and PSKd, completes the Thread Commissioning handshake, and
 * pushes the Thread Operational Dataset to the device." This file is that
 * commissioner surface on the BR. Acceptance is operator-driven: the caller
 * supplies the device's EUI-64 and its per-device PSKd (docs/09.1-thread.md
 * section 4.2); the firmware never derives or invents a PSKd and never
 * accepts arbitrary joiners.
 *
 * docs/08.8-border-router.md section 11 gates joiner acceptance on
 * reconciling the upstream web UI's raw scan/join/form controls with the
 * documented commissioning flow. The reconciliation here: those upstream
 * controls stay out of the BOS path entirely; the only joiner-acceptance
 * write is the token-authed POST /bos/joiner/accept below, scoped to one
 * explicit EUI-64 + PSKd per call.
 *
 * OT API citations (ESP-IDF bundled OpenThread headers,
 * components/openthread/openthread/include/openthread/):
 *   otCommissionerStart             commissioner.h:180
 *   otCommissionerAddJoiner         commissioner.h:231
 *   otCommissionerGetNextJoinerInfo commissioner.h:266
 *   otCommissionerGetState          commissioner.h:459
 *   otCommissionerState enum        commissioner.h:60
 *   otCommissionerStateCallback     commissioner.h:153
 *   otCommissionerJoinerCallback    commissioner.h:163
 *   otJoinerInfo / otJoinerPskd     commissioner.h:113..146
 *   otExtAddress                    platform/radio.h:176
 *   otThreadErrorToString           error.h:257
 * Requires CONFIG_OPENTHREAD_COMMISSIONER=y (set in this project's
 * sdkconfig and sdkconfig.defaults).
 */

#include "bos_joiner.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_commissioning.h"
#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "openthread/commissioner.h"
#include "openthread/error.h"
#include "openthread/instance.h"
#include "sdkconfig.h"

static const char *TAG = "bos_joiner";

#define BOS_JOINER_BODY_MAX         512U
#define BOS_JOINER_PSKD_MIN_LEN     6U
#define BOS_JOINER_PSKD_MAX_LEN     32U  /* OT_JOINER_MAX_PSKD_LENGTH */
#define BOS_JOINER_DEFAULT_TIMEOUT  300U /* seconds */
#define BOS_JOINER_TIMEOUT_MAX      3600U
/* Matches CONFIG_OPENTHREAD_COMM_MAX_JOINER_ENTRIES=2: queueing more than
 * the OT joiner table holds would only defer an honest NoBufs error. */
#define BOS_JOINER_PENDING_MAX      2

/* Joiner accepted while the commissioner petition is still in flight.
 * Flushed into otCommissionerAddJoiner from the commissioner state callback
 * (runs in the OpenThread task with the OT lock held). All access to this
 * table happens under the OpenThread lock. */
typedef struct {
    bool valid;
    otExtAddress eui64;
    char pskd[BOS_JOINER_PSKD_MAX_LEN + 1];
    uint32_t timeout_s;
} pending_joiner_t;

static pending_joiner_t s_pending[BOS_JOINER_PENDING_MAX];

/* Commissioner joiner event trail for /bos/thread-diag: the last
 * BOS_JOINER_EVENT_TRAIL_LEN joiner-callback events with monotonic esp_timer
 * timestamps. Written from commissioner_joiner_callback (OpenThread task,
 * OT lock held) and read by bos_joiner_event_trail_json with the caller
 * holding the same lock, so no extra synchronization is needed. */
#define BOS_JOINER_EVENT_TRAIL_LEN 16U

typedef struct {
    bool valid;
    uint64_t uptime_ms;
    const char *event; /* static string from the callback's event_names table */
    char eui64_hex[17];
} joiner_event_entry_t;

static joiner_event_entry_t s_event_trail[BOS_JOINER_EVENT_TRAIL_LEN];
static size_t s_event_trail_next;
static uint32_t s_event_trail_total;

static const char *commissioner_state_str(otCommissionerState state)
{
    switch (state) {
    case OT_COMMISSIONER_STATE_DISABLED:
        return "disabled";
    case OT_COMMISSIONER_STATE_PETITION:
        return "petitioning";
    case OT_COMMISSIONER_STATE_ACTIVE:
        return "active";
    default:
        return "unknown";
    }
}

static void eui64_to_hex(const otExtAddress *eui64, char out[17])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < OT_EXT_ADDRESS_SIZE; i++) {
        out[i * 2] = hex[eui64->m8[i] >> 4];
        out[i * 2 + 1] = hex[eui64->m8[i] & 0x0f];
    }
    out[16] = '\0';
}

/* Accepts 16 hex digits, with optional ':' or '-' group separators. */
static bool parse_eui64(const char *text, otExtAddress *out)
{
    size_t nibbles = 0;
    uint8_t bytes[OT_EXT_ADDRESS_SIZE] = {0};

    for (const char *p = text; *p != '\0'; p++) {
        if (*p == ':' || *p == '-') {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        if (nibbles >= OT_EXT_ADDRESS_SIZE * 2) {
            return false;
        }
        uint8_t value = (uint8_t)(isdigit((unsigned char)*p) ? *p - '0' : tolower((unsigned char)*p) - 'a' + 10);
        bytes[nibbles / 2] |= (nibbles % 2 == 0) ? (uint8_t)(value << 4) : value;
        nibbles++;
    }

    if (nibbles != OT_EXT_ADDRESS_SIZE * 2) {
        return false;
    }
    memcpy(out->m8, bytes, sizeof(bytes));
    return true;
}

/* Runs in the OpenThread task with the OT lock held. */
static void flush_pending_joiners(otInstance *instance)
{
    for (size_t i = 0; i < BOS_JOINER_PENDING_MAX; i++) {
        if (!s_pending[i].valid) {
            continue;
        }
        char eui64_hex[17];
        eui64_to_hex(&s_pending[i].eui64, eui64_hex);
        otError err = otCommissionerAddJoiner(instance,
                                              &s_pending[i].eui64,
                                              s_pending[i].pskd,
                                              s_pending[i].timeout_s);
        if (err == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "queued joiner %s added (timeout %us)", eui64_hex, (unsigned)s_pending[i].timeout_s);
        } else {
            ESP_LOGE(TAG, "queued joiner %s failed: %s", eui64_hex, otThreadErrorToString(err));
        }
        memset(&s_pending[i], 0, sizeof(s_pending[i]));
    }
}

static void commissioner_state_callback(otCommissionerState state, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "commissioner state: %s", commissioner_state_str(state));

    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        flush_pending_joiners(esp_openthread_get_instance());
    } else if (state == OT_COMMISSIONER_STATE_DISABLED) {
        for (size_t i = 0; i < BOS_JOINER_PENDING_MAX; i++) {
            if (s_pending[i].valid) {
                char eui64_hex[17];
                eui64_to_hex(&s_pending[i].eui64, eui64_hex);
                ESP_LOGW(TAG, "commissioner petition ended; dropping queued joiner %s (re-POST /bos/joiner/accept)",
                         eui64_hex);
                memset(&s_pending[i], 0, sizeof(s_pending[i]));
            }
        }
    }
}

static void commissioner_joiner_callback(otCommissionerJoinerEvent event,
                                         const otJoinerInfo *joiner_info,
                                         const otExtAddress *joiner_id,
                                         void *context)
{
    (void)context;
    static const char *event_names[] = {"start", "connected", "finalize", "end", "removed"};
    const char *event_name = ((size_t)event < sizeof(event_names) / sizeof(event_names[0]))
                                 ? event_names[event]
                                 : "unknown";
    char eui64_hex[17] = "unknown";
    if (joiner_id != NULL) {
        eui64_to_hex(joiner_id, eui64_hex);
    } else if (joiner_info != NULL && joiner_info->mType == OT_JOINER_INFO_TYPE_EUI64) {
        eui64_to_hex(&joiner_info->mSharedId.mEui64, eui64_hex);
    }
    ESP_LOGI(TAG, "joiner event %s (id %s)", event_name, eui64_hex);

    /* Record into the event trail (this callback runs in the OpenThread task
     * with the OT lock held). */
    joiner_event_entry_t *slot = &s_event_trail[s_event_trail_next];
    slot->valid = true;
    slot->uptime_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    slot->event = event_name;
    snprintf(slot->eui64_hex, sizeof(slot->eui64_hex), "%s", eui64_hex);
    s_event_trail_next = (s_event_trail_next + 1U) % BOS_JOINER_EVENT_TRAIL_LEN;
    s_event_trail_total++;
}

static bool queue_pending_joiner(const otExtAddress *eui64, const char *pskd, uint32_t timeout_s)
{
    for (size_t i = 0; i < BOS_JOINER_PENDING_MAX; i++) {
        if (!s_pending[i].valid) {
            s_pending[i].valid = true;
            s_pending[i].eui64 = *eui64;
            snprintf(s_pending[i].pskd, sizeof(s_pending[i].pskd), "%s", pskd);
            s_pending[i].timeout_s = timeout_s;
            return true;
        }
    }
    return false;
}

/* Error paths return ESP_OK after the response is sent; a non-ESP_OK handler
 * return makes esp_http_server close the socket immediately and the close
 * emits a TCP RST that destroys the in-flight response (same rule as
 * bos_commissioning.c send_error_json). */
static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *error)
{
    char json[160];
    int written = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", error);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"internal error\"}");
    } else {
        httpd_resp_sendstr(req, json);
    }
    return ESP_OK;
}

esp_err_t bos_joiner_accept_http_post(httpd_req_t *req)
{
    if (!bos_commissioning_request_authorized(req)) {
        return send_error_json(req, "401 Unauthorized", "unauthorized");
    }

    if (req->content_len == 0 || req->content_len > BOS_JOINER_BODY_MAX) {
        return send_error_json(req, "400 Bad Request", "body must be 1..512 bytes");
    }

    char body[BOS_JOINER_BODY_MAX + 1];
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            /* Socket-level receive failure: connection is unusable, close it. */
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    body[req->content_len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_error_json(req, "400 Bad Request", "invalid JSON");
    }

    const cJSON *eui64_item = cJSON_GetObjectItemCaseSensitive(root, "eui64");
    const cJSON *pskd_item = cJSON_GetObjectItemCaseSensitive(root, "pskd");
    const cJSON *timeout_item = cJSON_GetObjectItemCaseSensitive(root, "timeout_s");

    if (!cJSON_IsString(eui64_item) || !eui64_item->valuestring ||
        !cJSON_IsString(pskd_item) || !pskd_item->valuestring) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "eui64 and pskd are required strings");
    }

    otExtAddress eui64;
    if (!parse_eui64(eui64_item->valuestring, &eui64)) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "eui64 must be 16 hex digits");
    }

    /* The PSKd is taken exactly as the operator supplied it (per-device PSKd
     * model, docs/09.1-thread.md section 4.2). Length-check here; OpenThread
     * validates the Thread PSKd alphabet and rejects with InvalidArgs. */
    char pskd[BOS_JOINER_PSKD_MAX_LEN + 1];
    size_t pskd_len = strlen(pskd_item->valuestring);
    if (pskd_len < BOS_JOINER_PSKD_MIN_LEN || pskd_len > BOS_JOINER_PSKD_MAX_LEN) {
        cJSON_Delete(root);
        return send_error_json(req, "400 Bad Request", "pskd must be 6..32 characters");
    }
    snprintf(pskd, sizeof(pskd), "%s", pskd_item->valuestring);

    uint32_t timeout_s = BOS_JOINER_DEFAULT_TIMEOUT;
    if (timeout_item != NULL) {
        if (!cJSON_IsNumber(timeout_item) || timeout_item->valuedouble < 1 ||
            timeout_item->valuedouble > BOS_JOINER_TIMEOUT_MAX) {
            cJSON_Delete(root);
            return send_error_json(req, "400 Bad Request", "timeout_s must be 1..3600");
        }
        timeout_s = (uint32_t)timeout_item->valuedouble;
    }
    cJSON_Delete(root);

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {
        return send_error_json(req, "503 Service Unavailable", "openthread lock timeout");
    }

    /* esp_openthread_get_instance never returns NULL (static singleton);
     * otInstanceIsInitialized (instance.h:150) is the real readiness gate. */
    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        return send_error_json(req, "503 Service Unavailable", "openthread not running");
    }

    char eui64_hex[17];
    eui64_to_hex(&eui64, eui64_hex);

    otCommissionerState state = otCommissionerGetState(instance);
    const char *joiner_disposition = NULL;

    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        /* Drain anything still queued (covers a commissioner started outside
         * our state callback) before adding this entry. */
        flush_pending_joiners(instance);
        otError err = otCommissionerAddJoiner(instance, &eui64, pskd, timeout_s);
        if (err != OT_ERROR_NONE) {
            esp_openthread_lock_release();
            ESP_LOGW(TAG, "otCommissionerAddJoiner(%s) failed: %s", eui64_hex, otThreadErrorToString(err));
            if (err == OT_ERROR_INVALID_ARGS) {
                return send_error_json(req, "400 Bad Request", "openthread rejected eui64/pskd (InvalidArgs)");
            }
            if (err == OT_ERROR_NO_BUFS) {
                return send_error_json(req, "503 Service Unavailable", "joiner table full");
            }
            return send_error_json(req, "500 Internal Server Error", "otCommissionerAddJoiner failed");
        }
        joiner_disposition = "added";
        ESP_LOGI(TAG, "joiner %s accepted (timeout %us)", eui64_hex, (unsigned)timeout_s);
    } else {
        if (!queue_pending_joiner(&eui64, pskd, timeout_s)) {
            esp_openthread_lock_release();
            return send_error_json(req, "503 Service Unavailable", "pending joiner queue full");
        }
        if (state == OT_COMMISSIONER_STATE_DISABLED) {
            otError err = otCommissionerStart(instance,
                                              commissioner_state_callback,
                                              commissioner_joiner_callback,
                                              NULL);
            if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
                /* Drop the entry we just queued; the start failed. */
                commissioner_state_callback(OT_COMMISSIONER_STATE_DISABLED, NULL);
                esp_openthread_lock_release();
                ESP_LOGW(TAG, "otCommissionerStart failed: %s", otThreadErrorToString(err));
                if (err == OT_ERROR_INVALID_STATE) {
                    return send_error_json(req, "409 Conflict", "thread interface not attached; cannot start commissioner");
                }
                return send_error_json(req, "500 Internal Server Error", "otCommissionerStart failed");
            }
        }
        joiner_disposition = "queued";
        ESP_LOGI(TAG, "joiner %s queued until commissioner petition completes", eui64_hex);
    }

    state = otCommissionerGetState(instance);
    esp_openthread_lock_release();

    char json[224];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"ok\":true,\"commissioner_state\":\"%s\",\"joiner\":\"%s\","
                           "\"eui64\":\"%s\",\"timeout_s\":%u}",
                           commissioner_state_str(state),
                           joiner_disposition,
                           eui64_hex,
                           (unsigned)timeout_s);
    if (written < 0 || written >= (int)sizeof(json)) {
        return send_error_json(req, "500 Internal Server Error", "response render failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

esp_err_t bos_joiner_status_http_get(httpd_req_t *req)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        return send_error_json(req, "503 Service Unavailable", "openthread lock timeout");
    }

    otInstance *instance = esp_openthread_get_instance();
    if (!otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        return send_error_json(req, "503 Service Unavailable", "openthread not running");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *joiners = resp ? cJSON_AddArrayToObject(resp, "joiners") : NULL;
    cJSON *pending = resp ? cJSON_AddArrayToObject(resp, "pending") : NULL;
    bool render_ok = resp != NULL && joiners != NULL && pending != NULL &&
                     cJSON_AddStringToObject(resp, "commissioner_state",
                                             commissioner_state_str(otCommissionerGetState(instance))) != NULL;

    /* Active joiner entries straight from the OT commissioner table.
     * PSKd values are deliberately omitted. */
    if (render_ok) {
        uint16_t iterator = 0;
        otJoinerInfo info;
        while (render_ok && otCommissionerGetNextJoinerInfo(instance, &iterator, &info) == OT_ERROR_NONE) {
            cJSON *entry = cJSON_CreateObject();
            render_ok = entry != NULL && cJSON_AddItemToArray(joiners, entry);
            if (!render_ok) {
                cJSON_Delete(entry);
                break;
            }
            switch (info.mType) {
            case OT_JOINER_INFO_TYPE_EUI64: {
                char eui64_hex[17];
                eui64_to_hex(&info.mSharedId.mEui64, eui64_hex);
                render_ok = cJSON_AddStringToObject(entry, "type", "eui64") != NULL &&
                            cJSON_AddStringToObject(entry, "eui64", eui64_hex) != NULL;
                break;
            }
            case OT_JOINER_INFO_TYPE_ANY:
                render_ok = cJSON_AddStringToObject(entry, "type", "any") != NULL;
                break;
            case OT_JOINER_INFO_TYPE_DISCERNER:
            default:
                render_ok = cJSON_AddStringToObject(entry, "type", "discerner") != NULL;
                break;
            }
            render_ok = render_ok &&
                        cJSON_AddNumberToObject(entry, "expiration_ms", (double)info.mExpirationTime) != NULL;
        }
    }

    if (render_ok) {
        for (size_t i = 0; render_ok && i < BOS_JOINER_PENDING_MAX; i++) {
            if (!s_pending[i].valid) {
                continue;
            }
            char eui64_hex[17];
            eui64_to_hex(&s_pending[i].eui64, eui64_hex);
            cJSON *entry = cJSON_CreateObject();
            render_ok = entry != NULL && cJSON_AddItemToArray(pending, entry);
            if (!render_ok) {
                cJSON_Delete(entry);
                break;
            }
            render_ok = cJSON_AddStringToObject(entry, "eui64", eui64_hex) != NULL &&
                        cJSON_AddNumberToObject(entry, "timeout_s", (double)s_pending[i].timeout_s) != NULL;
        }
    }

    esp_openthread_lock_release();

    char *printed = render_ok ? cJSON_PrintUnformatted(resp) : NULL;
    cJSON_Delete(resp);
    if (printed == NULL) {
        return send_error_json(req, "500 Internal Server Error", "failed to render joiner status");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, printed);
    cJSON_free(printed);
    return ESP_OK;
}

/* CONFIG_NEWLIB_NANO_FORMAT has no 64-bit printf support; %llu misaligns the
 * variadic args (LoadProhibited panic). Format u64 manually, same rule as
 * bos_diagnostics_server.c u64_to_dec. */
static void u64_to_dec(uint64_t value, char out[21])
{
    char tmp[21];
    size_t i = 0;
    do {
        tmp[i++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0 && i < sizeof(tmp) - 1);
    size_t n = 0;
    while (i > 0) {
        out[n++] = tmp[--i];
    }
    out[n] = '\0';
}

int bos_joiner_commissioner_json(char *out, size_t out_len)
{
    otInstance *instance = esp_openthread_get_instance();
    int written = snprintf(out, out_len, "{\"state\":\"%s\",\"joiners\":[",
                           commissioner_state_str(otCommissionerGetState(instance)));
    if (written < 0 || written >= (int)out_len) {
        return -1;
    }
    size_t pos = (size_t)written;

    uint16_t iterator = 0;
    otJoinerInfo info;
    bool first = true;
    while (otCommissionerGetNextJoinerInfo(instance, &iterator, &info) == OT_ERROR_NONE) {
        const char *type = "discerner";
        char eui64_hex[17] = "";
        if (info.mType == OT_JOINER_INFO_TYPE_EUI64) {
            type = "eui64";
            eui64_to_hex(&info.mSharedId.mEui64, eui64_hex);
        } else if (info.mType == OT_JOINER_INFO_TYPE_ANY) {
            type = "any";
        }
        if (eui64_hex[0] != '\0') {
            written = snprintf(out + pos, out_len - pos,
                               "%s{\"type\":\"%s\",\"eui64\":\"%s\",\"expiration_ms\":%u}",
                               first ? "" : ",", type, eui64_hex, (unsigned)info.mExpirationTime);
        } else {
            written = snprintf(out + pos, out_len - pos,
                               "%s{\"type\":\"%s\",\"expiration_ms\":%u}",
                               first ? "" : ",", type, (unsigned)info.mExpirationTime);
        }
        if (written < 0 || written >= (int)(out_len - pos)) {
            return -1;
        }
        pos += (size_t)written;
        first = false;
    }

    written = snprintf(out + pos, out_len - pos, "]}");
    if (written < 0 || written >= (int)(out_len - pos)) {
        return -1;
    }
    pos += (size_t)written;
    return (int)pos;
}

int bos_joiner_event_trail_json(char *out, size_t out_len)
{
    int written = snprintf(out, out_len, "{\"total\":%u,\"events\":[", (unsigned)s_event_trail_total);
    if (written < 0 || written >= (int)out_len) {
        return -1;
    }
    size_t pos = (size_t)written;

    bool first = true;
    for (size_t i = 0; i < BOS_JOINER_EVENT_TRAIL_LEN; i++) {
        const joiner_event_entry_t *entry = &s_event_trail[(s_event_trail_next + i) % BOS_JOINER_EVENT_TRAIL_LEN];
        if (!entry->valid) {
            continue;
        }
        char uptime_str[21];
        u64_to_dec(entry->uptime_ms, uptime_str);
        written = snprintf(out + pos, out_len - pos,
                           "%s{\"uptime_ms\":%s,\"event\":\"%s\",\"eui64\":\"%s\"}",
                           first ? "" : ",", uptime_str, entry->event, entry->eui64_hex);
        if (written < 0 || written >= (int)(out_len - pos)) {
            return -1;
        }
        pos += (size_t)written;
        first = false;
    }

    written = snprintf(out + pos, out_len - pos, "]}");
    if (written < 0 || written >= (int)(out_len - pos)) {
        return -1;
    }
    pos += (size_t)written;
    return (int)pos;
}
