/**
 * Building OS Border Router: mesh-side ledger CoAP serving.
 */

#include "bos_ledger_mesh_serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_convergence_aggregator.h"
#include "bos_ledger_ingress.h"
#include "bos_mesh_announce.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "openthread/coap.h"
#include "openthread/error.h"
#include "openthread/message.h"
#include "openthread/thread.h"
#include "sdkconfig.h"

static const char *TAG = "bos_mesh_serve";

#define BOS_LEDGER_COAP_PATH_MANIFEST "mesh/ledger/manifest"
#define BOS_LEDGER_COAP_PATH_CHUNK    "mesh/ledger/chunk"
#define BOS_LEDGER_COAP_PATH_HAVE     "mesh/have"
#define BOS_LEDGER_HAVE_BOOTSTRAP_PEERS_MAX 2U
#define BOS_LEDGER_BLOCK_SZX OT_COAP_OPTION_BLOCK_SZX_256
#define BOS_PHONEBOOK_DIGEST_HEX_LEN 64

/* Held metadata for the active operational phonebook TARGET reported in
 * GET /mesh/have. The BR is NOT the phonebook byte seed: nodes leech the book
 * over TMFS from a peer (existing PUSH seeds the first nodes; node-to-node
 * gossip spreads it viral). This target only tells a polling node "a newer
 * phonebook version exists" so its pull/gossip wakes. Guarded because the CoAP
 * have handler runs on the OT task while publish runs on the httpd task. */
static SemaphoreHandle_t s_pb_lock;
static bool s_pb_present;
static uint32_t s_pb_version;
static char s_pb_digest[BOS_PHONEBOOK_DIGEST_HEX_LEN + 1];
static uint32_t s_pb_bytes_total;
static uint32_t s_pb_chunk_count;

static void ensure_pb_lock(void)
{
    if (!s_pb_lock) {
        s_pb_lock = xSemaphoreCreateMutex();
    }
}

static bool s_started;

static void handle_manifest(void *context, otMessage *message, const otMessageInfo *message_info);
static void handle_chunk(void *context, otMessage *message, const otMessageInfo *message_info);
static void handle_have(void *context, otMessage *message, const otMessageInfo *message_info);

static otCoapResource s_manifest_resource = {
    .mUriPath = BOS_LEDGER_COAP_PATH_MANIFEST,
    .mHandler = handle_manifest,
};
static otCoapResource s_chunk_resource = {
    .mUriPath = BOS_LEDGER_COAP_PATH_CHUNK,
    .mHandler = handle_chunk,
};
static otCoapResource s_have_resource = {
    .mUriPath = BOS_LEDGER_COAP_PATH_HAVE,
    .mHandler = handle_have,
};

static const char *ot_error_name(otError err)
{
    return otThreadErrorToString(err);
}

static const char *ledger_state_string(bos_ledger_state_t state)
{
    switch (state) {
    case BOS_LEDGER_STATE_NONE:
        return "none";
    case BOS_LEDGER_STATE_RECEIVING:
        return "receiving";
    case BOS_LEDGER_STATE_VALIDATING:
        return "validating";
    case BOS_LEDGER_STATE_COMMITTING:
        return "committing";
    case BOS_LEDGER_STATE_ACTIVE:
        return "active";
    default:
        return "unknown";
    }
}

static void digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < BOS_LEDGER_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[BOS_LEDGER_DIGEST_LEN * 2] = '\0';
}

static size_t block_size_from_szx(uint8_t szx)
{
    if (szx > OT_COAP_OPTION_BLOCK_SZX_1024) {
        szx = BOS_LEDGER_BLOCK_SZX;
    }
    return (size_t)1U << (4U + szx);
}

static bool request_block2(const otMessage *message, uint32_t *block_num, uint8_t *szx)
{
    uint64_t value = 0;
    otCoapOptionIterator iterator;
    if (otCoapOptionIteratorInit(&iterator, message) != OT_ERROR_NONE) {
        return false;
    }
    const otCoapOption *option = otCoapOptionIteratorGetFirstOptionMatching(&iterator, OT_COAP_OPTION_BLOCK2);
    if (!option) {
        *block_num = 0;
        *szx = BOS_LEDGER_BLOCK_SZX;
        return true;
    }
    if (otCoapOptionIteratorGetOptionUintValue(&iterator, &value) != OT_ERROR_NONE) {
        return false;
    }
    if ((value & 0x07U) > BOS_LEDGER_BLOCK_SZX) {
        return false;
    }
    *block_num = (uint32_t)(value >> 4);
    *szx = (uint8_t)(value & 0x07U);
    return true;
}

