#pragma once

// Shared helpers for the HTTP API handler modules (api_*.c). Implemented in
// web_common.c; web_server.c owns server startup and calls each module's
// api_*_register(httpd_handle_t).

#include <stddef.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

// ---- responses -----------------------------------------------------------

// Sends `root` as application/json and deletes it. Never returns NULL data.
esp_err_t web_send_json(httpd_req_t *req, cJSON *root);

// {"ok":true}
esp_err_t web_send_ok(httpd_req_t *req);

// {"ok":false,"error":msg} with the given HTTP status code.
esp_err_t web_send_err(httpd_req_t *req, const char *msg, int code);

// ---- requests ------------------------------------------------------------

// Read the whole request body (up to max_len bytes) into a heap buffer,
// NUL-terminated. Caller frees. NULL on empty/oversized/failed reads.
char *web_read_body(httpd_req_t *req, size_t max_len);

// Convenience: read body + parse JSON. NULL on failure (response NOT sent).
cJSON *web_read_json(httpd_req_t *req, size_t max_len);

// ---- auth ----------------------------------------------------------------

// True if no API key is configured, or the request carries a matching
// X-Api-Key header. Constant-time comparison.
bool web_auth_ok(httpd_req_t *req);

// If web_auth_ok() fails: send a 401 JSON error and return ESP_FAIL.
// Handlers for mutating endpoints call this first:
//   if (web_require_auth(req) != ESP_OK) return ESP_OK;  // response sent
esp_err_t web_require_auth(httpd_req_t *req);
