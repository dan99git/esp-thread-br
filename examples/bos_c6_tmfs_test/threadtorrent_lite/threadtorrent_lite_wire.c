#include "threadtorrent_lite_wire.h"

#include <string.h>

#define TMFS_MAGIC_0 ((uint8_t)'T')
#define TMFS_MAGIC_1 ((uint8_t)'M')

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)((value >> 8) & 0xffU);
    p[1] = (uint8_t)(value & 0xffU);
}

static void wr32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)((value >> 24) & 0xffU);
    p[1] = (uint8_t)((value >> 16) & 0xffU);
    p[2] = (uint8_t)((value >> 8) & 0xffU);
    p[3] = (uint8_t)(value & 0xffU);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

bool threadtorrent_lite_wire_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_header_t *header,
                                    const uint8_t *payload)
{
    if (!out || !header || header->version != THREADTORRENT_LITE_VERSION) {
        return false;
    }
    if (header->payload_len > THREADTORRENT_LITE_MAX_WIRE_PAYLOAD) {
        return false;
    }
    if (header->payload_len > 0U && !payload) {
        return false;
    }

    size_t packet_len = THREADTORRENT_LITE_HEADER_LEN + header->payload_len;
    if (out_len < packet_len) {
        return false;
    }

    memset(out, 0, packet_len);
    out[0] = TMFS_MAGIC_0;
    out[1] = TMFS_MAGIC_1;
    out[2] = header->version;
    out[3] = header->type;
    wr16(out + 4, header->payload_len);
    wr32(out + 6, header->session_id);
    wr32(out + 10, header->sequence);
    wr16(out + 14, header->flags);
    memcpy(out + THREADTORRENT_LITE_TAG_OFFSET, header->tag, THREADTORRENT_LITE_TAG_LEN);

    if (header->payload_len > 0U) {
        memcpy(out + THREADTORRENT_LITE_HEADER_LEN, payload, header->payload_len);
    }
    return true;
}

bool threadtorrent_lite_wire_decode(const uint8_t *packet,
                                    size_t packet_len,
                                    threadtorrent_lite_header_t *header,
                                    const uint8_t **payload,
                                    size_t *payload_len)
{
    if (!packet || !header || !payload || !payload_len) {
        return false;
    }
    if (packet_len < THREADTORRENT_LITE_HEADER_LEN ||
        packet_len > THREADTORRENT_LITE_MAX_PACKET_LEN) {
        return false;
    }
    if (packet[0] != TMFS_MAGIC_0 || packet[1] != TMFS_MAGIC_1) {
        return false;
    }
    if (packet[2] != THREADTORRENT_LITE_VERSION) {
        return false;
    }

    uint16_t len = rd16(packet + 4);
    if (len > THREADTORRENT_LITE_MAX_WIRE_PAYLOAD ||
        THREADTORRENT_LITE_HEADER_LEN + (size_t)len != packet_len) {
        return false;
    }

    memset(header, 0, sizeof(*header));
    header->version = packet[2];
    header->type = packet[3];
    header->payload_len = len;
    header->session_id = rd32(packet + 6);
    header->sequence = rd32(packet + 10);
    header->flags = rd16(packet + 14);
    memcpy(header->tag, packet + THREADTORRENT_LITE_TAG_OFFSET, THREADTORRENT_LITE_TAG_LEN);
    *payload = packet + THREADTORRENT_LITE_HEADER_LEN;
    *payload_len = len;
    return true;
}

void threadtorrent_lite_wire_clear_tag(uint8_t *packet, size_t packet_len)
{
    if (!packet || packet_len < THREADTORRENT_LITE_HEADER_LEN) {
        return;
    }
    memset(packet + THREADTORRENT_LITE_TAG_OFFSET, 0, THREADTORRENT_LITE_TAG_LEN);
}

