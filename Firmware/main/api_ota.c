// /api/ota/{upload,url,status}: the HTTP face of ota_update.c.
//   upload  streams the raw POST body straight into the inactive slot - no
//           JSON, no multipart, `curl --data-binary @firmware.bin` works
//   url     hands off to the async puller and returns immediately
//   status  open (no auth): slot info, running version, progress, last result

#include "api_handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "ota_update.h"
#include "web_common.h"

static const char *TAG = "api_ota";

#define UPLOAD_CHUNK       8192   // recv/flash granularity
#define UPLOAD_MAX_RETRIES 5      // consecutive socket timeouts before giving up
#define URL_BODY_MAX       600    // {"url":"..."} with a generous URL

// ---- POST /api/ota/upload (auth) -------------------------------------------

// Error paths return ESP_FAIL on purpose: the client may still be pushing
// megabytes of unread body, and only an httpd session close stops those
// bytes from being parsed as pipelined follow-up requests.
static esp_err_t h_ota_upload(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) {
        return ESP_FAIL;   // response sent; close to drop the unread body
    }

    const int total = req->content_len;
    if (total <= 0) {
        web_send_err(req, "empty body: POST the raw .bin image", 400);
        return ESP_FAIL;
    }

    esp_err_t err = ota_begin((size_t)total);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            web_send_err(req, "another update is already in progress", 409);
        } else if (err == ESP_ERR_INVALID_SIZE) {
            web_send_err(req, "image larger than the OTA slot", 413);
        } else {
            web_send_err(req, "could not start update (see /api/ota/status)", 500);
        }
        return ESP_FAIL;
    }

    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        ota_abort_update();
        web_send_err(req, "out of memory", 500);
        return ESP_FAIL;
    }

    int received = 0;
    int timeouts = 0;
    while (received < total) {
        int want = total - received;
        if (want > UPLOAD_CHUNK) want = UPLOAD_CHUNK;

        int n = httpd_req_recv(req, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= UPLOAD_MAX_RETRIES) {
            continue;   // transient stall (flash-erase gaps can cause these)
        }
        if (n <= 0) {
            free(buf);
            ota_abort_update();
            char msg[96];
            snprintf(msg, sizeof(msg), "receive failed at byte %d of %d",
                     received, total);
            ESP_LOGE(TAG, "upload: %s", msg);
            web_send_err(req, msg, 500);
            return ESP_FAIL;
        }
        timeouts = 0;

        err = ota_write_chunk(buf, n);
        if (err != ESP_OK) {
            free(buf);
            ota_abort_update();
            char msg[96];
            snprintf(msg, sizeof(msg), "flash write failed at byte %d of %d (%s)",
                     received, total, esp_err_to_name(err));
            ESP_LOGE(TAG, "upload: %s", msg);
            web_send_err(req, msg, 500);
            return ESP_FAIL;
        }
        received += n;
    }
    free(buf);

    err = ota_end_and_reboot();
    if (err != ESP_OK) {
        // ota_update.c has already cleaned up; surface its diagnostic. 400:
        // the transfer worked, the payload itself was not a bootable image.
        ota_status_t st;
        ota_get_status(&st);
        web_send_err(req, st.last_result, 400);
        return ESP_FAIL;
    }

    // The reboot is delayed inside ota_update.c so this response gets out.
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "note", "update applied, rebooting");
    return web_send_json(req, r);
}

// ---- POST /api/ota/url (auth) ----------------------------------------------

static esp_err_t h_ota_url(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;   // response sent

    cJSON *body = web_read_json(req, URL_BODY_MAX);
    if (!body) {
        web_send_err(req, "body must be JSON: {\"url\":\"http(s)://...\"}", 400);
        return ESP_FAIL;   // body may be unread (oversized): close the session
    }

    const cJSON *url = cJSON_GetObjectItemCaseSensitive(body, "url");
    if (!cJSON_IsString(url) || url->valuestring[0] == '\0') {
        cJSON_Delete(body);
        return web_send_err(req, "missing \"url\"", 400);
    }
    if (strncmp(url->valuestring, "http://", 7) != 0 &&
        strncmp(url->valuestring, "https://", 8) != 0) {
        cJSON_Delete(body);
        return web_send_err(req, "url must start with http:// or https://", 400);
    }

    esp_err_t err = ota_from_url(url->valuestring);   // copies the URL
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_err(req, "another update is already in progress", 409);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_err(req, "url too long", 400);
    }
    if (err != ESP_OK) {
        return web_send_err(req, "could not start update", 500);
    }

    // Download runs async; the UI polls /api/ota/status for progress/result.
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "note", "update started");
    return web_send_json(req, r);
}

// ---- GET /api/ota/status (open) --------------------------------------------

static esp_err_t h_ota_status(httpd_req_t *req)
{
    ota_status_t st;
    ota_get_status(&st);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "running_version", st.running_version);
    cJSON_AddStringToObject(r, "running_slot", st.running_slot);
    cJSON_AddStringToObject(r, "last_result", st.last_result);
    cJSON_AddBoolToObject(r, "in_progress", st.in_progress);
    cJSON_AddNumberToObject(r, "progress_pct", st.progress_pct);
    return web_send_json(req, r);
}

// GET /api/ota/check: one blocking HTTPS round trip to the GitHub releases
// API (about a second on a healthy network, 10 s timeout worst case). Open
// like /api/ota/status; installing the offered asset still goes through the
// auth-gated POST /api/ota/url.
static esp_err_t h_ota_check(httpd_req_t *req)
{
    ota_check_t chk;
    esp_err_t err = ota_check_github(&chk);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
    cJSON_AddStringToObject(r, "current", chk.current);
    cJSON_AddStringToObject(r, "latest", chk.latest);
    cJSON_AddBoolToObject(r, "update_available", chk.update_available);
    cJSON_AddStringToObject(r, "asset_url", chk.asset_url);
    if (chk.error[0] != '\0') {
        cJSON_AddStringToObject(r, "error", chk.error);
    }
    return web_send_json(req, r);
}

// ---- registration ----------------------------------------------------------

void api_ota_register(httpd_handle_t srv)
{
    static const httpd_uri_t upload = {
        .uri = "/api/ota/upload", .method = HTTP_POST, .handler = h_ota_upload};
    static const httpd_uri_t url = {
        .uri = "/api/ota/url", .method = HTTP_POST, .handler = h_ota_url};
    static const httpd_uri_t status = {
        .uri = "/api/ota/status", .method = HTTP_GET, .handler = h_ota_status};
    static const httpd_uri_t check = {
        .uri = "/api/ota/check", .method = HTTP_GET, .handler = h_ota_check};

    web_register_uri(srv, &upload);
    web_register_uri(srv, &url);
    web_register_uri(srv, &check);
    web_register_uri(srv, &status);
    ESP_LOGI(TAG, "OTA endpoints registered");
}
