/**
 * Building OS Border Router: mesh-side ledger CoAP serving.
 */

#include "bos_ledger_mesh_serve.h"

#include <stdio.h>
#include <string.h>

#include "bos_convergence_aggregator.h"
#include "bos_ledger_ingress.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "openthread/coap.h"
#include "openthread/dns.h"
#include "openthread/error.h"
#include "openthread/message.h"
#include "openthread/srp_client.h"
#include "openthread/thread.h"
#include "sdkconfig.h"

static const char *TAG = "bos_mesh_serve";

#define BOS_LEDGER_COAP_PATH_MANIFEST "mesh/ledger/manifest"
#define BOS_LEDGER_COAP_PATH_CHUNK    "mesh/ledger/chunk"
#define BOS_LEDGER_COAP_PATH_HAVE     "mesh/have"
#define BOS_LEDGER_HAVE_BOOTSTRAP_PEERS_MAX 2U
#define BOS_LEDGER_BLOCK_SZX OT_COAP_OPTION_BLOCK_SZX_256
#define BOS_MESH_SERVICE_NAME "_mesh._udp"
#define BOS_MESH_SRP_TXT_ENTRY_COUNT 7U

static bool s_started;
#if CONFIG_OPENTHREAD_SRP_CLIENT
static bool s_srp_client_ready;
static bool s_srp_service_registered;
static otSrpClientService s_srp_service;
static otDnsTxtEntry s_srp_txt[BOS_MESH_SRP_TXT_ENTRY_COUNT];
static char s_srp_instance_name[32];
static char s_srp_host_name[48];
static char s_txt_role[] = "seed";
static char s_txt_state[] = "active";
static char s_txt_ledger_version[12];
static char s_txt_digest[9];
static char s_txt_have[12];
static char s_txt_chunks[12];
#endif

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

static void digest_prefix_hex(const uint8_t digest_first_4[4], char out[9])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 4U; i++) {
        out[i * 2] = hex[digest_first_4[i] >> 4];
        out[i * 2 + 1] = hex[digest_first_4[i] & 0x0f];
    }
    out[8] = '\0';
}

#if CONFIG_OPENTHREAD_SRP_CLIENT
static void init_srp_labels(void)
{
    if (s_srp_instance_name[0] != '\0') {
        return;
    }

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_ETH);
    if (ret != ESP_OK) {
        ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (ret == ESP_OK) {
        snprintf(s_srp_instance_name,
                 sizeof(s_srp_instance_name),
                 "br-%02x%02x%02x%02x%02x%02x",
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    } else {
        snprintf(s_srp_instance_name, sizeof(s_srp_instance_name), "border-router");
    }
    snprintf(s_srp_host_name, sizeof(s_srp_host_name), "bos-%s", s_srp_instance_name);
}

static void set_txt_entry(size_t index, const char *key, const char *value)
{
    s_srp_txt[index] = (otDnsTxtEntry){
        .mKey = key,
        .mValue = (const uint8_t *)value,
        .mValueLength = (uint16_t)strlen(value),
    };
}

static void populate_srp_txt_entries(void)
{
    set_txt_entry(0, "class", "border_router");
    set_txt_entry(1, "role", s_txt_role);
    set_txt_entry(2, "state", s_txt_state);
    set_txt_entry(3, "lv", s_txt_ledger_version);
    set_txt_entry(4, "ld", s_txt_digest);
    set_txt_entry(5, "have", s_txt_have);
    set_txt_entry(6, "chunks", s_txt_chunks);
}

static esp_err_t ensure_srp_client_locked(otInstance *instance)
{
    if (s_srp_client_ready) {
        return ESP_OK;
    }

    init_srp_labels();
    otError err = otSrpClientSetHostName(instance, s_srp_host_name);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "mesh SRP host name set failed: %s", ot_error_name(err));
        return ESP_FAIL;
    }

    err = otSrpClientEnableAutoHostAddress(instance);
    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "mesh SRP auto host address failed: %s", ot_error_name(err));
        return ESP_FAIL;
    }

    otSrpClientEnableAutoStartMode(instance, NULL, NULL);
    s_srp_client_ready = true;
    return ESP_OK;
}

