/**
 * Building OS Border Router: ledger ingress protected push handler.
 *
 * Owns POST /bos/ledger/push: authorization, the in-flight push guard,
 * staged partition writes, and the accept-then-cache push flow. Envelope
 * validation lives in bos_ledger_ingress_cbor.c; state, metadata, and the
 * RAM store live in bos_ledger_ingress.c.
 */

#include "bos_ledger_ingress.h"
#include "bos_ledger_ingress_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_server_registration.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "bos_ingress";

/* In-flight push guard (docs/08.8-border-router.md s12): a push arriving
 * while a previous push is still being received/validated/persisted gets
 * 409 Conflict with the in-flight phase named in the body. esp_http_server
 * runs handlers on a single task today, so two push handlers cannot
 * interleave on the current httpd topology; the guard makes the documented
 * contract real independent of that topology and is the honest source of
 * the rejected push's "phase" field. */
static portMUX_TYPE s_push_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_push_in_flight = false;
static char s_push_phase[12] = "";

/* Tries to claim the single push slot. Returns false when another push is
 * already in flight. */
bool push_guard_acquire(const char *initial_phase)
{
    bool acquired = false;
    taskENTER_CRITICAL(&s_push_mux);
    if (!s_push_in_flight) {
        s_push_in_flight = true;
        strlcpy(s_push_phase, initial_phase, sizeof(s_push_phase));
        acquired = true;
    }
    taskEXIT_CRITICAL(&s_push_mux);
    return acquired;
}

void push_guard_set_phase(const char *phase)
{
    taskENTER_CRITICAL(&s_push_mux);
    strlcpy(s_push_phase, phase, sizeof(s_push_phase));
    taskEXIT_CRITICAL(&s_push_mux);
}

void push_guard_release(void)
{
    taskENTER_CRITICAL(&s_push_mux);
    s_push_in_flight = false;
    s_push_phase[0] = '\0';
    taskEXIT_CRITICAL(&s_push_mux);
}

/* 409 Conflict for a concurrent push. Sends the response and returns ESP_OK
 * (socket-RST rule, see bos_ledger_ingress_http_push). */
esp_err_t push_send_conflict(httpd_req_t *req)
{
    char phase[sizeof(s_push_phase)];
    taskENTER_CRITICAL(&s_push_mux);
    strlcpy(phase, s_push_phase, sizeof(phase));
    taskEXIT_CRITICAL(&s_push_mux);

    char json[96];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"ok\":false,\"error\":\"push_in_flight\",\"phase\":\"%s\"}",
                           phase[0] != '\0' ? phase : "unknown");
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (written < 0 || written >= (int)sizeof(json)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"push_in_flight\",\"phase\":\"unknown\"}");
    } else {
        httpd_resp_sendstr(req, json);
    }
    return ESP_OK;
}

static esp_err_t header_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return ESP_ERR_NOT_FOUND;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_len);
}

static bool push_authorized(httpd_req_t *req)
{
    char expected[BOS_LEDGER_HEADER_TOKEN_MAX];
    char provided[BOS_LEDGER_HEADER_TOKEN_MAX];

    if (bos_server_registration_get_token(expected, sizeof(expected)) == ESP_OK && expected[0] != '\0') {
        if (header_value(req, "X-Device-Token", provided, sizeof(provided)) == ESP_OK &&
            strcmp(expected, provided) == 0) {
            return true;
        }
    }

#ifdef CONFIG_BOS_SERVER_API_KEY
    if (CONFIG_BOS_SERVER_API_KEY[0] != '\0') {
        if (header_value(req, "x-api-key", provided, sizeof(provided)) == ESP_OK &&
            strcmp(CONFIG_BOS_SERVER_API_KEY, provided) == 0) {
            return true;
        }
    }
#endif

    return false;
}

