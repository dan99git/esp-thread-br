/**
 * Building OS Border Router: Ethernet self-diagnosis and self-recovery.
 * See bos_self_heal.h for the rationale.
 */

#include "bos_self_heal.h"

#include <stdio.h>
#include <string.h>

#include "bos_thread_diag.h" /* bos_u64_to_dec (shared, no %llu) */
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol_examples_common.h" /* get_example_eth_handle, get_example_netif */
#include "sdkconfig.h"

static const char *TAG = "bos_self_heal";

/* ---- Tunable thresholds (commented per the safety contract) ---------------
 *
 * The recovery must NEVER disturb a healthy BR. Every threshold below is a
 * grace window: the ladder only fires when Ethernet is genuinely down for
 * longer than the BR could plausibly need to come up on its own.
 */

/* Health assessment cadence. 10 s is frequent enough to heal within a minute
 * but far longer than any transient DHCP hiccup. */
#define BOS_SELF_HEAL_POLL_MS (10 * 1000)

/* No-IP grace window. The W5500 link + DHCP normally completes in a few
 * seconds; 45 s after boot (or after a disconnect) with no IPv4 is the
 * documented "stuck waiting for IP" signature. Below this we do nothing. */
#define BOS_SELF_HEAL_NO_IP_THRESHOLD_US (45ULL * 1000ULL * 1000ULL)

/* L1 W5500 hardware-reset attempts before escalating to L2. Each L1 attempt
 * is followed by a fresh no-IP grace window before the next. */
#define BOS_SELF_HEAL_L1_MAX_ATTEMPTS (3)

/* Settle window granted after an L1 reset before the IP is re-judged. Must
 * exceed link re-negotiation + DHCP; reuses the same 45 s no-IP threshold via
 * s_last_ip_or_action_us, so this constant just documents intent. */

/* L2 self-restart cap: minimum spacing between host restarts so a persistent
 * fault (dead W5500, unplugged cable) cannot hot-loop the BR. */
#define BOS_SELF_HEAL_L2_MIN_INTERVAL_US (2ULL * 60ULL * 1000ULL * 1000ULL)

/* Boot-loop detector: if more than this many self-heal-driven restarts happen
 * inside the window, stop restarting and stay up degraded so the fault is
 * observable on /bos/status instead of an endless reboot. */
#define BOS_SELF_HEAL_L2_BOOTLOOP_MAX (4)
#define BOS_SELF_HEAL_L2_BOOTLOOP_WINDOW_US (30ULL * 60ULL * 1000ULL * 1000ULL)

/* W5500 hardware reset pin. Confirmed docs/08.8 section, and wired as
 * CONFIG_EXAMPLE_ETH_PHY_RST_GPIO so the W5500 PHY driver already owns it as
 * phy_config.reset_gpio_num. We drive the same pin directly for the external
 * recovery pulse. */
#ifdef CONFIG_EXAMPLE_ETH_PHY_RST_GPIO
#define BOS_W5500_RST_GPIO (CONFIG_EXAMPLE_ETH_PHY_RST_GPIO)
#else
#define BOS_W5500_RST_GPIO (40)
#endif

/* W5500 reset timing. Datasheet: RST low pulse min 500 us; internal PLL lock
 * after reset takes up to ~10 ms (the esp_eth W5500 driver waits
 * W5500_WAIT_FOR_RESET_MS = 10 ms, esp_eth_phy_w5500.c:20). We hold low 10 ms
 * (20x the datasheet minimum) and settle 50 ms (5x the PLL-lock wait) so the
 * external pulse is unambiguous even with daughterboard RC on the line. */
#define BOS_W5500_RST_LOW_MS (10)
#define BOS_W5500_RST_SETTLE_MS (50)

/* ---- Recovery-history ring ------------------------------------------------ */

typedef enum {
    BOS_HEAL_NONE = 0,
    BOS_HEAL_L1_W5500_RESET,
    BOS_HEAL_L2_RESTART,
} bos_heal_level_t;