static esp_err_t publish_mesh_srp_locked(otInstance *instance,
                                         uint32_t version,
                                         const char digest[9],
                                         uint32_t chunk_count)
{
    if (ensure_srp_client_locked(instance) != ESP_OK) {
        return ESP_FAIL;
    }

    if (s_srp_service_registered) {
        otError clear_err = otSrpClientClearService(instance, &s_srp_service);
        if (clear_err != OT_ERROR_NONE && clear_err != OT_ERROR_NOT_FOUND) {
            ESP_LOGW(TAG, "mesh SRP clear failed: %s", ot_error_name(clear_err));
            return ESP_FAIL;
        }
        s_srp_service_registered = false;
    }

    snprintf(s_txt_ledger_version, sizeof(s_txt_ledger_version), "%u", (unsigned)version);
    snprintf(s_txt_digest, sizeof(s_txt_digest), "%s", digest);
    snprintf(s_txt_have, sizeof(s_txt_have), "%u", (unsigned)chunk_count);
    snprintf(s_txt_chunks, sizeof(s_txt_chunks), "%u", (unsigned)chunk_count);
    populate_srp_txt_entries();

    memset(&s_srp_service, 0, sizeof(s_srp_service));
    s_srp_service.mName = BOS_MESH_SERVICE_NAME;
    s_srp_service.mInstanceName = s_srp_instance_name;
    s_srp_service.mTxtEntries = s_srp_txt;
    s_srp_service.mNumTxtEntries = (uint8_t)BOS_MESH_SRP_TXT_ENTRY_COUNT;
    s_srp_service.mPort = OT_DEFAULT_COAP_PORT;

    otError err = otSrpClientAddService(instance, &s_srp_service);
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGW(TAG, "mesh SRP add failed: %s", ot_error_name(err));
        return ESP_FAIL;
    }

    s_srp_service_registered = true;
    ESP_LOGI(TAG,
             "mesh SRP seed published: %s.%s lv=%s ld=%s have=%s/%s",
             s_srp_instance_name,
             BOS_MESH_SERVICE_NAME,
             s_txt_ledger_version,
             s_txt_digest,
             s_txt_have,
             s_txt_chunks);
    return ESP_OK;
}
#endif

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

    bos_ledger_active_t active;
    bool active_ok = bos_ledger_ingress_get_active(&active) == ESP_OK && active.present;

    char json[1152];
    int len;
    if (active_ok) {
        char digest[BOS_LEDGER_DIGEST_LEN * 2 + 1];
        digest_hex(active.digest, digest);
        len = snprintf(json,
                       sizeof(json),
                       "{\"artifact_class\":\"ledger\",\"have\":%u,\"chunk_count\":%u,"
                       "\"target\":{\"ledger_version\":%u,\"manifest_digest\":\"%s\","
                       "\"chunk_size\":%u,\"chunks_total\":%u,\"bytes_total\":%u},"
                       "\"peers\":%s,\"source\":\"br-bootstrap\",\"state\":\"%s\"}",
                       (unsigned)active.chunk_count,
                       (unsigned)active.chunk_count,
                       (unsigned)active.version,
                       digest,
                       (unsigned)active.chunk_size,
                       (unsigned)active.chunk_count,
                       (unsigned)active.size_bytes,
                       peers_json,
                       ledger_state_string(bos_ledger_ingress_state()));
    } else {
        len = snprintf(json,
                       sizeof(json),
                       "{\"artifact_class\":\"ledger\",\"have\":0,\"chunk_count\":0,"
                       "\"target\":{\"ledger_version\":null,\"manifest_digest\":null,"
                       "\"chunk_size\":%u,\"chunks_total\":0,\"bytes_total\":0},"
                       "\"peers\":%s,\"source\":\"br-bootstrap\",\"state\":\"%s\"}",
                       (unsigned)BOS_LEDGER_CHUNK_SIZE,
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

esp_err_t bos_ledger_mesh_serve_publish_active(uint32_t version, const uint8_t *digest_first_4)
{
    if (!digest_first_4) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_OPENTHREAD_SRP_CLIENT
    bos_ledger_active_t active;
    uint32_t chunk_count = 0;
    if (bos_ledger_ingress_get_active(&active) == ESP_OK && active.present && active.version == version) {
        chunk_count = active.chunk_count;
    }

    char digest[9];
    digest_prefix_hex(digest_first_4, digest);

    otInstance *instance = esp_openthread_get_instance();
    if (!instance) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_err_t ret = publish_mesh_srp_locked(instance, version, digest, chunk_count);
    esp_openthread_lock_release();
    return ret;
#else
    ESP_LOGW(TAG, "SRP client disabled; cannot publish mesh seed for ledger version=%u", (unsigned)version);
    return ESP_ERR_NOT_SUPPORTED;
#endif
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