static otCoapCode esp_to_coap_error(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:
        return OT_COAP_CODE_NOT_FOUND;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return OT_COAP_CODE_BAD_REQUEST;
    case ESP_ERR_NO_MEM:
    case ESP_ERR_INVALID_STATE:
        return OT_COAP_CODE_SERVICE_UNAVAILABLE;
    default:
        return OT_COAP_CODE_INTERNAL_ERROR;
    }
}

static otError send_response_payload(otMessage *request,
                                     const otMessageInfo *message_info,
                                     otCoapCode code,
                                     otCoapOptionContentFormat content_format,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     bool block_response,
                                     uint32_t block_num,
                                     uint8_t block_szx,
                                     bool more)
{
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        return OT_ERROR_INVALID_STATE;
    }

    otMessage *response = otCoapNewMessage(instance, NULL);
    if (!response) {
        return OT_ERROR_NO_BUFS;
    }

    otError err = otCoapMessageInitResponse(response, request, OT_COAP_TYPE_ACKNOWLEDGMENT, code);
    if (err == OT_ERROR_NONE && payload_len > 0U) {
        err = otCoapMessageAppendContentFormatOption(response, content_format);
    }
    if (err == OT_ERROR_NONE && block_response) {
        err = otCoapMessageAppendBlock2Option(response, block_num, more, (otCoapBlockSzx)block_szx);
    }
    if (err == OT_ERROR_NONE && payload_len > 0U) {
        err = otCoapMessageSetPayloadMarker(response);
    }
    if (err == OT_ERROR_NONE && payload_len > 0U) {
        err = otMessageAppend(response, payload, (uint16_t)payload_len);
    }
    if (err == OT_ERROR_NONE) {
        err = otCoapSendResponse(instance, response, message_info);
    }
    if (err != OT_ERROR_NONE) {
        otMessageFree(response);
    }
    return err;
}

static void send_simple_error(otMessage *message, const otMessageInfo *message_info, otCoapCode code)
{
    otError err = send_response_payload(message,
                                        message_info,
                                        code,
                                        OT_COAP_OPTION_CONTENT_FORMAT_TEXT_PLAIN,
                                        NULL,
                                        0,
                                        false,
                                        0,
                                        BOS_LEDGER_BLOCK_SZX,
                                        false);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "CoAP error response failed: %s", ot_error_name(err));
    }
}

static void handle_manifest(void *context, otMessage *message, const otMessageInfo *message_info)
{
    (void)context;
    if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET) {
        send_simple_error(message, message_info, OT_COAP_CODE_METHOD_NOT_ALLOWED);
        return;
    }

    bos_ledger_active_t active;
    esp_err_t ret = bos_ledger_ingress_get_active(&active);
    if (ret != ESP_OK || !active.present) {
        send_simple_error(message, message_info, OT_COAP_CODE_NOT_FOUND);
        return;
    }

    char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
    digest_hex(active.digest, digest);
    char json[256];
    int len = snprintf(json,
                       sizeof(json),
                       "{\"artifact_class\":\"ledger\",\"ledger_version\":%u,\"size\":%u,"
                       "\"chunk_size\":%u,\"chunk_count\":%u,\"manifest_digest\":\"%s\"}",
                       (unsigned)active.version,
                       (unsigned)active.size_bytes,
                       (unsigned)active.chunk_size,
                       (unsigned)active.chunk_count,
                       digest);
    if (len < 0 || len >= (int)sizeof(json)) {
        send_simple_error(message, message_info, OT_COAP_CODE_INTERNAL_ERROR);
        return;
    }

    otError err = send_response_payload(message,
                                        message_info,
                                        OT_COAP_CODE_CONTENT,
                                        OT_COAP_OPTION_CONTENT_FORMAT_JSON,
                                        (const uint8_t *)json,
                                        (size_t)len,
                                        false,
                                        0,
                                        BOS_LEDGER_BLOCK_SZX,
                                        false);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "ledger manifest response failed: %s", ot_error_name(err));
    }
}