typedef enum {
    BOS_HEAL_OUTCOME_PENDING = 0,
    BOS_HEAL_OUTCOME_RECOVERED,
    BOS_HEAL_OUTCOME_FAILED,
    BOS_HEAL_OUTCOME_CAPPED,
    BOS_HEAL_OUTCOME_TRIGGERED,
} bos_heal_outcome_t;

typedef struct {
    uint64_t uptime_ms;     /* uptime at the time of the event */
    uint32_t trigger_no_ip_ms; /* how long Ethernet had had no IP when it fired */
    bos_heal_level_t level;
    bos_heal_outcome_t outcome;
} bos_heal_event_t;

#define BOS_HEAL_RING_LEN (8)

static bos_heal_event_t s_ring[BOS_HEAL_RING_LEN];
static size_t s_ring_count;   /* total events ever recorded (monotonic) */
static SemaphoreHandle_t s_lock;

/* Backbone health state, updated from the ETH/IP event handlers and the task.
 * Guarded by s_lock. */
static volatile bool s_link_up;
static volatile bool s_has_ipv4;
static uint64_t s_last_ip_us;          /* esp_timer of the most recent IPv4 */
static uint64_t s_last_action_us;      /* esp_timer of the most recent L1/L2 */
static uint32_t s_l1_attempts;         /* L1 attempts in the current down episode */
static uint32_t s_l1_total;            /* lifetime L1 resets (this boot) */
static bool s_started;

/* RTC slow memory survives a software restart (esp_restart) but is cleared on
 * power-on / brownout. Used to cap the L2 restart loop. */
RTC_NOINIT_ATTR static uint32_t s_rtc_l2_count;
RTC_NOINIT_ATTR static uint64_t s_rtc_l2_window_start_us;
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
#define BOS_SELF_HEAL_RTC_MAGIC (0xB05E1FED)

static const char *level_str(bos_heal_level_t level)
{
    switch (level) {
    case BOS_HEAL_L1_W5500_RESET:
        return "l1_w5500_reset";
    case BOS_HEAL_L2_RESTART:
        return "l2_restart";
    default:
        return "none";
    }
}

static const char *outcome_str(bos_heal_outcome_t outcome)
{
    switch (outcome) {
    case BOS_HEAL_OUTCOME_RECOVERED:
        return "recovered";
    case BOS_HEAL_OUTCOME_FAILED:
        return "failed";
    case BOS_HEAL_OUTCOME_CAPPED:
        return "capped";
    case BOS_HEAL_OUTCOME_TRIGGERED:
        return "triggered";
    default:
        return "pending";
    }
}

/* Record one event into the ring. Caller must NOT hold s_lock. Returns the
 * ring slot index so the caller can update the outcome later. */
static size_t ring_record(bos_heal_level_t level, bos_heal_outcome_t outcome, uint64_t no_ip_us)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t idx = s_ring_count % BOS_HEAL_RING_LEN;
    s_ring[idx].uptime_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    s_ring[idx].trigger_no_ip_ms = (uint32_t)(no_ip_us / 1000ULL);
    s_ring[idx].level = level;
    s_ring[idx].outcome = outcome;
    s_ring_count++;
    xSemaphoreGive(s_lock);
    return idx;
}

static void ring_set_outcome(size_t idx, bos_heal_outcome_t outcome)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ring[idx].outcome = outcome;
    xSemaphoreGive(s_lock);
}

/* ---- Event handlers ------------------------------------------------------- */

static bool is_backbone_netif(esp_netif_t *netif)
{
    if (netif == NULL) {
        return false;
    }
    /* get_example_netif() resolves to the ETH netif in this build
     * (CONFIG_EXAMPLE_CONNECT_ETHERNET). */
    return netif == get_example_netif();
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        if (!is_backbone_netif(event->esp_netif)) {
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_has_ipv4 = true;
        s_last_ip_us = (uint64_t)esp_timer_get_time();
        s_l1_attempts = 0; /* healthy again: reset the L1 episode counter */
        xSemaphoreGive(s_lock);
    }
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link_up = true;
    } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
        s_link_up = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_has_ipv4 = false;
        xSemaphoreGive(s_lock);
    }
}

/* ---- Recovery primitives -------------------------------------------------- */

