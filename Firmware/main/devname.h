#pragma once

// Device-name rules. ONE name identifies the device in three places: the
// network hostname (STA + AP netifs), the setup-AP SSID and the "device" field
// of outbound webhook payloads. Pure module (no ESP-IDF, no allocation) so the
// charset and the default-resolution rules are host-tested.

#include <stdbool.h>
#include <stddef.h>

// 24 chars leaves headroom under the 32-char SSID limit. The charset is the
// hostname one (letters, digits, hyphen, no leading/trailing hyphen), which is
// also legal in an SSID - one name, valid everywhere it is used.
#define DEVNAME_MAX 24

// What an unset name resolves to. The setup-AP SSID is the historical one, so
// an untouched device still appears under the name the guides print; the
// hostname is new (pre-2.2 builds set none and took the IDF default).
#define DEVNAME_DEFAULT_HOST "led-display"
#define DEVNAME_DEFAULT_AP   "LEDDisplay-Setup"

// Strict check, used by the API before anything is stored.
bool devname_valid(const char *s);

// Lenient pass for values that were never validated against these rules: the
// pre-2.2 webhook device name and imported config files. Illegal characters
// become hyphens, runs collapse, the ends are trimmed and the result is cut to
// DEVNAME_MAX. False when nothing usable is left (`out` is then empty).
bool devname_clean(const char *in, char *out, size_t outsz);

// Resolvers: `stored` is the saved name, NULL or "" when unset. The returned
// pointer is either `stored` itself or a string literal, so it lives as long
// as the caller's buffer.
const char *devname_hostname(const char *stored);
const char *devname_ap_ssid(const char *stored);

// Does an HTTP Host/Origin authority name this device? `name` is the resolved
// hostname. Matches the bare name or the name plus ONE DNS suffix label
// ("panel.local", "panel.lan"), case-insensitively, with an optional :port.
// Deeper names stay unmatched so the DNS-rebinding guard in web_common.c keeps
// working.
bool devname_host_matches(const char *host, const char *name);
