// Serves the embedded, gzipped web UI assets and the captive-portal probe
// URLs. Assets are gzipped at build time from main/web/ (see CMakeLists).

#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "api_handlers.h"

static const char *TAG = "web_static";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t style_css_gz_start[]  asm("_binary_style_css_gz_start");
extern const uint8_t style_css_gz_end[]    asm("_binary_style_css_gz_end");
extern const uint8_t app_js_gz_start[]     asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]       asm("_binary_app_js_gz_end");
extern const uint8_t editor_js_gz_start[]  asm("_binary_editor_js_gz_start");
extern const uint8_t editor_js_gz_end[]    asm("_binary_editor_js_gz_end");

typedef struct {
    const char    *uri;
    const uint8_t *start;
    const uint8_t *end;
    const char    *ctype;
} asset_t;

static const asset_t s_assets[] = {
    {"/",          index_html_gz_start, index_html_gz_end, "text/html; charset=utf-8"},
    {"/style.css", style_css_gz_start,  style_css_gz_end,  "text/css"},
    {"/app.js",    app_js_gz_start,     app_js_gz_end,     "application/javascript"},
    {"/editor.js", editor_js_gz_start,  editor_js_gz_end,  "application/javascript"},
};

static esp_err_t asset_get(httpd_req_t *req)
{
    const asset_t *a = (const asset_t *)req->user_ctx;
    httpd_resp_set_type(req, a->ctype);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // Small files on a LAN: cheap to refetch, and no-cache means a firmware
    // update is picked up on the next page load without cache busting.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
}

// Common captive-portal probe targets that phones hit to detect a portal.
// Returning the page (instead of redirecting) keeps the captive sheet on
// the connection screen, which is what users expect.
static esp_err_t captive_get(httpd_req_t *req)
{
    req->user_ctx = (void *)&s_assets[0];
    return asset_get(req);
}

void web_static_register(httpd_handle_t srv)
{
    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); i++) {
        httpd_uri_t u = {
            .uri      = s_assets[i].uri,
            .method   = HTTP_GET,
            .handler  = asset_get,
            .user_ctx = (void *)&s_assets[i],
        };
        httpd_register_uri_handler(srv, &u);
    }

    static const char *probes[] = {
        "/generate_204",                // Android
        "/gen_204",
        "/hotspot-detect.html",         // iOS / macOS
        "/library/test/success.html",
        "/ncsi.txt",                    // Windows
        "/connecttest.txt",
        "/redirect",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        httpd_uri_t u = {.uri = probes[i], .method = HTTP_GET, .handler = captive_get};
        httpd_register_uri_handler(srv, &u);
    }

    ESP_LOGI(TAG, "static assets + captive probes registered");
}
