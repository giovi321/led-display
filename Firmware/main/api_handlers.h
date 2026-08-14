#pragma once

// Registration entry points for the per-domain HTTP handler modules.
// web_server.c starts httpd and calls each of these once. Every module owns
// its file exclusively:
//
//   api_display.c      /api/display/{text,brightness,brightness_cap,
//                      light_sensor,clear,test,map,identify}
//   api_layout.c       /api/layout (GET/POST)
//   api_colors.c       /api/colors, /api/value, /api/value/config
//   api_wifi.c         /api/wifi/{scan,networks,networks/delete,connect,reset}
//   api_system.c       /api/status, /api/system/{reboot,boot_screen,device_name,
//                      factory_reset}, /api/schedule
//   api_config.c       /api/config/{export,import}
//   api_ota.c          /api/ota/{upload,url,status}
//   api_integrations.c /api/integrations, /api/wireguard
//   web_static.c       /, gzipped assets, captive-portal probes

#include "esp_http_server.h"

void api_display_register(httpd_handle_t srv);
void api_layout_register(httpd_handle_t srv);
void api_colors_register(httpd_handle_t srv);
void api_wifi_register(httpd_handle_t srv);
void api_system_register(httpd_handle_t srv);
void api_config_register(httpd_handle_t srv);
void api_ota_register(httpd_handle_t srv);
void api_integrations_register(httpd_handle_t srv);
void web_static_register(httpd_handle_t srv);
