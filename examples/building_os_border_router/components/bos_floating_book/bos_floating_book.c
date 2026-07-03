/**
 * Building OS Border Router: floating book (see include/bos_floating_book.h).
 */

#include "bos_floating_book.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bos_time.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bos_float";

/* Field widths. Row worst case (values + 7 commas + newline):
 * 16+63+47+32+48+32+42+45+8 = 333 bytes serialized; 16 rows ~= 5.4 KB. */
#define FLOAT_EUI64_LEN 17
#define FLOAT_DEVICE_ID_LEN 64
#define FLOAT_CLASS_LEN 48
#define FLOAT_MAKE_LEN 33
#define FLOAT_MODEL_LEN 49
#define FLOAT_FW_LEN 33
#define FLOAT_MODEL_ID_LEN 80
#define FLOAT_SKU_LEN 24
#define FLOAT_RPW_LEN 16
#define FLOAT_RL_LEN 12
#define FLOAT_CCT_LEN 8
#define FLOAT_DISCOVERED_LEN 43 /* ISO-8601 (20) + "|unsynced" (9) + slack */
#define FLOAT_EID_LEN 46

typedef struct {
    char eui64[FLOAT_EUI64_LEN];
    char device_id[FLOAT_DEVICE_ID_LEN];
    char device_class[FLOAT_CLASS_LEN];
    char make[FLOAT_MAKE_LEN];
    char model[FLOAT_MODEL_LEN];
    char model_id[FLOAT_MODEL_ID_LEN];
    char sku[FLOAT_SKU_LEN];
    char fw[FLOAT_FW_LEN];
    char rated_power_w[FLOAT_RPW_LEN];
    char rated_lumens[FLOAT_RL_LEN];
    char cct[FLOAT_CCT_LEN];
    char discovered_at[FLOAT_DISCOVERED_LEN];
    char eid[FLOAT_EID_LEN];
} floating_row_t;

/* Ring is index-ordered oldest (0) -> newest (count-1). */
static SemaphoreHandle_t s_lock;
static floating_row_t s_rows[BOS_FLOATING_MAX_ROWS];
static size_t s_row_count;
static bool s_dirty;

/* v3 column order (phonebook v3, 17 columns, exact order). Byte-identical to
 * the operational-book v3 column header and the gateway serializer. */
#define FLOATING_COLUMNS_V3 \
    "eui64,device_id,device_class,make,model,model_id,sku,fw,rated_power_w,rated_lumens,cct,discovered_at,eid,x,y,z,spatial_id,tags"

/* Copy src into dst dropping CSV-hostile characters (spec: commas prohibited
 * in every field; newlines would break row framing). Truncates to fit. */
static void copy_sanitized(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0U) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }
    size_t out = 0;
    for (const char *p = src; *p != '\0' && out < dst_len - 1U; p++) {
        if (*p == ',' || *p == '\n' || *p == '\r') {
            continue;
        }
        dst[out++] = *p;
    }
    dst[out] = '\0';
}

