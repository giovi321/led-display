#include "web_server.h"

#include "esp_log.h"

#include "api_handlers.h"

static const char *TAG = "web";

esp_err_t web_server_start(void)
{
    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // Headroom over the 67 currently registered (48 API, 19 in web_static:
    // assets, tab routes, captive-portal probes): httpd silently refuses
    // handlers past this bound, so it must never sit at the exact count.
    // Each slot costs one pointer.
    cfg.max_uri_handlers = 72;
    cfg.stack_size       = 10240;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    // Each api_*.c module registers its own URIs; every module owns its
    // file exclusively (see api_handlers.h). web_static.c goes last because
    // it registers the wildcard captive-portal probes.
    api_system_register(srv);
    api_display_register(srv);
    api_layout_register(srv);
    api_colors_register(srv);
    api_wifi_register(srv);
    api_config_register(srv);
    api_ota_register(srv);
    api_integrations_register(srv);
    api_value_poll_register(srv);
    web_static_register(srv);

    ESP_LOGI(TAG, "HTTP server listening on :80");
    return ESP_OK;
}
