#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THREADTORRENT_LITE_SERVICE_NAME "_tmfs._udp"
#define THREADTORRENT_LITE_PORT 61616U

#define THREADTORRENT_LITE_VERSION 1U
#define THREADTORRENT_LITE_HEADER_LEN 32U
#define THREADTORRENT_LITE_TAG_OFFSET 16U
#define THREADTORRENT_LITE_TAG_LEN 16U

#define THREADTORRENT_LITE_WIRE_PAYLOAD_SAFE 48U
#define THREADTORRENT_LITE_WIRE_PAYLOAD_DEFAULT 64U
#define THREADTORRENT_LITE_WIRE_PAYLOAD_TEST 96U
#define THREADTORRENT_LITE_PIECE_BYTES_DEFAULT 2048U
#define THREADTORRENT_LITE_CWND_START 4U
#define THREADTORRENT_LITE_CWND_MAX 8U
#define THREADTORRENT_LITE_NODE_INFLIGHT_MAX 16U
#define THREADTORRENT_LITE_ACK_DELAY_MS 40U
#define THREADTORRENT_LITE_RTO_MIN_MS 500U
#define THREADTORRENT_LITE_RTO_MAX_MS 4000U
#define THREADTORRENT_LITE_RETRY_MAX 6U

#define THREADTORRENT_LITE_DIGEST_LEN 32U
#define THREADTORRENT_LITE_HAVE_PAYLOAD_LEN 56U
#define THREADTORRENT_LITE_CATALOG_PAYLOAD_LEN 56U
#define THREADTORRENT_LITE_OPEN_PAYLOAD_LEN 40U
#define THREADTORRENT_LITE_WANT_PAYLOAD_LEN 48U
#define THREADTORRENT_LITE_DATA_HEADER_LEN 48U

#define THREADTORRENT_LITE_MAX_WIRE_PAYLOAD THREADTORRENT_LITE_WIRE_PAYLOAD_TEST
#define THREADTORRENT_LITE_DATA_MAX_BYTES \
    (THREADTORRENT_LITE_MAX_WIRE_PAYLOAD - THREADTORRENT_LITE_DATA_HEADER_LEN)
#define THREADTORRENT_LITE_MAX_PACKET_LEN \
    (THREADTORRENT_LITE_HEADER_LEN + THREADTORRENT_LITE_MAX_WIRE_PAYLOAD)

typedef enum {
    THREADTORRENT_LITE_MSG_HELLO = 1,
    THREADTORRENT_LITE_MSG_HELLO_OK = 2,
    THREADTORRENT_LITE_MSG_CATALOG_REQ = 3,
    THREADTORRENT_LITE_MSG_CATALOG_RESP = 4,
    THREADTORRENT_LITE_MSG_OPEN = 5,
    THREADTORRENT_LITE_MSG_OPEN_OK = 6,
    THREADTORRENT_LITE_MSG_WANT = 7,
    THREADTORRENT_LITE_MSG_DATA = 8,
    THREADTORRENT_LITE_MSG_ACK = 9,
    THREADTORRENT_LITE_MSG_HAVE = 10,
    THREADTORRENT_LITE_MSG_CANCEL = 11,
    THREADTORRENT_LITE_MSG_PING = 12,
    THREADTORRENT_LITE_MSG_PONG = 13,
    THREADTORRENT_LITE_MSG_ERROR = 14,
} threadtorrent_lite_msg_type_t;

typedef enum {
    THREADTORRENT_LITE_ARTIFACT_NONE = 0,
    THREADTORRENT_LITE_ARTIFACT_LEDGER = 1,
    THREADTORRENT_LITE_ARTIFACT_FIRMWARE = 2,
    THREADTORRENT_LITE_ARTIFACT_PACKAGE = 3,
} threadtorrent_lite_artifact_class_t;

typedef enum {
    THREADTORRENT_LITE_ROLE_LEAF = 1,
    THREADTORRENT_LITE_ROLE_SEED = 2,
    THREADTORRENT_LITE_ROLE_LEECH = 3,
} threadtorrent_lite_role_t;

