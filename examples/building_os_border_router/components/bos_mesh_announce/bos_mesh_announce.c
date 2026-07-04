/**
 * Building OS Border Router: mesh-wide artifact-availability announce (nudge).
 */

#include "bos_mesh_announce.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "freertos/FreeRTOS.h"
#include "openthread/coap.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"

static const char *TAG = "bos_announce";

#define BOS_MESH_ANNOUNCE_PATH "mesh/announce"

/* Realm-local all-nodes multicast ff03::1: byte-identical to the destination the
 * device emit path builds (mesh_coap.c fill_realm_local_all_nodes) and to the
 * BR control-event emit. NON multicast is the proven, routable mesh-wide path. */
static void fill_realm_local_all_nodes(otIp6Address *addr)
{
    memset(addr, 0, sizeof(*addr));
    addr->mFields.m8[0] = 0xffU;
    addr->mFields.m8[1] = 0x03U;
    addr->mFields.m8[15] = 0x01U;
}

esp_err_t bos_mesh_announce_fire(const char *artifact_class, uint32_t version, const char *digest_prefix8)
{
    if (!artifact_class || !digest_prefix8) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[128];
    int len = snprintf(body,
                       sizeof(body),
                       "{\"artifact_class\":\"%s\",\"version\":%u,\"digest\":\"%s\"}",
                       artifact_class,
                       (unsigned)version,
                       digest_prefix8);
    if (len < 0 || len >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        ESP_LOGW(TAG, "%s announce skipped: openthread lock timeout", artifact_class);
        return ESP_ERR_TIMEOUT;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (!instance || !otInstanceIsInitialized(instance)) {
        esp_openthread_lock_release();
        ESP_LOGW(TAG, "%s announce skipped: openthread not running", artifact_class);
        return ESP_ERR_INVALID_STATE;
    }

    otMessage *request = otCoapNewMessage(instance, NULL);
    if (!request) {
        esp_openthread_lock_release();
        return ESP_ERR_NO_MEM;
    }

    otCoapMessageInit(request, OT_COAP_TYPE_NON_CONFIRMABLE, OT_COAP_CODE_POST);
    otError err = otCoapMessageAppendUriPathOptions(request, BOS_MESH_ANNOUNCE_PATH);
    if (err == OT_ERROR_NONE) {
        err = otCoapMessageAppendContentFormatOption(request, OT_COAP_OPTION_CONTENT_FORMAT_JSON);
    }
    if (err == OT_ERROR_NONE) {
        err = otCoapMessageSetPayloadMarker(request);
    }
    if (err == OT_ERROR_NONE) {
        err = otMessageAppend(request, body, (uint16_t)len);
    }
    if (err == OT_ERROR_NONE) {
        otMessageInfo info;
        memset(&info, 0, sizeof(info));
        fill_realm_local_all_nodes(&info.mPeerAddr);
        info.mPeerPort = OT_DEFAULT_COAP_PORT;
        /* No response handler: a NON multicast nudge has no single responder. */
        err = otCoapSendRequest(instance, request, &info, NULL, NULL);
    }
    if (err != OT_ERROR_NONE) {
        otMessageFree(request);
        esp_openthread_lock_release();
        ESP_LOGW(TAG, "%s announce send failed: %s", artifact_class, otThreadErrorToString(err));
        return ESP_FAIL;
    }

    esp_openthread_lock_release();
    ESP_LOGI(TAG,
             "mesh announce fired: %s v%u digest=%s -> coap://[ff03::1]/%s",
             artifact_class,
             (unsigned)version,
             digest_prefix8,
             BOS_MESH_ANNOUNCE_PATH);
    return ESP_OK;
}
