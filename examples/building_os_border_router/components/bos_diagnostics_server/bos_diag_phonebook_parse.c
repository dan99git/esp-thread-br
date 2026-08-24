/**
 * Building OS Border Router: local diagnostics web surface.
 * Split part: v3 phonebook CSV parsing + push-header validation
 * (see bos_diag_internal.h).
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

static char *phonebook_trim(char *value)
{
    while (*value != '\0' && isspace((unsigned char)*value)) {
        value++;
    }
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return value;
}

static esp_err_t phonebook_copy_field(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0U || !src || strlen(src) >= dst_len) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(dst, src, strlen(src) + 1);
    return ESP_OK;
}

static int phonebook_csv_split(char *line, char **fields, size_t max_fields)
{
    if (!line || !fields || max_fields == 0U) {
        return -1;
    }

    size_t count = 1;
    bool in_quotes = false;
    bool at_field_start = true;
    char *src = line;
    char *dst = line;
    fields[0] = dst;

    while (*src != '\0') {
        char ch = *src++;
        if (in_quotes) {
            if (ch == '"') {
                if (*src == '"') {
                    *dst++ = '"';
                    src++;
                } else {
                    in_quotes = false;
                }
            } else {
                *dst++ = ch;
            }
            continue;
        }

        if (ch == ',') {
            if (count >= max_fields) {
                return -1;
            }
            *dst++ = '\0';
            fields[count++] = dst;
            at_field_start = true;
            continue;
        }
        if (ch == '"' && at_field_start) {
            in_quotes = true;
            at_field_start = false;
            continue;
        }
        *dst++ = ch;
        if (!isspace((unsigned char)ch)) {
            at_field_start = false;
        }
    }
    if (in_quotes) {
        return -1;
    }
    *dst = '\0';

    for (size_t i = 0; i < count; i++) {
        fields[i] = phonebook_trim(fields[i]);
    }
    return (int)count;
}

static bool phonebook_is_ascii_digit_string(const char *value)
{
    if (!value || value[0] == '\0') {
        return false;
    }
    for (const char *p = value; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static bool phonebook_is_sha256_hex(const char *value)
{
    if (!value || strlen(value) != BOS_PHONEBOOK_DIGEST_HEX_LEN) {
        return false;
    }
    for (const char *p = value; *p != '\0'; p++) {
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        if (isalpha((unsigned char)*p) && !islower((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void phonebook_sha256_hex(const char *body, size_t body_len, char out[BOS_PHONEBOOK_DIGEST_HEX_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    if (mbedtls_sha256((const unsigned char *)body, body_len, digest, 0) != 0) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[BOS_PHONEBOOK_DIGEST_HEX_LEN] = '\0';
}

static esp_err_t phonebook_parse_comment_header(const char *line,
                                                bos_phonebook_t *book,
                                                char *error,
                                                size_t error_len)
{
    char version[BOS_PHONEBOOK_VERSION_MAX] = "";
    char digest[BOS_PHONEBOOK_DIGEST_MAX] = "";
    char generated_at[BOS_PHONEBOOK_GENERATED_AT_MAX] = "";
    char extra = '\0';
    /* v3 magic only ("bos-phonebook-v3", 18-col contract). A v2 ("bos-phonebook-v2")
     * or v1 ("bos-phonebook") magic fails this literal sscanf and is rejected
     * here with NO fallback. The version seen is recorded in
     * book->columns_version and cross-checked against the column-header line by
     * the caller. */
    int matched = sscanf(line,
                         "# bos-phonebook-v3 version=%23s digest=%95s generated_at=%47s %c",
                         version,
                         digest,
                         generated_at,
                         &extra);
    if (matched != 3) {
        snprintf(error, error_len, "phonebook: missing or invalid comment header");
        return ESP_ERR_INVALID_ARG;
    }
    book->columns_version = 3;
    if (!phonebook_is_ascii_digit_string(version)) {
        snprintf(error, error_len, "phonebook: invalid version");
        return ESP_ERR_INVALID_ARG;
    }
    if (!phonebook_is_sha256_hex(digest)) {
        snprintf(error, error_len, "phonebook: invalid digest");
        return ESP_ERR_INVALID_ARG;
    }
    if (generated_at[0] == '\0') {
        snprintf(error, error_len, "phonebook: invalid generated_at");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(phonebook_copy_field(book->version, sizeof(book->version), version), TAG, "version too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(book->digest, sizeof(book->digest), digest), TAG, "digest too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(book->generated_at, sizeof(book->generated_at), generated_at), TAG, "generated_at too long");
    return ESP_OK;
}

