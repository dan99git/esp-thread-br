/**
 * Building OS Border Router: heartbeat RAM ring buffer and flush.
 * Split from bos_server_registration.c; behavior unchanged.
 */

#include "bos_server_registration.h"
#include "bos_server_reg_internal.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Heartbeat buffer (docs/08.8-border-router.md s12): while the site server
 * is unreachable, heartbeats are buffered in RAM, capped at
 * BOS_REG_HEARTBEAT_BUFFER_CAP entries; on overflow the oldest entry is
 * dropped and counted. The ring is flushed oldest-first on the next
 * successful server contact, before the live heartbeat. RAM-only by design
 * (the docs say "in memory"); lost on reboot, which keeps the boot-relative
 * recorded_uptime_ms timestamps coherent. Entries are only mutated on the
 * registration task; the spinlock guards the count/drop counters read by
 * the diagnostics handler on the httpd task. */
typedef struct {
    uint64_t recorded_uptime_ms;
    bool online;
} bos_heartbeat_entry_t;

static portMUX_TYPE s_hb_mux = portMUX_INITIALIZER_UNLOCKED;
static bos_heartbeat_entry_t s_hb_buffer[BOS_REG_HEARTBEAT_BUFFER_CAP];
static size_t s_hb_head;     /* index of oldest entry */
static size_t s_hb_count;
static uint32_t s_hb_dropped;

/* CONFIG_NEWLIB_NANO_FORMAT has no 64-bit printf support; %llu misaligns the
 * variadic args (LoadProhibited panic). Format u64 manually instead (same
 * helper as bos_diagnostics_server.c). */
void bos_reg_u64_to_dec(uint64_t value, char out[21])
{
    char tmp[21];
    size_t i = 0;
    do {
        tmp[i++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    } while (value != 0 && i < sizeof(tmp) - 1);
    size_t n = 0;
    while (i > 0) {
        out[n++] = tmp[--i];
    }
    out[n] = '\0';
}

/* Appends the heartbeat that just failed to send to the RAM ring. On
 * overflow the oldest entry is dropped and counted. */
void buffer_heartbeat(void)
{
    bos_heartbeat_entry_t entry = {
        .recorded_uptime_ms = (uint64_t)(esp_timer_get_time() / 1000ULL),
        .online = true,
    };

    taskENTER_CRITICAL(&s_hb_mux);
    if (s_hb_count == BOS_REG_HEARTBEAT_BUFFER_CAP) {
        s_hb_head = (s_hb_head + 1U) % BOS_REG_HEARTBEAT_BUFFER_CAP;
        s_hb_count--;
        s_hb_dropped++;
    }
    s_hb_buffer[(s_hb_head + s_hb_count) % BOS_REG_HEARTBEAT_BUFFER_CAP] = entry;
    s_hb_count++;
    taskEXIT_CRITICAL(&s_hb_mux);
}

/* Replays one buffered heartbeat: the fields the live heartbeat sends plus
 * the boot-relative capture timestamp and its age at flush time, so the
 * server can place the missed heartbeat in wall-clock time from its own
 * receive timestamp. The status route reads only "online"; extra fields
 * are ignored. */
static esp_err_t send_buffered_heartbeat(const bos_heartbeat_entry_t *entry)
{
    char path[96];
    char body[160];
    char recorded_str[21];
    char age_str[21];
    bos_http_response_t response;

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    bos_reg_u64_to_dec(entry->recorded_uptime_ms, recorded_str);
    bos_reg_u64_to_dec(now_ms > entry->recorded_uptime_ms ? now_ms - entry->recorded_uptime_ms : 0ULL, age_str);

    int written = snprintf(path, sizeof(path), "/api/devices/%s/status", bos_reg_device_id);
    if (written < 0 || written >= (int)sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(body,
                       sizeof(body),
                       "{\"online\":%s,\"buffered\":true,\"recorded_uptime_ms\":%s,\"age_ms\":%s}",
                       entry->online ? "true" : "false",
                       recorded_str,
                       age_str);
    if (written < 0 || written >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return perform_json_request("PUT", path, body, true, &response);
}

/* Drains the ring oldest-first. Stops at the first send failure, leaving
 * the unsent tail (plus anything newer) buffered. ESP_OK when the ring is
 * empty. Entries are popped only after a successful send. */
esp_err_t flush_heartbeat_buffer(void)
{
    while (true) {
        bos_heartbeat_entry_t entry = {0};
        size_t count_before;

        taskENTER_CRITICAL(&s_hb_mux);
        count_before = s_hb_count;
        if (count_before > 0) {
            entry = s_hb_buffer[s_hb_head];
        }
        taskEXIT_CRITICAL(&s_hb_mux);

        if (count_before == 0) {
            return ESP_OK;
        }

        esp_err_t err = send_buffered_heartbeat(&entry);
        if (err != ESP_OK) {
            return err;
        }

        taskENTER_CRITICAL(&s_hb_mux);
        if (s_hb_count > 0) {
            s_hb_head = (s_hb_head + 1U) % BOS_REG_HEARTBEAT_BUFFER_CAP;
            s_hb_count--;
        }
        taskEXIT_CRITICAL(&s_hb_mux);
    }
}

void bos_server_registration_heartbeat_stats(uint32_t *buffered_count, uint32_t *dropped_count)
{
    taskENTER_CRITICAL(&s_hb_mux);
    uint32_t buffered = (uint32_t)s_hb_count;
    uint32_t dropped = s_hb_dropped;
    taskEXIT_CRITICAL(&s_hb_mux);

    if (buffered_count != NULL) {
        *buffered_count = buffered;
    }
    if (dropped_count != NULL) {
        *dropped_count = dropped;
    }
}
