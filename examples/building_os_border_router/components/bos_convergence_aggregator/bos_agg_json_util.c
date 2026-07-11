/**
 * Building OS Border Router: mesh convergence aggregator JSON/hex append helpers.
 */

#include "bos_agg_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int appendf(char **cursor, size_t *remaining, const char *fmt, ...)
{
    if (!cursor || !*cursor || !remaining || *remaining == 0U || !fmt) {
        return -1;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(*cursor, *remaining, fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= *remaining) {
        return -1;
    }
    *cursor += written;
    *remaining -= (size_t)written;
    return written;
}

int append_raw(char **cursor, size_t *remaining, const char *value)
{
    if (!cursor || !*cursor || !remaining || *remaining == 0U || !value) {
        return -1;
    }

    size_t len = strlen(value);
    if (len >= *remaining) {
        return -1;
    }
    memcpy(*cursor, value, len);
    *cursor += len;
    *remaining -= len;
    **cursor = '\0';
    return (int)len;
}

int append_u64_decimal(char **cursor, size_t *remaining, uint64_t value)
{
    char buf[21];
    size_t pos = sizeof(buf);
    buf[--pos] = '\0';

    do {
        buf[--pos] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value > 0U && pos > 0U);

    return append_raw(cursor, remaining, &buf[pos]);
}

int append_json_string(char **cursor, size_t *remaining, const char *value)
{
    if (appendf(cursor, remaining, "\"") < 0) {
        return -1;
    }
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    while (*p) {
        if (*p == '"' || *p == '\\') {
            if (appendf(cursor, remaining, "\\%c", *p) < 0) {
                return -1;
            }
        } else if (*p >= 0x20U && *p < 0x7fU) {
            if (appendf(cursor, remaining, "%c", *p) < 0) {
                return -1;
            }
        } else {
            if (appendf(cursor, remaining, "\\u%04x", (unsigned)*p) < 0) {
                return -1;
            }
        }
        p++;
    }
    return appendf(cursor, remaining, "\"");
}

void bos_agg_digest_hex(const uint8_t digest[BOS_LEDGER_DIGEST_LEN], char out[BOS_LEDGER_DIGEST_LEN * 2 + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < BOS_LEDGER_DIGEST_LEN; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[BOS_LEDGER_DIGEST_LEN * 2] = '\0';
}

void ip6_to_hex(const uint8_t addr[16], char out[40])
{
    snprintf(out,
             40,
             "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             addr[0],
             addr[1],
             addr[2],
             addr[3],
             addr[4],
             addr[5],
             addr[6],
             addr[7],
             addr[8],
             addr[9],
             addr[10],
             addr[11],
             addr[12],
             addr[13],
             addr[14],
             addr[15]);
}

// Plain 32-hex-char form consumed by the XIAO mesh bootstrap parser
// (hex_to_ip6_address in mesh_coap.c rejects the colon-separated form).
void ip6_to_plain_hex(const uint8_t addr[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16U; i++) {
        out[i * 2] = hex[addr[i] >> 4];
        out[i * 2 + 1] = hex[addr[i] & 0x0f];
    }
    out[32] = '\0';
}