static esp_err_t phonebook_parse_row(char **fields, int count, bos_phonebook_t *book)
{
    /* v3: eui64,device_id,device_class,make,model,model_id,sku,fw,
     *     rated_power_w,rated_lumens,cct,discovered_at,eid,x,y,z,spatial_id,
     *     tags (18 cols). Keys on eui64 and requires a parseable eid for the
     *     BR resolve + spatial-proxy table. make/model/model_id/sku/photometry
     *     pass through in the stored raw document; not needed by the resolve
     *     table. */
    const int eid_idx = 12; /* eid is column 13 in v3 */
    if (count != 18 || book->row_count >= BOS_PHONEBOOK_MAX_ROWS ||
        fields[0][0] == '\0' || fields[eid_idx][0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    bos_phonebook_row_t row = {0};
    if (!proxy_normalise_eui64(fields[0], strlen(fields[0]), row.eui64)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (otIp6AddressFromString(fields[eid_idx], &row.address) != OT_ERROR_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.device_id, sizeof(row.device_id), fields[1]), TAG, "device_id too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.eid, sizeof(row.eid), fields[eid_idx]), TAG, "eid too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.device_class, sizeof(row.device_class), fields[2]), TAG, "device_class too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.x, sizeof(row.x), fields[13]), TAG, "x too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.y, sizeof(row.y), fields[14]), TAG, "y too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.z, sizeof(row.z), fields[15]), TAG, "z too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.spatial_id, sizeof(row.spatial_id), fields[16]), TAG, "spatial_id too long");
    ESP_RETURN_ON_ERROR(phonebook_copy_field(row.tags, sizeof(row.tags), fields[17]), TAG, "tags too long");

    book->rows[book->row_count++] = row;
    return ESP_OK;
}

static char *phonebook_lf_body(const char *body, char *error, size_t error_len)
{
    size_t body_len = strlen(body);
    char *normal = malloc(body_len + 1U);
    if (!normal) {
        snprintf(error, error_len, "out of memory");
        return NULL;
    }
    size_t out = 0;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == '\r') {
            if (body[i + 1U] == '\n') {
                continue;
            }
            normal[out++] = '\n';
            continue;
        }
        normal[out++] = body[i];
    }
    normal[out] = '\0';
    return normal;
}