static void handle_chunk(void *context, otMessage *message, const otMessageInfo *message_info)
{
    (void)context;
    if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET) {
        send_simple_error(message, message_info, OT_COAP_CODE_METHOD_NOT_ALLOWED);
        return;
    }

    uint32_t block_num = 0;
    uint8_t szx = BOS_LEDGER_BLOCK_SZX;
    if (!request_block2(message, &block_num, &szx)) {
        send_simple_error(message, message_info, OT_COAP_CODE_BAD_REQUEST);
        return;
    }

    size_t requested_size = block_size_from_szx(szx);
    size_t offset = (size_t)block_num * requested_size;
    uint8_t buf[BOS_LEDGER_CHUNK_SIZE];
    if (requested_size > sizeof(buf)) {
        send_simple_error(message, message_info, OT_COAP_CODE_BAD_REQUEST);
        return;
    }

    bos_ledger_active_t active;
    esp_err_t ret = bos_ledger_ingress_get_active(&active);
    if (ret != ESP_OK || !active.present || offset >= active.size_bytes) {
        send_simple_error(message, message_info, OT_COAP_CODE_NOT_FOUND);
        return;
    }

    size_t read_len = 0;
    ret = bos_ledger_ingress_read_active(offset, buf, requested_size, &read_len);
    if (ret != ESP_OK) {
        send_simple_error(message, message_info, esp_to_coap_error(ret));
        return;
    }

    bool more = (offset + read_len) < active.size_bytes;
    otError err = send_response_payload(message,
                                        message_info,
                                        OT_COAP_CODE_CONTENT,
                                        OT_COAP_OPTION_CONTENT_FORMAT_OCTET_STREAM,
                                        buf,
                                        read_len,
                                        true,
                                        block_num,
                                        szx,
                                        more);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "ledger chunk response failed: %s", ot_error_name(err));
    }
}

static void handle_have(void *context, otMessage *message, const otMessageInfo *message_info)
{
    (void)context;
    if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET) {
        send_simple_error(message, message_info, OT_COAP_CODE_METHOD_NOT_ALLOWED);
        return;
    }

    // Compact bootstrap response: target metadata plus up to
    // BOS_LEDGER_HAVE_BOOTSTRAP_PEERS_MAX TMFS device peers so a stale device
    // can fetch the ledger from a device instead of the BR. The full torrent
    // JSON does not fit the Thread IPv6 MTU once peer rows exist.
    char peers_json[640];
    int peers_len = bos_convergence_aggregator_bootstrap_peers_json(peers_json,
                                                                    sizeof(peers_json),
                                                                    BOS_LEDGER_HAVE_BOOTSTRAP_PEERS_MAX);
    if (peers_len < 0) {
        ESP_LOGW(TAG, "bootstrap peer snapshot failed; /mesh/have responds without peers");
        snprintf(peers_json, sizeof(peers_json), "[]");
    }

    // Phonebook availability target, sibling of the ledger target. A polling
    // node reads this to learn a newer BR phonebook version exists, then wakes
    // its own pull/gossip and leeches the book over TMFS from a peer. The BR
    // does not serve the phonebook bytes here (no BR phonebook chunk/peer list
    // today - see report), so no phonebook peers[] is emitted.
    char pb_block[224];
    ensure_pb_lock();
    if (s_pb_lock && xSemaphoreTake(s_pb_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_pb_present) {
            snprintf(pb_block,
                     sizeof(pb_block),
                     "\"phonebook\":{\"artifact_class\":\"phonebook\",\"version\":%u,"
                     "\"digest\":\"%s\",\"chunk_size\":%u,\"chunk_count\":%u,"
                     "\"bytes_total\":%u}",
                     (unsigned)s_pb_version,
                     s_pb_digest,
                     (unsigned)BOS_MESH_CHUNK_SIZE,
                     (unsigned)s_pb_chunk_count,
                     (unsigned)s_pb_bytes_total);
        } else {
            snprintf(pb_block, sizeof(pb_block), "\"phonebook\":null");
        }
        xSemaphoreGive(s_pb_lock);
    } else {
        snprintf(pb_block, sizeof(pb_block), "\"phonebook\":null");
    }

    bos_ledger_active_t active;
    bool active_ok = bos_ledger_ingress_get_active(&active) == ESP_OK && active.present;

    char json[1408];
    int len;
    if (active_ok) {
        char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
        digest_hex(active.digest, digest);
        len = snprintf(json,
                       sizeof(json),
                       "{\"artifact_class\":\"ledger\",\"have\":%u,\"chunk_count\":%u,"
                       "\"target\":{\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                       "\"chunk_size\":%u,\"chunks_total\":%u,\"bytes_total\":%u},"
                       "%s,\"peers\":%s,\"source\":\"br-bootstrap\",\"state\":\"%s\"}",
                       (unsigned)active.chunk_count,
                       (unsigned)active.chunk_count,
                       (unsigned)active.version,
                       digest,
                       (unsigned)active.chunk_size,
                       (unsigned)active.chunk_count,
                       (unsigned)active.size_bytes,
                       pb_block,
                       peers_json,
                       ledger_state_string(bos_ledger_ingress_state()));
    } else {
        len = snprintf(json,
                       sizeof(json),
                       "{\"artifact_class\":\"ledger\",\"have\":0,\"chunk_count\":0,"
                       "\"target\":{\"ledger_version\":null,\"manifest_digest\":null,"
                       "\"chunk_size\":%u,\"chunks_total\":0,\"bytes_total\":0},"
                       "%s,\"peers\":%s,\"source\":\"br-bootstrap\",\"state\":\"%s\"}",
                       (unsigned)BOS_LEDGER_CHUNK_SIZE,
                       pb_block,
                       peers_json,
                       ledger_state_string(bos_ledger_ingress_state()));
    }
    if (len < 0 || len >= (int)sizeof(json)) {
        send_simple_error(message, message_info, OT_COAP_CODE_INTERNAL_ERROR);
        return;
    }

    otError err = send_response_payload(message,
                                        message_info,
                                        OT_COAP_CODE_CONTENT,
                                        OT_COAP_OPTION_CONTENT_FORMAT_JSON,
                                        (const uint8_t *)json,
                                        (size_t)len,
                                        false,
                                        0,
                                        BOS_LEDGER_BLOCK_SZX,
                                        false);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "ledger have response failed: %s", ot_error_name(err));
    }
}

