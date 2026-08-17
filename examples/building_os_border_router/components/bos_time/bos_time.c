/**
 * Building OS Border Router: timekeeping (see include/bos_time.h).
 */

#include "bos_time.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bos_time";

/* Persist the last-known epoch every 15 minutes; on the bench the drift
 * across a reboot is then bounded by one period plus boot time. */
#define BOS_TIME_PERSIST_PERIOD_US (15ULL * 60ULL * 1000000ULL)

/* Anything before 2020-01-01 is not a plausible wall clock on this fleet. */
#define BOS_TIME_MIN_PLAUSIBLE_EPOCH 1577836800LL

typedef enum {
    BOS_TIME_SOURCE_NONE = 0,
    BOS_TIME_SOURCE_NVS_RESTORE,
    BOS_TIME_SOURCE_GATEWAY,
    BOS_TIME_SOURCE_SNTP,
} bos_time_source_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bos_time_source_t s_source = BOS_TIME_SOURCE_NONE;
static bool s_sntp_started;
static esp_timer_handle_t s_persist_timer;

static void set_source(bos_time_source_t source)
{
    taskENTER_CRITICAL(&s_mux);
    if (source > s_source) {
        s_source = source;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static bos_time_source_t get_source(void)
{
    taskENTER_CRITICAL(&s_mux);
    bos_time_source_t source = s_source;
    taskEXIT_CRITICAL(&s_mux);
    return source;
}

static void persist_epoch(void)
{
    time_t now = time(NULL);
    if ((long long)now < BOS_TIME_MIN_PLAUSIBLE_EPOCH) {
        return; /* never persist an implausible clock */
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(BOS_TIME_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "epoch persist open failed: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u64(h, BOS_TIME_NVS_KEY_EPOCH, (uint64_t)now);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "epoch persist failed: %s", esp_err_to_name(err));
    }
}

static void persist_timer_cb(void *arg)
{
    (void)arg;
    persist_epoch();
}

static void restore_epoch_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(BOS_TIME_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint64_t epoch = 0;
    esp_err_t err = nvs_get_u64(h, BOS_TIME_NVS_KEY_EPOCH, &epoch);
    nvs_close(h);
    if (err != ESP_OK || (long long)epoch < BOS_TIME_MIN_PLAUSIBLE_EPOCH) {
        return;
    }

    /* Only restore when the clock is still at boot-epoch; a restore never
     * overrides a real source. */
    if ((long long)time(NULL) >= BOS_TIME_MIN_PLAUSIBLE_EPOCH) {
        return;
    }
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    set_source(BOS_TIME_SOURCE_NVS_RESTORE);
    ESP_LOGI(TAG, "clock restored from NVS (approximate, marked unsynced)");
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    set_source(BOS_TIME_SOURCE_SNTP);
    persist_epoch();
    ESP_LOGI(TAG, "clock synced via SNTP");
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

/* SNTP needs a route + DNS; start it on the first backbone IPv4 rather than
 * before the netif exists. lwIP SNTP then retries internally. */
static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base != IP_EVENT) {
        return;
    }
    if (event_id == IP_EVENT_ETH_GOT_IP || event_id == IP_EVENT_STA_GOT_IP) {
        start_sntp_once();
    }
}

/* days-from-civil (Howard Hinnant's algorithm): UTC struct tm fields to
 * epoch seconds without timegm() (newlib mktime applies TZ). */
static long long civil_to_epoch(int year, int month, int day, int hour, int minute, int second)
{
    long long y = year;
    long long m = month;
    y -= m <= 2 ? 1 : 0;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400LL + hour * 3600LL + minute * 60LL + second;
}

static bool parse_http_date(const char *value, long long *epoch_out)
{
    /* RFC 7231 IMF-fixdate: "Tue, 02 Jul 2026 03:04:05 GMT". */
    static const char *const months[12] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char day_name[4] = "";
    char month_name[4] = "";
    char zone[4] = "";
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;

    int matched = sscanf(value,
                         "%3s, %d %3s %d %d:%d:%d %3s",
                         day_name, &day, month_name, &year,
                         &hour, &minute, &second, zone);
    if (matched != 8 || strcasecmp(zone, "GMT") != 0) {
        return false;
    }
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strcasecmp(month_name, months[i]) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0 || day < 1 || day > 31 || year < 2020 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 60) {
        return false;
    }
    *epoch_out = civil_to_epoch(year, month, day, hour, minute, second);
    return true;
}

void bos_time_note_http_date(const char *http_date)
{
    if (!http_date || http_date[0] == '\0') {
        return;
    }
    if (get_source() == BOS_TIME_SOURCE_SNTP) {
        return; /* SNTP outranks the gateway Date header */
    }

    long long epoch = 0;
    if (!parse_http_date(http_date, &epoch) || epoch < BOS_TIME_MIN_PLAUSIBLE_EPOCH) {
        return;
    }

    long long now = (long long)time(NULL);
    long long delta = epoch > now ? epoch - now : now - epoch;
    bool was_synced = get_source() >= BOS_TIME_SOURCE_GATEWAY;
    if (was_synced && delta <= 5) {
        return; /* already gateway-synced and within tolerance: free-run */
    }

    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    set_source(BOS_TIME_SOURCE_GATEWAY);
    persist_epoch();
    ESP_LOGI(TAG, "clock set from gateway HTTP Date header (delta was %llds)", delta);
}

bool bos_time_is_synced(void)
{
    return get_source() >= BOS_TIME_SOURCE_GATEWAY;
}

const char *bos_time_source(void)
{
    switch (get_source()) {
    case BOS_TIME_SOURCE_SNTP:
        return "sntp";
    case BOS_TIME_SOURCE_GATEWAY:
        return "gateway";
    case BOS_TIME_SOURCE_NVS_RESTORE:
        return "nvs-restore";
    default:
        return "none";
    }
}

int bos_time_now_iso8601(char *out, size_t out_len)
{
    if (!out || out_len < BOS_TIME_ISO8601_LEN) {
        return -1;
    }
    time_t now = time(NULL);
    struct tm utc;
    if (!gmtime_r(&now, &utc)) {
        return -1;
    }
    int written = snprintf(out,
                           out_len,
                           "%04d-%02d-%02dT%02d:%02d:%02dZ",
                           utc.tm_year + 1900,
                           utc.tm_mon + 1,
                           utc.tm_mday,
                           utc.tm_hour,
                           utc.tm_min,
                           utc.tm_sec);
    return (written < 0 || written >= (int)out_len) ? -1 : written;
}

esp_err_t bos_time_init(void)
{
    restore_epoch_from_nvs();

    esp_err_t err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (s_persist_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = persist_timer_cb,
            .name = "bos_time_persist",
        };
        err = esp_timer_create(&args, &s_persist_timer);
        if (err != ESP_OK) {
            return err;
        }
        err = esp_timer_start_periodic(s_persist_timer, BOS_TIME_PERSIST_PERIOD_US);
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG, "timekeeping initialised: source=%s", bos_time_source());
    return ESP_OK;
}