static bool valid_eui64(const char *value)
{
    if (!value || strlen(value) != 16U) {
        return false;
    }
    for (const char *p = value; *p != '\0'; p++) {
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void lower_copy(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    for (; src[i] != '\0' && i < dst_len - 1U; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

/* mid carries "make::model" (spec columns 4-5). No "::" => make blank,
 * the whole mid becomes model. */
static void split_mid(const char *mid, char *make, size_t make_len, char *model, size_t model_len)
{
    make[0] = '\0';
    model[0] = '\0';
    if (!mid || mid[0] == '\0') {
        return;
    }
    const char *sep = strstr(mid, "::");
    if (!sep) {
        copy_sanitized(model, model_len, mid);
        return;
    }
    size_t make_src_len = (size_t)(sep - mid);
    char make_raw[FLOAT_MAKE_LEN];
    size_t n = make_src_len < sizeof(make_raw) - 1U ? make_src_len : sizeof(make_raw) - 1U;
    memcpy(make_raw, mid, n);
    make_raw[n] = '\0';
    copy_sanitized(make, make_len, make_raw);
    copy_sanitized(model, model_len, sep + 2);
}

static void stamp_discovered_at(char *out, size_t out_len)
{
    char iso[BOS_TIME_ISO8601_LEN];
    if (bos_time_now_iso8601(iso, sizeof(iso)) < 0) {
        snprintf(out, out_len, "unknown|unsynced");
        return;
    }
    snprintf(out, out_len, "%s|%s", iso, bos_time_is_synced() ? "synced" : "unsynced");
}

/* Overwrites dst with src when src is non-empty and different; reports
 * whether anything changed (drives the NVS dirty flag). */
static bool update_field(char *dst, size_t dst_len, const char *src_sanitized)
{
    if (!src_sanitized || src_sanitized[0] == '\0') {
        return false;
    }
    if (strncmp(dst, src_sanitized, dst_len) == 0) {
        return false;
    }
    snprintf(dst, dst_len, "%s", src_sanitized);
    return true;
}

static int find_row(const char *eui64)
{
    for (size_t i = 0; i < s_row_count; i++) {
        if (strcmp(s_rows[i].eui64, eui64) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Serialize the ring as 13-field CSV lines (internal persistence format;
 * fields are pre-sanitized so a plain comma split reloads them). v3 bumped
 * the field count 8 -> 13 (model_id + sku + rated_power_w/rated_lumens/cct); an
 * old blob with a different field count no longer matches the 13-field split
 * and is rejected cleanly in load_from_nvs rather than mis-parsed. */
static int serialize_rows(char *out, size_t out_len)
{
    size_t pos = 0;
    for (size_t i = 0; i < s_row_count; i++) {
        const floating_row_t *r = &s_rows[i];
        int written = snprintf(out + pos,
                               out_len - pos,
                               "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                               r->eui64,
                               r->device_id,
                               r->device_class,
                               r->make,
                               r->model,
                               r->model_id,
                               r->sku,
                               r->fw,
                               r->rated_power_w,
                               r->rated_lumens,
                               r->cct,
                               r->discovered_at,
                               r->eid);
        if (written < 0 || (size_t)written >= out_len - pos) {
            return -1;
        }
        pos += (size_t)written;
    }
    out[pos] = '\0';
    return (int)pos;
}

static void load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(BOS_FLOATING_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, BOS_FLOATING_NVS_KEY, NULL, &len);
    if (err != ESP_OK || len == 0U || len > BOS_FLOATING_RENDER_MAX) {
        nvs_close(h);
        return;
    }
    char *raw = malloc(len + 1U);
    if (!raw) {
        nvs_close(h);
        return;
    }
    err = nvs_get_blob(h, BOS_FLOATING_NVS_KEY, raw, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(raw);
        return;
    }
    raw[len] = '\0';

    size_t loaded = 0;
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save);
         line != NULL && loaded < BOS_FLOATING_MAX_ROWS;
         line = strtok_r(NULL, "\n", &save)) {
        char *fields[13] = {0};
        size_t count = 1;
        fields[0] = line;
        for (char *p = line; *p != '\0' && count < 13; p++) {
            if (*p == ',') {
                *p = '\0';
                fields[count++] = p + 1;
            }
        }
        if (count != 13 || !valid_eui64(fields[0])) {
            ESP_LOGW(TAG, "skipping malformed persisted floating row");
            continue;
        }
        floating_row_t *r = &s_rows[loaded];
        memset(r, 0, sizeof(*r));
        snprintf(r->eui64, sizeof(r->eui64), "%s", fields[0]);
        snprintf(r->device_id, sizeof(r->device_id), "%s", fields[1]);
        snprintf(r->device_class, sizeof(r->device_class), "%s", fields[2]);
        snprintf(r->make, sizeof(r->make), "%s", fields[3]);
        snprintf(r->model, sizeof(r->model), "%s", fields[4]);
        snprintf(r->model_id, sizeof(r->model_id), "%s", fields[5]);
        snprintf(r->sku, sizeof(r->sku), "%s", fields[6]);
        snprintf(r->fw, sizeof(r->fw), "%s", fields[7]);
        snprintf(r->rated_power_w, sizeof(r->rated_power_w), "%s", fields[8]);
        snprintf(r->rated_lumens, sizeof(r->rated_lumens), "%s", fields[9]);
        snprintf(r->cct, sizeof(r->cct), "%s", fields[10]);
        snprintf(r->discovered_at, sizeof(r->discovered_at), "%s", fields[11]);
        snprintf(r->eid, sizeof(r->eid), "%s", fields[12]);
        loaded++;
    }
    free(raw);
    s_row_count = loaded;
    if (loaded > 0U) {
        ESP_LOGI(TAG, "loaded %u floating-book rows from NVS", (unsigned)loaded);
    }
}

esp_err_t bos_floating_book_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
        load_from_nvs();
    }
    return ESP_OK;
}

esp_err_t bos_floating_book_touch(const bos_floating_touch_t *touch)
{
    if (!touch || !s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    char eui64[FLOAT_EUI64_LEN];
    if (!touch->eui64 || !valid_eui64(touch->eui64)) {
        return ESP_ERR_INVALID_ARG;
    }
    lower_copy(eui64, sizeof(eui64), touch->eui64);

    /* Sanitize outside the lock. */
    char device_id[FLOAT_DEVICE_ID_LEN];
    char device_class[FLOAT_CLASS_LEN];
    char make[FLOAT_MAKE_LEN];
    char model[FLOAT_MODEL_LEN];
    char model_id[FLOAT_MODEL_ID_LEN];
    char sku[FLOAT_SKU_LEN];
    char fw[FLOAT_FW_LEN];
    char eid[FLOAT_EID_LEN];
    char rated_power_w[FLOAT_RPW_LEN];
    char rated_lumens[FLOAT_RL_LEN];
    char cct[FLOAT_CCT_LEN];
    copy_sanitized(device_id, sizeof(device_id), touch->device_id);
    copy_sanitized(device_class, sizeof(device_class), touch->device_class);
    split_mid(touch->model_id, make, sizeof(make), model, sizeof(model));
    copy_sanitized(model_id, sizeof(model_id), touch->model_id);
    copy_sanitized(sku, sizeof(sku), touch->sku);
    copy_sanitized(fw, sizeof(fw), touch->fw);
    copy_sanitized(eid, sizeof(eid), touch->eid);
    copy_sanitized(rated_power_w, sizeof(rated_power_w), touch->rated_power_w);
    copy_sanitized(rated_lumens, sizeof(rated_lumens), touch->rated_lumens);
    copy_sanitized(cct, sizeof(cct), touch->cct);

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    int idx = find_row(eui64);
    if (idx < 0) {
        if (s_row_count == BOS_FLOATING_MAX_ROWS) {
            /* Ring semantics: oldest floating row drops first (spec 2.1). */
            memmove(&s_rows[0], &s_rows[1], sizeof(s_rows[0]) * (BOS_FLOATING_MAX_ROWS - 1U));
            s_row_count--;
        }
        floating_row_t *r = &s_rows[s_row_count++];
        memset(r, 0, sizeof(*r));
        memcpy(r->eui64, eui64, sizeof(r->eui64));
        snprintf(r->device_id, sizeof(r->device_id), "%s", device_id);
        snprintf(r->device_class, sizeof(r->device_class), "%s", device_class);
        snprintf(r->make, sizeof(r->make), "%s", make);
        snprintf(r->model, sizeof(r->model), "%s", model);
        snprintf(r->model_id, sizeof(r->model_id), "%s", model_id);
        snprintf(r->sku, sizeof(r->sku), "%s", sku);
        snprintf(r->fw, sizeof(r->fw), "%s", fw);
        snprintf(r->rated_power_w, sizeof(r->rated_power_w), "%s", rated_power_w);
        snprintf(r->rated_lumens, sizeof(r->rated_lumens), "%s", rated_lumens);
        snprintf(r->cct, sizeof(r->cct), "%s", cct);
        snprintf(r->eid, sizeof(r->eid), "%s", eid);
        stamp_discovered_at(r->discovered_at, sizeof(r->discovered_at));
        s_dirty = true;
        ESP_LOGI(TAG, "floating touch: new row eui64=%s (%u rows)", eui64, (unsigned)s_row_count);
    } else {
        floating_row_t *r = &s_rows[idx];
        bool changed = false;
        changed |= update_field(r->device_id, sizeof(r->device_id), device_id);
        changed |= update_field(r->device_class, sizeof(r->device_class), device_class);
        changed |= update_field(r->make, sizeof(r->make), make);
        changed |= update_field(r->model, sizeof(r->model), model);
        changed |= update_field(r->model_id, sizeof(r->model_id), model_id);
        changed |= update_field(r->sku, sizeof(r->sku), sku);
        changed |= update_field(r->fw, sizeof(r->fw), fw);
        changed |= update_field(r->rated_power_w, sizeof(r->rated_power_w), rated_power_w);
        changed |= update_field(r->rated_lumens, sizeof(r->rated_lumens), rated_lumens);
        changed |= update_field(r->cct, sizeof(r->cct), cct);
        changed |= update_field(r->eid, sizeof(r->eid), eid);
        if (changed) {
            s_dirty = true;
        }
    }

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void bos_floating_book_persist_if_dirty(void)
{
    if (!s_lock) {
        return;
    }

    char *raw = malloc(BOS_FLOATING_RENDER_MAX);
    if (!raw) {
        ESP_LOGW(TAG, "floating persist skipped: out of memory");
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        free(raw);
        return;
    }
    bool dirty = s_dirty;
    int len = -1;
    if (dirty) {
        len = serialize_rows(raw, BOS_FLOATING_RENDER_MAX);
        if (len >= 0) {
            s_dirty = false; /* re-set on write failure below */
        }
    }
    xSemaphoreGive(s_lock);

    if (!dirty) {
        free(raw);
        return;
    }
    if (len < 0) {
        free(raw);
        ESP_LOGE(TAG, "floating persist failed: serialization overflow");
        return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_FLOATING_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, BOS_FLOATING_NVS_KEY, raw, (size_t)len + 1U);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    free(raw);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "floating persist failed: %s", esp_err_to_name(err));
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_dirty = true;
            xSemaphoreGive(s_lock);
        }
    }
}

int bos_floating_book_render_csv(char *out, size_t out_len)
{
    if (!out || out_len == 0U || !s_lock) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return -1;
    }

    char generated_at[FLOAT_DISCOVERED_LEN];
    stamp_discovered_at(generated_at, sizeof(generated_at));

    size_t pos = 0;
    int written = snprintf(out,
                           out_len,
                           "# bos-floating-book rows=%u cap=%u generated_at=%s\n%s\n",
                           (unsigned)s_row_count,
                           (unsigned)BOS_FLOATING_MAX_ROWS,
                           generated_at,
                           FLOATING_COLUMNS_V3);
    if (written < 0 || (size_t)written >= out_len) {
        xSemaphoreGive(s_lock);
        return -1;
    }
    pos = (size_t)written;

    for (size_t i = 0; i < s_row_count; i++) {
        const floating_row_t *r = &s_rows[i];
        /* 18 columns (v3): cols 1-13 populated (eui64..eid, incl. model_id,
         * sku and photometry), cols 14-18 (x,y,z,spatial_id,tags) blank while
         * floating -> the 5 trailing commas. */
        written = snprintf(out + pos,
                           out_len - pos,
                           "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,,,,,\n",
                           r->eui64,
                           r->device_id,
                           r->device_class,
                           r->make,
                           r->model,
                           r->model_id,
                           r->sku,
                           r->fw,
                           r->rated_power_w,
                           r->rated_lumens,
                           r->cct,
                           r->discovered_at,
                           r->eid);
        if (written < 0 || (size_t)written >= out_len - pos) {
            xSemaphoreGive(s_lock);
            return -1;
        }
        pos += (size_t)written;
    }

    xSemaphoreGive(s_lock);
    return (int)pos;
}

size_t bos_floating_book_row_count(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return 0;
    }
    size_t count = s_row_count;
    xSemaphoreGive(s_lock);
    return count;
}