esp_err_t bos_ledger_mesh_serve_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }

    otError err = otCoapStart(instance, OT_DEFAULT_COAP_PORT);
    if (err == OT_ERROR_NONE || err == OT_ERROR_ALREADY) {
        otCoapAddResource(instance, &s_manifest_resource);
        otCoapAddResource(instance, &s_chunk_resource);
        otCoapAddResource(instance, &s_have_resource);
    }
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGW(TAG, "otCoapStart failed: %s", ot_error_name(err));
        return ESP_FAIL;
    }

    s_started = true;
    ESP_LOGI(TAG, "ledger CoAP resources registered: /%s, /%s, /%s",
             BOS_LEDGER_COAP_PATH_MANIFEST,
             BOS_LEDGER_COAP_PATH_CHUNK,
             BOS_LEDGER_COAP_PATH_HAVE);

    bos_ledger_active_t active;
    if (bos_ledger_ingress_get_active(&active) == ESP_OK && active.present) {
        esp_err_t publish = bos_ledger_mesh_serve_publish_active(active.version, active.digest);
        if (publish != ESP_OK) {
            ESP_LOGW(TAG, "persisted active ledger SRP publish failed: %s", esp_err_to_name(publish));
        }
    }
    return ESP_OK;
}

esp_err_t bos_ledger_mesh_serve_publish_phonebook(const char *raw,
                                                  size_t len,
                                                  const char *version_str,
                                                  const char *digest_hex64,
                                                  bool announce)
{
    (void)raw; // The BR reports the phonebook TARGET only; it does not seed the
               // bytes (PUSH + node-to-node TMFS gossip carry the document).
    if (!version_str || !digest_hex64 || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t version = (uint32_t)strtoul(version_str, NULL, 10);

    ensure_pb_lock();
    if (s_pb_lock) {
        xSemaphoreTake(s_pb_lock, portMAX_DELAY);
    }
    s_pb_present = true;
    s_pb_version = version;
    snprintf(s_pb_digest, sizeof(s_pb_digest), "%s", digest_hex64);
    s_pb_bytes_total = (uint32_t)len;
    s_pb_chunk_count = (uint32_t)((len + BOS_MESH_CHUNK_SIZE - 1U) / BOS_MESH_CHUNK_SIZE);
    if (s_pb_lock) {
        xSemaphoreGive(s_pb_lock);
    }

    ESP_LOGI(TAG,
             "phonebook mesh target updated: v%u digest=%.8s bytes=%u chunks=%u announce=%d",
             (unsigned)version,
             digest_hex64,
             (unsigned)len,
             (unsigned)s_pb_chunk_count,
             (int)announce);

    if (announce) {
        char prefix[9];
        snprintf(prefix, sizeof(prefix), "%.8s", digest_hex64);
        esp_err_t announced = bos_mesh_announce_fire("phonebook", version, prefix);
        if (announced != ESP_OK) {
            ESP_LOGW(TAG, "phonebook announce nudge failed: %s", esp_err_to_name(announced));
            return announced;
        }
    }
    return ESP_OK;
}

int bos_ledger_mesh_serve_torrent_json(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return -1;
    }

    bos_ledger_active_t active;
    esp_err_t err = bos_ledger_ingress_get_active(&active);
    if (err == ESP_OK && active.present) {
        char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
        digest_hex(active.digest, digest);
        /* The BR is a peer holding a replaceable cache of the server's
         * artifact, not the implicit sole authority: self carries
         * source=cache plus the cache persist state. */
        int written = snprintf(out,
                               out_len,
                               "{\"target\":{\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                               "\"chunk_size\":%u,\"chunks_total\":%u,\"bytes_total\":%u},"
                               "\"self\":{\"id\":\"border-router\",\"role\":\"seed\",\"source\":\"cache\","
                               "\"persist\":\"%s\",\"state\":\"seedable\","
                               "\"ledger_version\":%u,\"manifest_digest\":\"%s\",\"chunks_have\":%u,"
                               "\"chunks_total\":%u,\"progress_pct\":100,\"rate_bps\":0,\"eta_ms\":null,"
                               "\"last_seen_ms\":0,\"last_error\":null},"
                               "\"peers\":[],"
                               "\"counts\":{\"online\":1,\"seeders\":1,\"leechers\":0,\"current\":1,"
                               "\"stale\":0,\"offline\":0,\"failed\":0},"
                               "\"source\":\"br-ledger\",\"state\":\"%s\","
                               "\"note\":\"SRP/TMFS peer torrent rows are joined by bos_convergence_aggregator\"}",
                               (unsigned)active.version,
                               digest,
                               (unsigned)active.chunk_size,
                               (unsigned)active.chunk_count,
                               (unsigned)active.size_bytes,
                               bos_ledger_ingress_persist_state_str(),
                               (unsigned)active.version,
                               digest,
                               (unsigned)active.chunk_count,
                               (unsigned)active.chunk_count,
                               ledger_state_string(bos_ledger_ingress_state()));
        return (written < 0 || (size_t)written >= out_len) ? -1 : written;
    }

    int written = snprintf(out,
                           out_len,
                           "{\"target\":{\"ledger_version\":null,\"manifest_digest\":null,"
                           "\"chunk_size\":%u,\"chunks_total\":0,\"bytes_total\":0},"
                           "\"self\":{\"id\":\"border-router\",\"role\":\"seed\",\"source\":\"cache\","
                           "\"persist\":\"%s\",\"state\":\"waiting\","
                           "\"ledger_version\":null,\"manifest_digest\":null,\"chunks_have\":0,"
                           "\"chunks_total\":0,\"progress_pct\":0,\"rate_bps\":0,\"eta_ms\":null,"
                           "\"last_seen_ms\":0,\"last_error\":null},"
                           "\"peers\":[],"
                           "\"counts\":{\"online\":1,\"seeders\":0,\"leechers\":0,\"current\":0,"
                           "\"stale\":0,\"offline\":0,\"failed\":0},"
                           "\"source\":\"br-ledger\",\"state\":\"%s\","
                           "\"note\":\"No active ledger is held on this BR\"}",
                           (unsigned)BOS_LEDGER_CHUNK_SIZE,
                           bos_ledger_ingress_persist_state_str(),
                           ledger_state_string(bos_ledger_ingress_state()));
    return (written < 0 || (size_t)written >= out_len) ? -1 : written;
}
