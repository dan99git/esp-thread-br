/**
 * Building OS Border Router: Ethernet self-diagnosis and self-recovery.
 *
 * Built for the recurring "BR goes dark after reboot" failure: the W5500 SPI
 * Ethernet driver attaches to the netif but never acquires an IPv4 ("Waiting
 * for IP" forever), leaving the BR unreachable on the LAN until a manual
 * workshop power-cycle.
 *
 * A dedicated monitor task watches the Ethernet backbone (link up + IPv4
 * acquired + time since last IP) and, only when the backbone is genuinely down
 * past a threshold, runs a bounded recovery ladder:
 *   L1: hardware-reset the W5500 (esp_eth_stop -> GPIO40 reset pulse ->
 *       esp_eth_start) up to N times.
 *   L2: esp_restart() the host, boot-loop-capped via RTC slow memory.
 *
 * The recovery history and current health snapshot are exposed through
 * bos_self_heal_json(), surfaced inside the /bos/status "self_heal" block.
 *
 * The Thread/RCP side already has esp-openthread RCP recovery
 * (esp_rcp_update) plus the /bos/thread-reset route; this component is
 * Ethernet-only by design.
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the self-heal monitor: registers ETH/IP event handlers for the
 * backbone, reads the boot-loop counter from RTC slow memory, and creates the
 * health-monitor task. Idempotent. */
esp_err_t bos_self_heal_start(void);

/* Render the current health snapshot + recovery-history ring as a JSON object
 * (no trailing newline) into out. Returns the number of bytes written
 * (excluding the NUL), or -1 if the buffer was too small. Safe to call from
 * the HTTP handler task. */
int bos_self_heal_json(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