esp_err_t phonebook_parse_csv(const char *raw,
                              bos_phonebook_t *out,
                              char computed_digest[BOS_PHONEBOOK_DIGEST_HEX_LEN + 1],
                              char *error,
                              size_t error_len)
{
    if (!raw || !out || !computed_digest || !error || error_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    error[0] = '\0';
    computed_digest[0] = '\0';

    size_t raw_len = strlen(raw);
    if (raw_len == 0U || raw_len >= BOS_PHONEBOOK_BODY_MAX) {
        snprintf(error, error_len, "empty or oversized phonebook");
        return ESP_ERR_INVALID_SIZE;
    }

    const char *newline = strchr(raw, '\n');
    if (!newline) {
        snprintf(error, error_len, "phonebook: missing body");
        return ESP_ERR_INVALID_ARG;
    }

    size_t header_len = (size_t)(newline - raw);
    if (header_len > 0U && raw[header_len - 1U] == '\r') {
        header_len--;
    }
    char header[192];
    if (header_len == 0U || header_len >= sizeof(header)) {
        snprintf(error, error_len, "phonebook: invalid comment header");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(header, raw, header_len);
    header[header_len] = '\0';

    const char *body = newline + 1;
    if (body[0] == '\0') {
        snprintf(error, error_len, "phonebook: missing body");
        return ESP_ERR_INVALID_ARG;
    }

    char *hash_body = phonebook_lf_body(body, error, error_len);
    if (!hash_body) {
        return ESP_ERR_NO_MEM;
    }

    /* Heap-allocated: bos_phonebook_t is ~39 KB and overflows task stacks. */
    bos_phonebook_t *book = calloc(1, sizeof(*book));
    if (!book) {
        snprintf(error, error_len, "out of memory");
        free(hash_body);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t header_err = phonebook_parse_comment_header(header, book, error, error_len);
    if (header_err != ESP_OK) {
        free(hash_body);
        free(book);
        return header_err;
    }

    phonebook_sha256_hex(hash_body, strlen(hash_body), computed_digest);
    if (computed_digest[0] == '\0') {
        snprintf(error, error_len, "phonebook: digest computation failed");
        free(hash_body);
        free(book);
        return ESP_FAIL;
    }
    if (strcmp(computed_digest, book->digest) != 0) {
        snprintf(error, error_len, "phonebook: digest mismatch");
        free(hash_body);
        free(book);
        return ESP_ERR_INVALID_ARG;
    }

    /* v3 only: the column header must be the exact 18-col v3 literal. A v2
     * (14-col) or v1 (9-col) header fails this and is rejected here - the
     * second, independent rejection point after the magic line. */
    size_t columns_len = strlen(BOS_PHONEBOOK_COLUMNS_V3);
    int columns_version = 0;
    if (strncmp(hash_body, BOS_PHONEBOOK_COLUMNS_V3, columns_len) == 0 && hash_body[columns_len] == '\n') {
        columns_version = 3;
    } else {
        snprintf(error, error_len, "phonebook: column header mismatch");
        free(hash_body);
        free(book);
        return ESP_ERR_INVALID_ARG;
    }
    /* The magic recorded from line 1 must agree with the column header:
     * v3 magic requires the 18-col line. */
    if (columns_version != book->columns_version) {
        snprintf(error, error_len, "phonebook: magic/column header version mismatch");
        free(hash_body);
        free(book);
        return ESP_ERR_INVALID_ARG;
    }
    const char *rows_body = hash_body + columns_len + 1U;

    char *copy = malloc(strlen(rows_body) + 1U);
    if (!copy) {
        snprintf(error, error_len, "out of memory");
        free(hash_body);
        free(book);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, rows_body, strlen(rows_body) + 1U);
    free(hash_body);

    size_t line_no = 2;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        line_no++;
        line = phonebook_trim(line);
        if (line[0] == '\0') {
            continue;
        }

        char *fields[BOS_PHONEBOOK_FIELDS_MAX] = {0};
        int count = phonebook_csv_split(line, fields, sizeof(fields) / sizeof(fields[0]));
        if (count < 0) {
            snprintf(error, error_len, "invalid CSV at line %u", (unsigned)line_no);
            free(copy);
            free(book);
            return ESP_ERR_INVALID_ARG;
        }
        if (phonebook_parse_row(fields, count, book) != ESP_OK) {
            snprintf(error, error_len, "invalid phonebook row at line %u", (unsigned)line_no);
            free(copy);
            free(book);
            return ESP_ERR_INVALID_ARG;
        }
    }
    free(copy);

    if (book->row_count == 0U) {
        snprintf(error, error_len, "phonebook has no rows");
        free(book);
        return ESP_ERR_INVALID_ARG;
    }
    book->loaded = true;
    *out = *book;
    free(book);
    return ESP_OK;
}

esp_err_t phonebook_validate_push_headers(httpd_req_t *req,
                                          const bos_phonebook_t *book,
                                          const char *computed_digest,
                                          char *error,
                                          size_t error_len)
{
    char header_version[BOS_PHONEBOOK_VERSION_MAX] = "";
    char header_digest[BOS_PHONEBOOK_DIGEST_MAX] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Phonebook-Version", header_version, sizeof(header_version)) != ESP_OK) {
        snprintf(error, error_len, "missing X-Phonebook-Version");
        return ESP_ERR_INVALID_ARG;
    }
    if (httpd_req_get_hdr_value_str(req, "X-Phonebook-Digest", header_digest, sizeof(header_digest)) != ESP_OK) {
        snprintf(error, error_len, "missing X-Phonebook-Digest");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(header_version, book->version) != 0) {
        snprintf(error, error_len, "X-Phonebook-Version does not match document header");
        return ESP_ERR_INVALID_ARG;
    }
    if (!phonebook_is_sha256_hex(header_digest)) {
        snprintf(error, error_len, "invalid X-Phonebook-Digest");
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(header_digest, book->digest) != 0 || strcmp(header_digest, computed_digest) != 0) {
        snprintf(error, error_len, "X-Phonebook-Digest does not match phonebook body");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
