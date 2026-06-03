/**
 * Building OS Border Router: local diagnostics web surface.
 *
 * Registers BOS diagnostics routes on the forked port 80 BR web server.
 * It reports BR health, backbone state, Thread state, and current BOS
 * ledger/convergence stubs without starting a second diagnostics port.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bos_diagnostics_server_start(void);

#ifdef __cplusplus
}
#endif