typedef enum {
    THREADTORRENT_LITE_STATE_NONE = 0,
    THREADTORRENT_LITE_STATE_STAGING = 1,
    THREADTORRENT_LITE_STATE_SEEDING = 2,
    THREADTORRENT_LITE_STATE_ACTIVE = 3,
    THREADTORRENT_LITE_STATE_ERROR = 4,
} threadtorrent_lite_transfer_state_t;

typedef struct {
    uint8_t version;
    uint8_t type;
    uint16_t payload_len;
    uint32_t session_id;
    uint32_t sequence;
    uint16_t flags;
    uint8_t tag[THREADTORRENT_LITE_TAG_LEN];
} threadtorrent_lite_header_t;

typedef struct {
    uint8_t artifact_class;
    uint8_t state;
    uint8_t role;
    uint8_t flags;
    uint32_t ledger_version;
    uint32_t chunk_size;
    uint32_t chunks_have;
    uint32_t chunks_total;
    uint32_t bytes_total;
    uint8_t manifest_digest[THREADTORRENT_LITE_DIGEST_LEN];
} threadtorrent_lite_have_t;

typedef struct {
    uint8_t artifact_class;
    uint8_t state;
    uint16_t flags;
    uint32_t ledger_version;
    uint32_t chunk_size;
    uint32_t chunk_count;
    uint32_t bytes_total;
    uint16_t max_data_bytes;
    uint16_t reserved;
    uint8_t manifest_digest[THREADTORRENT_LITE_DIGEST_LEN];
} threadtorrent_lite_catalog_t;

typedef struct {
    uint8_t artifact_class;
    uint8_t flags;
    uint16_t max_data_bytes;
    uint32_t ledger_version;
    uint8_t manifest_digest[THREADTORRENT_LITE_DIGEST_LEN];
} threadtorrent_lite_open_t;

typedef struct {
    uint8_t artifact_class;
    uint8_t flags;
    uint16_t max_bytes;
    uint32_t ledger_version;
    uint32_t chunk_index;
    uint32_t chunk_offset;
    uint8_t manifest_digest[THREADTORRENT_LITE_DIGEST_LEN];
} threadtorrent_lite_want_t;

typedef struct {
    uint8_t artifact_class;
    uint8_t flags;
    uint16_t data_len;
    uint32_t ledger_version;
    uint32_t chunk_index;
    uint32_t chunk_offset;
    uint8_t manifest_digest[THREADTORRENT_LITE_DIGEST_LEN];
    const uint8_t *data;
} threadtorrent_lite_data_t;

bool threadtorrent_lite_wire_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_header_t *header,
                                    const uint8_t *payload);

bool threadtorrent_lite_wire_decode(const uint8_t *packet,
                                    size_t packet_len,
                                    threadtorrent_lite_header_t *header,
                                    const uint8_t **payload,
                                    size_t *payload_len);

void threadtorrent_lite_wire_clear_tag(uint8_t *packet, size_t packet_len);

bool threadtorrent_lite_have_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_have_t *have);

bool threadtorrent_lite_have_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_have_t *out);

bool threadtorrent_lite_catalog_encode(uint8_t *out,
                                       size_t out_len,
                                       const threadtorrent_lite_catalog_t *catalog);

bool threadtorrent_lite_catalog_decode(const uint8_t *payload,
                                       size_t payload_len,
                                       threadtorrent_lite_catalog_t *out);

bool threadtorrent_lite_open_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_open_t *open);

bool threadtorrent_lite_open_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_open_t *out);

bool threadtorrent_lite_want_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_want_t *want);

bool threadtorrent_lite_want_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_want_t *out);

bool threadtorrent_lite_data_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_data_t *data);

bool threadtorrent_lite_data_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_data_t *out);

bool threadtorrent_lite_ack_mark(uint32_t base_sequence,
                                 uint32_t sequence,
                                 uint64_t *bitmap);

bool threadtorrent_lite_ack_has(uint32_t base_sequence,
                                uint64_t bitmap,
                                uint32_t sequence);

#ifdef __cplusplus
}
#endif
