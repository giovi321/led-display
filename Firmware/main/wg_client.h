#pragma once

// Optional WireGuard client (vendored esp_wireguard component). Compiled
// only when CONFIG_LEDDISP_WIREGUARD=y; wg_client.c provides no-op stubs
// otherwise so callers never need #ifdefs. Runtime-disabled until the user
// configures and enables it in the Network section.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WG_KEY_MAX       63   // base64 WireGuard keys are 44 chars; headroom
#define WG_HOST_MAX      64
#define WG_ALLOWED_MAX   64
#define WG_ADDR_MAX      32

typedef struct {
    bool     enabled;
    char     private_key[WG_KEY_MAX + 1];       // this device's private key
    char     peer_public_key[WG_KEY_MAX + 1];
    char     endpoint_host[WG_HOST_MAX + 1];    // hostname or IP
    uint16_t endpoint_port;
    char     address[WG_ADDR_MAX + 1];          // tunnel IP, e.g. "10.6.0.12/32"
    char     allowed_ips[WG_ALLOWED_MAX + 1];   // e.g. "10.6.0.0/24"
    uint16_t keepalive_s;                       // 0 = off, typical 25
} wg_config_t;

// Bring-up steps, in order. Reported after a crash reboot as the step that
// was in flight, which is the only way to see where a tunnel that takes the
// whole device down died.
typedef enum {
    WG_STAGE_IDLE    = 0,
    WG_STAGE_INIT    = 1,   // esp_wireguard_init: DNS lookup + PSA crypto init
    WG_STAGE_CONNECT = 2,   // esp_wireguard_connect: netif, key derivation, handshake
    WG_STAGE_ALLOWED = 3,   // routing the allowed IPs into the tunnel
    WG_STAGE_MONITOR = 4,   // steady state, the component's timers own it
} wg_stage_t;

typedef struct {
    bool    compiled_in;    // false when the Kconfig option is off
    bool    enabled;
    bool    up;             // handshake completed and peer reachable
    bool    held;           // enabled but suspended after repeated crash reboots
    int64_t last_handshake; // unix seconds, 0 = never
    // Diagnostics. The tunnel runs its crypto on the shared lwIP thread, so a
    // stack overflow there reboots the device with nothing to show for it.
    uint8_t  last_crash_stage;      // step in flight at the last crash
    uint32_t last_crash_tcpip_free; // free lwIP stack measured in that run
    uint32_t tcpip_stack_free_min;  // smallest free lwIP stack seen, bytes
} wg_status_t;

// Load config from NVS; if enabled, bring the tunnel up (needs WiFi STA).
esp_err_t wg_client_init(void);

// Save config and apply immediately (tear down / reconnect as needed).
esp_err_t wg_client_apply(const wg_config_t *cfg);

void wg_client_get_config(wg_config_t *out);
void wg_client_get_status(wg_status_t *out);