/* L1: hardware-reset the W5500 and re-start the Ethernet driver in place.
 *
 * The eth handle IS obtainable: protocol_examples_common exports
 * get_example_eth_handle() (protocol_examples_common.h:145,
 * eth_connect.c:201). We stop the driver, drive the W5500 RST line low->high
 * with the datasheet timing, then start it again. The netif glue and driver
 * stay installed, so no re-attach is needed; esp_eth_start re-runs PHY init
 * (which itself re-asserts reset_gpio), and the existing IP_EVENT handlers
 * (here and in the diagnostics server) pick the new IP back up.
 *
 * Returns ESP_OK if the start succeeded (an IP is then awaited by the task).
 */
static esp_err_t l1_w5500_reset(void)
{
    esp_eth_handle_t eth = get_example_eth_handle();
    if (eth == NULL) {
        ESP_LOGE(TAG, "L1: eth handle unavailable; cannot W5500-reset, escalating");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGW(TAG, "L1: W5500 RECOVERY - stopping eth, pulsing RST (GPIO%d), restarting",
             BOS_W5500_RST_GPIO);

    /* esp_eth_stop is safe to call even if already stopped; log on error but
     * proceed to the reset pulse regardless (the pulse is the real recovery). */
    esp_err_t err = esp_eth_stop(eth);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "L1: esp_eth_stop returned %s; continuing to reset pulse", esp_err_to_name(err));
    }

    /* Drive the W5500 reset line: configure as output, hold low, settle high.
     * The pin is already a GPIO (owned by the PHY driver as reset_gpio_num);
     * re-asserting output mode here is idempotent. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOS_W5500_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(BOS_W5500_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(BOS_W5500_RST_LOW_MS));
    gpio_set_level(BOS_W5500_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BOS_W5500_RST_SETTLE_MS));

    err = esp_eth_start(eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "L1: esp_eth_start failed after reset: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "L1: W5500 reset complete, eth restarted; awaiting IP");
    return ESP_OK;
}

/* L2: self-restart, boot-loop-capped via RTC slow memory. Records the event,
 * bumps the RTC counter, and either restarts or stays up degraded if the cap
 * is hit. Never returns when it restarts. */
