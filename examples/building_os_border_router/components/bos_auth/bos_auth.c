#include "bos_auth.h"

#include <string.h>

#include "sdkconfig.h"

#include "bos_server_registration.h"

#define BOS_AUTH_VALUE_MAX 128

/* Constant-time equality over secret strings. Iterates the longer length so
 * runtime does not depend on WHERE the strings differ; only the length is
 * observable, never the content (the CWE-208 fix). */
static bool ct_equal(const char *expected, const char *provided)
{
    size_t elen = strlen(expected);
    size_t plen = strlen(provided);
    size_t max = elen > plen ? elen : plen;
    volatile unsigned char diff = (elen == plen) ? 0U : 1U;
    for (size_t i = 0; i < max; i++) {
        unsigned char e = i < elen ? (unsigned char)expected[i] : 0U;
        unsigned char p = i < plen ? (unsigned char)provided[i] : 0U;
        diff |= (unsigned char)(e ^ p);
    }
    return diff == 0U;
}

static esp_err_t header_value(httpd_req_t *req, const char *name, char *out, size_t out_len)
{
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len >= out_len) {
        return ESP_ERR_NOT_FOUND;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_len);
}

bool bos_auth_request_authorized(httpd_req_t *req)
{
    char expected[BOS_AUTH_VALUE_MAX];
    char provided[BOS_AUTH_VALUE_MAX];

    if (bos_server_registration_get_token(expected, sizeof(expected)) == ESP_OK &&
        expected[0] != '\0') {
        if (header_value(req, "X-Device-Token", provided, sizeof(provided)) == ESP_OK &&
            ct_equal(expected, provided)) {
            return true;
        }
    }

#ifdef CONFIG_BOS_SERVER_API_KEY
    if (CONFIG_BOS_SERVER_API_KEY[0] != '\0') {
        if (header_value(req, "x-api-key", provided, sizeof(provided)) == ESP_OK &&
            ct_equal(CONFIG_BOS_SERVER_API_KEY, provided)) {
            return true;
        }
    }
#endif

    return false;
}
