/**
 * Building OS - bare ESP32-C6 ThreadTorrent-lite test node.
 *
 * This firmware is for a bare XIAO/ESP32-C6 board. It starts only OpenThread,
 * SRP, and a minimal authenticated ThreadTorrent-lite UDP responder so the BR
 * ledger torrent UI can discover and list a real C6 peer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "openthread/dataset.h"
#include "openthread/error.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/srp_client.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/udp.h"
#include "threadtorrent_lite_auth.h"
#include "threadtorrent_lite_wire.h"

static const char *TAG = "bos_c6_tmfs";

#define LED_GPIO 15
#define LED_ON 0
#define LED_OFF 1
#define THREAD_TASK_STACK_BYTES 8192
#define THREAD_TASK_PRIO 5
#define THREAD_NETWORK_NAME "BuildingOS"
#define THREAD_CHANNEL 15
#define THREAD_PANID 0xB051
#define THREAD_ROUTER_SELECTION_JITTER 1
#define TMFS_SERVICE_NAME "_tmfs._udp"
#define TMFS_INSTANCE_PREFIX "bare-c6-"
#define TMFS_HOST_PREFIX "bos-"
#define TMFS_LEDGER_VERSION 0U
#define TMFS_CHUNKS_HAVE 0U
#define TMFS_CHUNKS_TOTAL 0U

static const uint8_t THREAD_NETWORK_KEY[OT_NETWORK_KEY_SIZE] = {
    0xB0, 0x5F, 0x1B, 0xAD, 0xC0, 0xDE, 0xFA, 0xCE,
    0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67,
};

static const uint8_t THREAD_EXT_PAN_ID[OT_EXT_PAN_ID_SIZE] = {
    0xB0, 0x5F, 0x00, 0x00, 0x00, 0x00, 0xC6, 0x01,
};

static const uint8_t THREAD_MESH_LOCAL_PREFIX[OT_MESH_LOCAL_PREFIX_SIZE] = {
    0xFD, 0x0B, 0x05, 0xF0, 0x00, 0x00, 0xC6, 0x01,
};

static const uint8_t TMFS_SWARM_KEY[32] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static otInstance *s_instance;
static otUdpSocket s_tmfs_socket;
static otDeviceRole s_role = OT_DEVICE_ROLE_DISABLED;
static char s_instance_name[32];
static char s_host_name[48];
static char s_node_id[32];
static char s_txt_version[] = "1";
static char s_txt_catalog[] = "ledger";
static char s_txt_caps[] = "p2p,bare-test";
static char s_txt_wire_payload[8];
static char s_txt_role[] = "leech";
static char s_txt_state[] = "waiting";
static char s_txt_ledger_version[12];
static char s_txt_digest[] = "";
static char s_txt_have[12];
static char s_txt_chunks[12];
static otSrpClientService s_tmfs_service;
static otDnsTxtEntry s_tmfs_txt[11];

static void led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, LED_OFF);
}

static void led_set(bool on)
{
    gpio_set_level(LED_GPIO, on ? LED_ON : LED_OFF);
}

static const char *role_name(otDeviceRole role)
{
    switch (role) {
    case OT_DEVICE_ROLE_DISABLED:
        return "disabled";
    case OT_DEVICE_ROLE_DETACHED:
        return "detached";
    case OT_DEVICE_ROLE_CHILD:
        return "child";
    case OT_DEVICE_ROLE_ROUTER:
        return "router";
    case OT_DEVICE_ROLE_LEADER:
        return "leader";
    default:
        return "unknown";
    }
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        switch (s_role) {
        case OT_DEVICE_ROLE_LEADER:
        case OT_DEVICE_ROLE_ROUTER:
        case OT_DEVICE_ROLE_CHILD:
            led_set(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(900));
            break;
        default:
            led_set(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            led_set(false);
            vTaskDelay(pdMS_TO_TICKS(2920));
            break;
        }
    }
}

static void init_labels(void)
{
    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_IEEE802154);
    if (ret != ESP_OK) {
        ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    if (ret == ESP_OK) {
        snprintf(s_node_id,
                 sizeof(s_node_id),
                 "%s%02x%02x%02x%02x%02x%02x",
                 TMFS_INSTANCE_PREFIX,
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    } else {
        snprintf(s_node_id, sizeof(s_node_id), "%stest", TMFS_INSTANCE_PREFIX);
    }
    snprintf(s_instance_name, sizeof(s_instance_name), "%s", s_node_id);
    snprintf(s_host_name, sizeof(s_host_name), "%s%s", TMFS_HOST_PREFIX, s_node_id);
}

static void set_txt(size_t idx, const char *key, const char *value)
{
    s_tmfs_txt[idx] = (otDnsTxtEntry){
        .mKey = key,
        .mValue = (const uint8_t *)value,
        .mValueLength = (uint16_t)strlen(value),
    };
}

static void populate_txt(void)
{
    snprintf(s_txt_wire_payload, sizeof(s_txt_wire_payload), "%u",
             (unsigned)THREADTORRENT_LITE_WIRE_PAYLOAD_DEFAULT);
    snprintf(s_txt_ledger_version, sizeof(s_txt_ledger_version), "%u",
             (unsigned)TMFS_LEDGER_VERSION);
    snprintf(s_txt_have, sizeof(s_txt_have), "%u", (unsigned)TMFS_CHUNKS_HAVE);
    snprintf(s_txt_chunks, sizeof(s_txt_chunks), "%u", (unsigned)TMFS_CHUNKS_TOTAL);

    set_txt(0, "v", s_txt_version);
    set_txt(1, "nid", s_node_id);
    set_txt(2, "cat", s_txt_catalog);
    set_txt(3, "caps", s_txt_caps);
    set_txt(4, "wp", s_txt_wire_payload);
    set_txt(5, "role", s_txt_role);
    set_txt(6, "state", s_txt_state);
    set_txt(7, "lv", s_txt_ledger_version);
    set_txt(8, "ld", s_txt_digest);
    set_txt(9, "have", s_txt_have);
    set_txt(10, "chunks", s_txt_chunks);
}

static otError publish_tmfs_service(void)
{
    init_labels();
    populate_txt();

    otError err = otSrpClientSetHostName(s_instance, s_host_name);
    if (err != OT_ERROR_NONE) {
        return err;
    }
    err = otSrpClientEnableAutoHostAddress(s_instance);
    if (err != OT_ERROR_NONE) {
        return err;
    }

    memset(&s_tmfs_service, 0, sizeof(s_tmfs_service));
    s_tmfs_service.mName = TMFS_SERVICE_NAME;
    s_tmfs_service.mInstanceName = s_instance_name;
    s_tmfs_service.mTxtEntries = s_tmfs_txt;
    s_tmfs_service.mNumTxtEntries = (uint8_t)(sizeof(s_tmfs_txt) / sizeof(s_tmfs_txt[0]));
    s_tmfs_service.mPort = THREADTORRENT_LITE_PORT;

    err = otSrpClientAddService(s_instance, &s_tmfs_service);
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        return err;
    }
    otSrpClientEnableAutoStartMode(s_instance, NULL, NULL);
    ESP_LOGI(TAG,
             "SRP staged: %s.%s nid=%s role=%s state=%s have=%s/%s",
             s_instance_name,
             TMFS_SERVICE_NAME,
             s_node_id,
             s_txt_role,
             s_txt_state,
             s_txt_have,
             s_txt_chunks);
    return OT_ERROR_NONE;
}

static bool encode_have_payload(uint8_t payload[THREADTORRENT_LITE_HAVE_PAYLOAD_LEN])
{
    threadtorrent_lite_have_t have = {
        .artifact_class = THREADTORRENT_LITE_ARTIFACT_LEDGER,
        .state = THREADTORRENT_LITE_STATE_NONE,
        .role = THREADTORRENT_LITE_ROLE_LEECH,
        .ledger_version = TMFS_LEDGER_VERSION,
        .chunk_size = THREADTORRENT_LITE_PIECE_BYTES_DEFAULT,
        .chunks_have = TMFS_CHUNKS_HAVE,
        .chunks_total = TMFS_CHUNKS_TOTAL,
        .bytes_total = 0,
    };
    return threadtorrent_lite_have_encode(payload, THREADTORRENT_LITE_HAVE_PAYLOAD_LEN, &have);
}

static void send_tmfs_response(const otMessageInfo *request_info,
                               const threadtorrent_lite_header_t *request,
                               uint8_t type,
                               const uint8_t *payload,
                               uint16_t payload_len)
{
    uint8_t packet[THREADTORRENT_LITE_MAX_PACKET_LEN] = {0};
    threadtorrent_lite_header_t header = {
        .version = THREADTORRENT_LITE_VERSION,
        .type = type,
        .payload_len = payload_len,
        .session_id = request->session_id,
        .sequence = request->sequence,
        .flags = 0,
    };
    if (!threadtorrent_lite_wire_encode(packet, sizeof(packet), &header, payload)) {
        ESP_LOGW(TAG, "TMFS encode failed for type=%u", (unsigned)type);
        return;
    }
    size_t packet_len = THREADTORRENT_LITE_HEADER_LEN + payload_len;
    if (threadtorrent_lite_auth_sign(packet, packet_len, TMFS_SWARM_KEY, sizeof(TMFS_SWARM_KEY)) != ESP_OK) {
        ESP_LOGW(TAG, "TMFS sign failed for type=%u", (unsigned)type);
        return;
    }

    otMessage *message = otUdpNewMessage(s_instance, NULL);
    if (!message) {
        ESP_LOGW(TAG, "TMFS response allocation failed");
        return;
    }
    otError err = otMessageAppend(message, packet, (uint16_t)packet_len);
    if (err == OT_ERROR_NONE) {
        otMessageInfo info = {0};
        info.mPeerAddr = request_info->mPeerAddr;
        info.mPeerPort = request_info->mPeerPort;
        info.mSockPort = THREADTORRENT_LITE_PORT;
        err = otUdpSend(s_instance, &s_tmfs_socket, message, &info);
    }
    if (err != OT_ERROR_NONE) {
        otMessageFree(message);
        ESP_LOGW(TAG, "TMFS response send failed: %s", otThreadErrorToString(err));
    }
}

static void tmfs_udp_receive(void *context, otMessage *message, const otMessageInfo *message_info)
{
    (void)context;
    if (!message || !message_info) {
        return;
    }
    uint16_t offset = otMessageGetOffset(message);
    uint16_t length = otMessageGetLength(message);
    if (length < offset || length - offset > THREADTORRENT_LITE_MAX_PACKET_LEN) {
        ESP_LOGW(TAG, "TMFS packet has invalid length");
        return;
    }

    uint8_t packet[THREADTORRENT_LITE_MAX_PACKET_LEN] = {0};
    uint16_t packet_len = (uint16_t)(length - offset);
    if (otMessageRead(message, offset, packet, packet_len) != packet_len) {
        ESP_LOGW(TAG, "TMFS packet short read");
        return;
    }

    threadtorrent_lite_header_t header = {0};
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    if (!threadtorrent_lite_wire_decode(packet, packet_len, &header, &payload, &payload_len)) {
        ESP_LOGW(TAG, "TMFS packet decode failed");
        return;
    }
    if (threadtorrent_lite_auth_verify(packet, packet_len, TMFS_SWARM_KEY, sizeof(TMFS_SWARM_KEY)) != ESP_OK) {
        ESP_LOGW(TAG, "TMFS packet auth failed type=%u", (unsigned)header.type);
        return;
    }

    switch (header.type) {
    case THREADTORRENT_LITE_MSG_HELLO: {
        static const uint8_t hello[] = "tmfs:bare-c6";
        send_tmfs_response(message_info, &header, THREADTORRENT_LITE_MSG_HELLO_OK,
                           hello, (uint16_t)(sizeof(hello) - 1U));
        break;
    }
    case THREADTORRENT_LITE_MSG_PING:
        send_tmfs_response(message_info, &header, THREADTORRENT_LITE_MSG_PONG,
                           payload, (uint16_t)payload_len);
        break;
    case THREADTORRENT_LITE_MSG_HAVE: {
        uint8_t have_payload[THREADTORRENT_LITE_HAVE_PAYLOAD_LEN] = {0};
        if (encode_have_payload(have_payload)) {
            send_tmfs_response(message_info, &header, THREADTORRENT_LITE_MSG_HAVE,
                               have_payload, (uint16_t)sizeof(have_payload));
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "TMFS packet type=%u authenticated; no bare-test handler",
                 (unsigned)header.type);
        break;
    }
}

static otError start_tmfs_udp(void)
{
    otError err = otUdpOpen(s_instance, &s_tmfs_socket, tmfs_udp_receive, NULL);
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        return err;
    }
    otSockAddr sock = {0};
    sock.mPort = THREADTORRENT_LITE_PORT;
    err = otUdpBind(s_instance, &s_tmfs_socket, &sock, OT_NETIF_THREAD_HOST);
    if (err == OT_ERROR_ALREADY) {
        err = OT_ERROR_NONE;
    }
    if (err == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "TMFS UDP listening on %u", (unsigned)THREADTORRENT_LITE_PORT);
    }
    return err;
}

static void state_changed(uint32_t flags, void *context)
{
    (void)flags;
    (void)context;
    otDeviceRole role = otThreadGetDeviceRole(s_instance);
    if (role != s_role) {
        s_role = role;
        ESP_LOGI(TAG, "Thread role: %s", role_name(role));
    }
}

static otError set_fixed_dataset(void)
{
    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));

    dataset.mActiveTimestamp.mSeconds = 1;
    dataset.mComponents.mIsActiveTimestampPresent = true;
    dataset.mChannel = THREAD_CHANNEL;
    dataset.mComponents.mIsChannelPresent = true;
    dataset.mPanId = THREAD_PANID;
    dataset.mComponents.mIsPanIdPresent = true;
    memcpy(dataset.mExtendedPanId.m8, THREAD_EXT_PAN_ID, sizeof(THREAD_EXT_PAN_ID));
    dataset.mComponents.mIsExtendedPanIdPresent = true;
    memcpy(dataset.mMeshLocalPrefix.m8, THREAD_MESH_LOCAL_PREFIX, sizeof(THREAD_MESH_LOCAL_PREFIX));
    dataset.mComponents.mIsMeshLocalPrefixPresent = true;
    memcpy(dataset.mNetworkKey.m8, THREAD_NETWORK_KEY, sizeof(THREAD_NETWORK_KEY));
    dataset.mComponents.mIsNetworkKeyPresent = true;
    snprintf(dataset.mNetworkName.m8, sizeof(dataset.mNetworkName.m8), "%s", THREAD_NETWORK_NAME);
    dataset.mComponents.mIsNetworkNamePresent = true;

    return otDatasetSetActive(s_instance, &dataset);
}

static void thread_task(void *arg)
{
    (void)arg;
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 4,
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    esp_openthread_platform_config_t platform_cfg = {
        .radio_config = {
            .radio_mode = RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = HOST_CONNECTION_MODE_NONE,
        },
        .port_config = {
            .storage_partition_name = "nvs",
            .netif_queue_size = 10,
            .task_queue_size = 10,
        },
    };
    ESP_ERROR_CHECK(esp_openthread_init(&platform_cfg));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&platform_cfg)));

    s_instance = esp_openthread_get_instance();
    ESP_ERROR_CHECK(s_instance ? ESP_OK : ESP_ERR_INVALID_STATE);

    esp_openthread_lock_acquire(portMAX_DELAY);
    otError err = otInstanceErasePersistentInfo(s_instance);
    if (err == OT_ERROR_NONE) {
        err = otSetStateChangedCallback(s_instance, state_changed, NULL);
    }
    if (err == OT_ERROR_NONE) {
        err = set_fixed_dataset();
    }
    otThreadSetRouterSelectionJitter(s_instance, THREAD_ROUTER_SELECTION_JITTER);
    if (err == OT_ERROR_NONE) {
        err = publish_tmfs_service();
    }
    if (err == OT_ERROR_NONE) {
        err = start_tmfs_udp();
    }
    if (err == OT_ERROR_NONE) {
        err = otIp6SetEnabled(s_instance, true);
    }
    if (err == OT_ERROR_NONE) {
        err = otThreadSetEnabled(s_instance, true);
    }
    state_changed(0, NULL);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "OpenThread bare TMFS start failed: %s", otThreadErrorToString(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG,
             "Bare C6 TMFS test started: network=%s channel=%u panid=0x%04x node=%s",
             THREAD_NETWORK_NAME,
             (unsigned)THREAD_CHANNEL,
             (unsigned)THREAD_PANID,
             s_node_id);
    esp_openthread_launch_mainloop();
    ESP_LOGE(TAG, "OpenThread mainloop exited");
    vTaskDelete(NULL);
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Building OS bare ESP32-C6 TMFS test firmware");
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_ret = esp_event_loop_create_default();
    if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_ret);
    }

    led_init();
    xTaskCreate(led_task, "led", 2048, NULL, 4, NULL);
    BaseType_t created = xTaskCreate(thread_task,
                                     "thread",
                                     THREAD_TASK_STACK_BYTES,
                                     NULL,
                                     THREAD_TASK_PRIO,
                                     NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "thread task create failed");
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "runtime: role=%s node=%s service=%s:%u",
                 role_name(s_role),
                 s_node_id[0] ? s_node_id : "-",
                 TMFS_SERVICE_NAME,
                 (unsigned)THREADTORRENT_LITE_PORT);
    }
}