static void l2_restart(uint64_t no_ip_us)
{
    uint64_t now = (uint64_t)esp_timer_get_time();

    /* Reset the boot-loop window if it has elapsed (the window is wall-clock
     * within a continuous power session: RTC counters are cleared on
     * power-on). */
    if (now - s_rtc_l2_window_start_us > BOS_SELF_HEAL_L2_BOOTLOOP_WINDOW_US) {
        s_rtc_l2_window_start_us = now;
        s_rtc_l2_count = 0;
    }

    if (s_rtc_l2_count >= BOS_SELF_HEAL_L2_BOOTLOOP_MAX) {
        /* Boot-loop cap reached: a persistent fault. Stay up degraded so the
         * operator sees the history on /bos/status instead of an endless
         * reboot cycle. */
        ESP_LOGE(TAG,
                 "L2: boot-loop cap reached (%u restarts in window); staying UP degraded, "
                 "Ethernet down - manual intervention required",
                 (unsigned)s_rtc_l2_count);
        ring_record(BOS_HEAL_L2_RESTART, BOS_HEAL_OUTCOME_CAPPED, no_ip_us);
        return;
    }

    s_rtc_l2_count++;
    ring_record(BOS_HEAL_L2_RESTART, BOS_HEAL_OUTCOME_TRIGGERED, no_ip_us);
    ESP_LOGE(TAG,
             "L2: Ethernet unrecovered after %d L1 resets; self-restarting "
             "(restart %u of %d in boot-loop window)",
             BOS_SELF_HEAL_L1_MAX_ATTEMPTS,
             (unsigned)s_rtc_l2_count,
             BOS_SELF_HEAL_L2_BOOTLOOP_MAX);

    /* Give the log a moment to flush over UART before the restart. */
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ---- Monitor task --------------------------------------------------------- */

static void monitor_task(void *ctx)
{
    (void)ctx;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BOS_SELF_HEAL_POLL_MS));

        uint64_t now = (uint64_t)esp_timer_get_time();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool has_ipv4 = s_has_ipv4;
        uint64_t last_ip = s_last_ip_us;       /* 0 if never */
        uint64_t last_action = s_last_action_us;
        uint32_t l1_attempts = s_l1_attempts;
        xSemaphoreGive(s_lock);

        if (has_ipv4) {
            /* HEALTHY: never touch the W5500 while an IP is held. */
            continue;
        }

        /* No IPv4. Reference point for "how long down": the later of the last
         * successful IP and the last recovery action (so each L1 attempt gets
         * a fresh grace window before the next escalation). Before the first
         * IP, the reference is boot (esp_timer epoch = 0). */
        uint64_t reference = last_ip;
        if (last_action > reference) {
            reference = last_action;
        }
        uint64_t no_ip_us = now - reference;

        if (no_ip_us < BOS_SELF_HEAL_NO_IP_THRESHOLD_US) {
            /* Still inside the grace window; give DHCP/link more time. */
            continue;
        }

        if (l1_attempts < BOS_SELF_HEAL_L1_MAX_ATTEMPTS) {
            size_t slot = ring_record(BOS_HEAL_L1_W5500_RESET, BOS_HEAL_OUTCOME_PENDING, no_ip_us);
            esp_err_t err = l1_w5500_reset();

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_l1_attempts++;
            s_l1_total++;
            s_last_action_us = (uint64_t)esp_timer_get_time();
            xSemaphoreGive(s_lock);

            ring_set_outcome(slot, err == ESP_OK ? BOS_HEAL_OUTCOME_PENDING : BOS_HEAL_OUTCOME_FAILED);
            /* Outcome is judged on the next poll: if an IP arrives, the IP
             * handler clears s_l1_attempts and we mark recovery implicitly by
             * the next healthy poll. */
            continue;
        }

        /* L1 exhausted. Respect the L2 minimum interval so a restart cannot
         * be triggered back-to-back faster than the cooldown. */
        if (last_action != 0 && (now - last_action) < BOS_SELF_HEAL_L2_MIN_INTERVAL_US) {
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_last_action_us = now;
        xSemaphoreGive(s_lock);

        l2_restart(no_ip_us); /* may not return */
    }
}

/* ---- Public API ----------------------------------------------------------- */

esp_err_t bos_self_heal_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Initialise RTC boot-loop state only on a true power-on (magic absent);
     * preserve it across self-restarts. */
    if (s_rtc_magic != BOS_SELF_HEAL_RTC_MAGIC) {
        s_rtc_magic = BOS_SELF_HEAL_RTC_MAGIC;
        s_rtc_l2_count = 0;
        s_rtc_l2_window_start_us = (uint64_t)esp_timer_get_time();
    }

    esp_err_t err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register IP event handler: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register ETH event handler: %s", esp_err_to_name(err));
        return err;
    }

    /* 4 KiB stack: the task does GPIO + esp_eth_stop/start + logging; no deep
     * recursion or large stack buffers. */
    if (xTaskCreate(monitor_task, "bos_self_heal", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create self-heal monitor task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG,
             "Ethernet self-heal started: poll %ds, no-IP threshold %llds, "
             "L1 W5500 reset x%d, L2 restart cap %d/window (RST GPIO%d)",
             BOS_SELF_HEAL_POLL_MS / 1000,
             (long long)(BOS_SELF_HEAL_NO_IP_THRESHOLD_US / 1000000ULL),
             BOS_SELF_HEAL_L1_MAX_ATTEMPTS,
             BOS_SELF_HEAL_L2_BOOTLOOP_MAX,
             BOS_W5500_RST_GPIO);
    return ESP_OK;
}

/* Append one quoted decimal u64 via the shared helper (no %llu). */
static int append(char *out, size_t out_len, size_t *pos, const char *fragment)
{
    size_t flen = strlen(fragment);
    if (*pos + flen >= out_len) {
        return -1;
    }
    memcpy(out + *pos, fragment, flen);
    *pos += flen;
    return 0;
}