bool threadtorrent_lite_have_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_have_t *have)
{
    if (!out || !have || out_len < THREADTORRENT_LITE_HAVE_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, THREADTORRENT_LITE_HAVE_PAYLOAD_LEN);
    out[0] = have->artifact_class;
    out[1] = have->state;
    out[2] = have->role;
    out[3] = have->flags;
    wr32(out + 4, have->ledger_version);
    wr32(out + 8, have->chunk_size);
    wr32(out + 12, have->chunks_have);
    wr32(out + 16, have->chunks_total);
    wr32(out + 20, have->bytes_total);
    memcpy(out + 24, have->manifest_digest, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_have_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_have_t *out)
{
    if (!payload || !out || payload_len != THREADTORRENT_LITE_HAVE_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->artifact_class = payload[0];
    out->state = payload[1];
    out->role = payload[2];
    out->flags = payload[3];
    out->ledger_version = rd32(payload + 4);
    out->chunk_size = rd32(payload + 8);
    out->chunks_have = rd32(payload + 12);
    out->chunks_total = rd32(payload + 16);
    out->bytes_total = rd32(payload + 20);
    memcpy(out->manifest_digest, payload + 24, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_catalog_encode(uint8_t *out,
                                       size_t out_len,
                                       const threadtorrent_lite_catalog_t *catalog)
{
    if (!out || !catalog || out_len < THREADTORRENT_LITE_CATALOG_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, THREADTORRENT_LITE_CATALOG_PAYLOAD_LEN);
    out[0] = catalog->artifact_class;
    out[1] = catalog->state;
    wr16(out + 2, catalog->flags);
    wr32(out + 4, catalog->ledger_version);
    wr32(out + 8, catalog->chunk_size);
    wr32(out + 12, catalog->chunk_count);
    wr32(out + 16, catalog->bytes_total);
    wr16(out + 20, catalog->max_data_bytes);
    wr16(out + 22, catalog->reserved);
    memcpy(out + 24, catalog->manifest_digest, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_catalog_decode(const uint8_t *payload,
                                       size_t payload_len,
                                       threadtorrent_lite_catalog_t *out)
{
    if (!payload || !out || payload_len != THREADTORRENT_LITE_CATALOG_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->artifact_class = payload[0];
    out->state = payload[1];
    out->flags = rd16(payload + 2);
    out->ledger_version = rd32(payload + 4);
    out->chunk_size = rd32(payload + 8);
    out->chunk_count = rd32(payload + 12);
    out->bytes_total = rd32(payload + 16);
    out->max_data_bytes = rd16(payload + 20);
    out->reserved = rd16(payload + 22);
    memcpy(out->manifest_digest, payload + 24, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_open_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_open_t *open)
{
    if (!out || !open || out_len < THREADTORRENT_LITE_OPEN_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, THREADTORRENT_LITE_OPEN_PAYLOAD_LEN);
    out[0] = open->artifact_class;
    out[1] = open->flags;
    wr16(out + 2, open->max_data_bytes);
    wr32(out + 4, open->ledger_version);
    memcpy(out + 8, open->manifest_digest, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_open_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_open_t *out)
{
    if (!payload || !out || payload_len != THREADTORRENT_LITE_OPEN_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->artifact_class = payload[0];
    out->flags = payload[1];
    out->max_data_bytes = rd16(payload + 2);
    out->ledger_version = rd32(payload + 4);
    memcpy(out->manifest_digest, payload + 8, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_want_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_want_t *want)
{
    if (!out || !want || out_len < THREADTORRENT_LITE_WANT_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, THREADTORRENT_LITE_WANT_PAYLOAD_LEN);
    out[0] = want->artifact_class;
    out[1] = want->flags;
    wr16(out + 2, want->max_bytes);
    wr32(out + 4, want->ledger_version);
    wr32(out + 8, want->chunk_index);
    wr32(out + 12, want->chunk_offset);
    memcpy(out + 16, want->manifest_digest, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_want_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_want_t *out)
{
    if (!payload || !out || payload_len != THREADTORRENT_LITE_WANT_PAYLOAD_LEN) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->artifact_class = payload[0];
    out->flags = payload[1];
    out->max_bytes = rd16(payload + 2);
    out->ledger_version = rd32(payload + 4);
    out->chunk_index = rd32(payload + 8);
    out->chunk_offset = rd32(payload + 12);
    memcpy(out->manifest_digest, payload + 16, THREADTORRENT_LITE_DIGEST_LEN);
    return true;
}

bool threadtorrent_lite_data_encode(uint8_t *out,
                                    size_t out_len,
                                    const threadtorrent_lite_data_t *data)
{
    if (!out || !data ||
        data->data_len > THREADTORRENT_LITE_DATA_MAX_BYTES ||
        out_len < THREADTORRENT_LITE_DATA_HEADER_LEN + (size_t)data->data_len ||
        (data->data_len > 0U && !data->data)) {
        return false;
    }

    memset(out, 0, THREADTORRENT_LITE_DATA_HEADER_LEN + (size_t)data->data_len);
    out[0] = data->artifact_class;
    out[1] = data->flags;
    wr16(out + 2, data->data_len);
    wr32(out + 4, data->ledger_version);
    wr32(out + 8, data->chunk_index);
    wr32(out + 12, data->chunk_offset);
    memcpy(out + 16, data->manifest_digest, THREADTORRENT_LITE_DIGEST_LEN);
    if (data->data_len > 0U) {
        memcpy(out + THREADTORRENT_LITE_DATA_HEADER_LEN, data->data, data->data_len);
    }
    return true;
}

bool threadtorrent_lite_data_decode(const uint8_t *payload,
                                    size_t payload_len,
                                    threadtorrent_lite_data_t *out)
{
    if (!payload || !out || payload_len < THREADTORRENT_LITE_DATA_HEADER_LEN) {
        return false;
    }

    uint16_t data_len = rd16(payload + 2);
    if (data_len > THREADTORRENT_LITE_DATA_MAX_BYTES ||
        THREADTORRENT_LITE_DATA_HEADER_LEN + (size_t)data_len != payload_len) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->artifact_class = payload[0];
    out->flags = payload[1];
    out->data_len = data_len;
    out->ledger_version = rd32(payload + 4);
    out->chunk_index = rd32(payload + 8);
    out->chunk_offset = rd32(payload + 12);
    memcpy(out->manifest_digest, payload + 16, THREADTORRENT_LITE_DIGEST_LEN);
    out->data = payload + THREADTORRENT_LITE_DATA_HEADER_LEN;
    return true;
}

bool threadtorrent_lite_ack_mark(uint32_t base_sequence,
                                 uint32_t sequence,
                                 uint64_t *bitmap)
{
    if (!bitmap || sequence < base_sequence) {
        return false;
    }

    uint32_t delta = sequence - base_sequence;
    if (delta >= 64U) {
        return false;
    }
    *bitmap |= (uint64_t)1U << delta;
    return true;
}

bool threadtorrent_lite_ack_has(uint32_t base_sequence,
                                uint64_t bitmap,
                                uint32_t sequence)
{
    if (sequence < base_sequence) {
        return false;
    }

    uint32_t delta = sequence - base_sequence;
    if (delta >= 64U) {
        return false;
    }
    return (bitmap & ((uint64_t)1U << delta)) != 0U;
}
