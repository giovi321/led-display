#pragma once

// Auth headers for outbound HTTP requests (webhook sender, value poller):
// one optional custom header (name and value, e.g. "Authorization: Bearer ..").
// Static inline so it needs no CMake entry; it compiles anywhere that can
// include esp_http_client.h.
//
// The header value is never logged.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

// RFC 7230 token: 1+ of ALPHA DIGIT and !#$%&'*+-.^_`|~
static inline bool http_auth_name_valid(const char *name)
{
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL;
        if (!ok) return false;
    }
    return true;
}

// A header value must not carry line breaks (header injection).
static inline bool http_auth_value_valid(const char *value)
{
    return value && strpbrk(value, "\r\n") == NULL;
}

// Adds "<hdr_name>: <hdr_value>" when `hdr_name` is non-empty. An invalid
// custom header (bad name token or line break in the value) is skipped with a
// warning that does not name the value.
static inline void http_auth_apply(esp_http_client_handle_t c,
                                   const char *hdr_name, const char *hdr_value)
{
    if (!c) return;
    if (hdr_name && hdr_name[0]) {
        if (!http_auth_name_valid(hdr_name)) {
            ESP_LOGW("http_auth", "custom header name is not a valid token, skipped");
        } else if (!http_auth_value_valid(hdr_value ? hdr_value : "")) {
            ESP_LOGW("http_auth", "custom header value has line breaks, skipped");
        } else {
            esp_http_client_set_header(c, hdr_name, hdr_value ? hdr_value : "");
        }
    }
}