int bos_self_heal_json(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return -1;
    }
    if (s_lock == NULL) {
        /* Not started yet: honest-null. */
        int n = snprintf(out, out_len, "null");
        return (n < 0 || n >= (int)out_len) ? -1 : n;
    }

    /* Snapshot under the lock into locals, then render without holding it. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has_ipv4 = s_has_ipv4;
    bool link_up = s_link_up;
    uint64_t last_ip_us = s_last_ip_us;
    uint32_t l1_total = s_l1_total;
    size_t total = s_ring_count;
    uint32_t rtc_l2 = s_rtc_l2_count;
    bos_heal_event_t ring_copy[BOS_HEAL_RING_LEN];
    memcpy(ring_copy, s_ring, sizeof(ring_copy));
    xSemaphoreGive(s_lock);

    uint64_t now = (uint64_t)esp_timer_get_time();
    uint64_t ms_since_ip = (last_ip_us == 0) ? 0 : (now - last_ip_us) / 1000ULL;

    char num[21];
    size_t pos = 0;
    out[0] = '\0';

    if (append(out, out_len, &pos, "{\"healthy\":") != 0) return -1;
    if (append(out, out_len, &pos, has_ipv4 ? "true" : "false") != 0) return -1;
    if (append(out, out_len, &pos, ",\"link_up\":") != 0) return -1;
    if (append(out, out_len, &pos, link_up ? "true" : "false") != 0) return -1;
    if (append(out, out_len, &pos, ",\"has_ipv4\":") != 0) return -1;
    if (append(out, out_len, &pos, has_ipv4 ? "true" : "false") != 0) return -1;

    if (append(out, out_len, &pos, ",\"ms_since_last_ip\":") != 0) return -1;
    if (last_ip_us == 0) {
        if (append(out, out_len, &pos, "null") != 0) return -1;
    } else {
        bos_u64_to_dec(ms_since_ip, num);
        if (append(out, out_len, &pos, num) != 0) return -1;
    }

    if (append(out, out_len, &pos, ",\"l1_w5500_resets\":") != 0) return -1;
    bos_u64_to_dec((uint64_t)l1_total, num);
    if (append(out, out_len, &pos, num) != 0) return -1;

    if (append(out, out_len, &pos, ",\"l2_restarts_in_window\":") != 0) return -1;
    bos_u64_to_dec((uint64_t)rtc_l2, num);
    if (append(out, out_len, &pos, num) != 0) return -1;

    if (append(out, out_len, &pos, ",\"events\":[") != 0) return -1;

    /* Emit up to the last BOS_HEAL_RING_LEN events, oldest-first. */
    size_t count = total < BOS_HEAL_RING_LEN ? total : BOS_HEAL_RING_LEN;
    size_t start = total < BOS_HEAL_RING_LEN ? 0 : total % BOS_HEAL_RING_LEN;
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start + i) % BOS_HEAL_RING_LEN;
        const bos_heal_event_t *e = &ring_copy[idx];
        if (i != 0) {
            if (append(out, out_len, &pos, ",") != 0) return -1;
        }
        if (append(out, out_len, &pos, "{\"uptime_ms\":") != 0) return -1;
        bos_u64_to_dec(e->uptime_ms, num);
        if (append(out, out_len, &pos, num) != 0) return -1;
        if (append(out, out_len, &pos, ",\"no_ip_ms\":") != 0) return -1;
        bos_u64_to_dec((uint64_t)e->trigger_no_ip_ms, num);
        if (append(out, out_len, &pos, num) != 0) return -1;
        if (append(out, out_len, &pos, ",\"level\":\"") != 0) return -1;
        if (append(out, out_len, &pos, level_str(e->level)) != 0) return -1;
        if (append(out, out_len, &pos, "\",\"outcome\":\"") != 0) return -1;
        if (append(out, out_len, &pos, outcome_str(e->outcome)) != 0) return -1;
        if (append(out, out_len, &pos, "\"}") != 0) return -1;
    }

    if (append(out, out_len, &pos, "]}") != 0) return -1;
    out[pos] = '\0';
    return (int)pos;
}
