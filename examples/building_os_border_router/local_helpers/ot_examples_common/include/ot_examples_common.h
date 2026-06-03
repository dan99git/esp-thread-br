/*
 * Compatibility header for ESP-IDF checkouts that do not ship
 * examples/openthread/ot_common_components/ot_examples_common.
 */

#pragma once

#include "sdkconfig.h"

#if CONFIG_ESP_COEX_EXTERNAL_COEXIST_ENABLE
#error "local ot_examples_common helper does not implement external coexistence"
#endif

static inline void ot_external_coexist_init(void)
{
}