esp_err_t write_staged_partition(const esp_partition_t *partition,
                                 const uint8_t *bytes,
                                 size_t len)
{
    if (!partition || !bytes || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        return err;
    }
    return esp_partition_write(partition, 0, bytes, len);
}

/* Error paths must return ESP_OK once a response has been sent. A non-ESP_OK
 * handler return makes esp_http_server close the socket immediately
 * (httpd_uri.c: "Handler returns error, this socket should be closed") and
 * skips the httpd_req_delete() purge of any unread POST body, so close() on
 * the socket with unreceived data emits a TCP RST that destroys the in-flight
 * response body (observed on hardware: 401 headers then connection reset).
 * Return ESP_FAIL only when the socket itself is unusable. */
esp_err_t bos_ledger_ingress_http_push(httpd_req_t *req)
{
    if (!push_authorized(req)) {
        bos_ledger_set_last_error("auth", ESP_FAIL, "unauthorized ledger push", (uint32_t)req->content_len, "");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unauthorized\"}");
        return ESP_OK;
    }

    /* Conflict guard (docs/08.8 s12): one push at a time. A second push
     * while this one is receiving/validating/persisting gets 409 with the
     * in-flight phase named; the in-flight push's last_error context is
     * left untouched. */
    if (!push_guard_acquire("receiving")) {
        return push_send_conflict(req);
    }
    clear_last_error();

    if (req->content_len == 0 || req->content_len > BOS_LEDGER_PUSH_MAX_BYTES) {
        bos_ledger_set_last_error("body_size", ESP_ERR_INVALID_SIZE, "ledger body must be 1..65536 bytes", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ledger body must be 1..65536 bytes");
        push_guard_release();
        return ESP_OK;
    }

    uint8_t *body = (uint8_t *)malloc(req->content_len);
    if (!body) {
        bos_ledger_set_last_error("receive", ESP_ERR_NO_MEM, "out of memory allocating ledger body", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        push_guard_release();
        return ESP_OK;
    }

    bos_ledger_state_t prior_state = bos_ledger_ingress_state();
    bos_ledger_ingress_set_state(BOS_LEDGER_STATE_RECEIVING);
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, (char *)body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            bos_ledger_ingress_set_state(prior_state);
            bos_ledger_set_last_error("receive", ESP_FAIL, "failed to receive ledger body", (uint32_t)req->content_len, "");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to receive ledger body");
            push_guard_release();
            /* Socket-level receive failure: connection is unusable, close it. */
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    bos_ledger_ingress_set_state(BOS_LEDGER_STATE_VALIDATING);
    push_guard_set_phase("validating");
    bos_ledger_body_view_t view = {0};
    esp_err_t err = validate_ledger_envelope(body, req->content_len, &view);
    if (err != ESP_OK) {
        free(body);
        bos_ledger_ingress_set_state(prior_state);
        bos_ledger_set_last_error("validate", err, "ledger envelope digest validation failed", (uint32_t)req->content_len, "");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ledger envelope digest validation failed");
        push_guard_release();
        return ESP_OK;
    }

    /* ACCEPT: the validated ledger becomes the serving source immediately.
     * Capture metadata before the RAM store takes ownership of body (view
     * pointers reference the body buffer and may dangle after the RAM copy
     * is cleared on persistence success). */
    uint32_t ledger_version = view.ledger_version;
    uint32_t size_bytes = (uint32_t)req->content_len;
    uint8_t digest_bytes[BOS_LEDGER_DIGEST_LEN];
    memcpy(digest_bytes, view.manifest_digest, BOS_LEDGER_DIGEST_LEN);
    uint32_t chunks = (size_bytes + BOS_LEDGER_CHUNK_SIZE - 1U) / BOS_LEDGER_CHUNK_SIZE;

    uint8_t *accepted = body;
    ram_ledger_set(accepted, req->content_len, &view);
    body = NULL; /* owned by the RAM ledger store now */
    bos_ledger_ingress_set_state(BOS_LEDGER_STATE_ACTIVE);
    push_guard_set_phase("persisting");

    /* CACHE WRITE: flash partition + NVS metadata are a replaceable
     * delivery cache (server is the source of truth). Any failure here is
     * a logged warning and a degraded cache, never a push failure. Push
     * handling is serialized on the single httpd task, so the RAM copy
     * installed above cannot be swapped out under this write. */
    uint8_t active_idx = 0;
    if (read_partition_idx(&active_idx) != ESP_OK) {
        active_idx = 1;
    }
    uint8_t target_idx = active_idx == 0 ? 1 : 0;
    const char *target_label = target_idx == 0 ? BOS_LEDGER_PARTITION_A : BOS_LEDGER_PARTITION_B;
    const esp_partition_t *target = partition_for_idx(target_idx);

    if (!target) {
        err = ESP_ERR_NOT_FOUND;
        bos_ledger_set_last_error("partition_lookup", err, "inactive ledger partition missing", size_bytes, target_label);
    } else {
        err = write_staged_partition(target, accepted, size_bytes);
        if (err == ESP_OK) {
            err = commit_active_metadata(target_idx, ledger_version, digest_bytes, size_bytes);
            if (err != ESP_OK) {
                bos_ledger_set_last_error("metadata_commit", err, "failed to commit active ledger metadata", size_bytes, target->label);
            }
        } else {
            bos_ledger_set_last_error("partition_write", err, "failed to write staged ledger partition", size_bytes, target->label);
        }
    }

    char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
    bos_ledger_digest_hex(digest_bytes, digest);

    if (err != ESP_OK) {
        /* Degraded cache: the RAM copy keeps serving; the ledger is lost on
         * reboot (persist state then reads "none" until the server pushes
         * again -- a BR pull-from-server refetch path needs a server-side
         * artifact route that does not exist yet; recorded follow-up). */
        ESP_LOGW(TAG,
                 "ledger v%u accepted; cache persistence failed (%s); serving RAM copy, lost on reboot",
                 (unsigned)ledger_version,
                 esp_err_to_name(err));
        char persist_warning_json[256];
        int warning_written = bos_ledger_ingress_last_error_json(persist_warning_json, sizeof(persist_warning_json));
        if (warning_written < 0 || warning_written >= (int)sizeof(persist_warning_json)) {
            snprintf(persist_warning_json, sizeof(persist_warning_json), "{\"present\":false}");
        }
        char json[576];
        int written = snprintf(json,
                               sizeof(json),
                               "{\"ok\":true,\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                               "\"size_bytes\":%u,\"chunk_size\":%u,\"chunk_count\":%u,"
                               "\"persist\":\"failed\",\"serving\":\"ram\",\"persist_warning\":%s}",
                               (unsigned)ledger_version,
                               digest,
                               (unsigned)size_bytes,
                               (unsigned)BOS_LEDGER_CHUNK_SIZE,
                               (unsigned)chunks,
                               persist_warning_json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        if (written < 0 || written >= (int)sizeof(json)) {
            httpd_resp_sendstr(req, "{\"ok\":true,\"persist\":\"failed\",\"serving\":\"ram\",\"persist_warning\":{\"present\":false}}");
        } else {
            httpd_resp_sendstr(req, json);
        }
        push_guard_release();
        return ESP_OK;
    }

    /* Cache write succeeded: flash is the serving source again, drop RAM. */
    ram_ledger_clear();

    char json[320];
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"ledger_version\":%u,\"manifest_digest\":\"%s\","
             "\"size_bytes\":%u,\"chunk_size\":%u,\"chunk_count\":%u,"
             "\"partition\":\"%s\",\"persist\":\"persisted\"}",
             (unsigned)ledger_version,
             digest,
             (unsigned)size_bytes,
             (unsigned)BOS_LEDGER_CHUNK_SIZE,
             (unsigned)chunks,
             target_label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    push_guard_release();
    return ESP_OK;
}
