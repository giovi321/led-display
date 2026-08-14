#include "wifi_pick.h"

#include <stdint.h>
#include <string.h>

static int usable(const char *ssid)
{
    return ssid && ssid[0] != '\0';
}

// Strongest scan hit for `ssid`, or 0 when the scan never saw it.
static int scan_rssi(const char *ssid, const wifi_pick_scan_t *scan, int n_scan,
                     int *out_rssi)
{
    int found = 0;
    for (int i = 0; i < n_scan; i++) {
        if (!usable(scan[i].ssid) || strcmp(scan[i].ssid, ssid) != 0) continue;
        if (!found || scan[i].rssi > *out_rssi) *out_rssi = scan[i].rssi;
        found = 1;
    }
    return found;
}

int wifi_pick_order(const char *const *saved, int n_saved,
                    const wifi_pick_scan_t *scan, int n_scan,
                    int *out_order, int max_out)
{
    if (!saved || !out_order || n_saved <= 0 || max_out <= 0) return 0;
    if (!scan) n_scan = 0;
    if (n_saved > WIFI_PICK_MAX_SAVED) n_saved = WIFI_PICK_MAX_SAVED;

    uint32_t picked = 0;
    int n_out = 0;

    // Selection sort by RSSI over the saved entries the scan saw. n_saved is
    // single digits in practice, so the quadratic walk is cheaper than a
    // scratch array.
    while (n_out < max_out) {
        int best = -1, best_rssi = 0;
        for (int i = 0; i < n_saved; i++) {
            if (picked & ((uint32_t)1 << i)) continue;
            if (!usable(saved[i])) continue;
            int rssi = 0;
            if (!scan_rssi(saved[i], scan, n_scan, &rssi)) continue;
            if (best < 0 || rssi > best_rssi) {
                best = i;
                best_rssi = rssi;
            }
        }
        if (best < 0) break;
        picked |= (uint32_t)1 << best;
        out_order[n_out++] = best;
    }

    // Whatever the scan missed still deserves a blind attempt: hidden
    // networks never appear in scan results.
    for (int i = 0; i < n_saved && n_out < max_out; i++) {
        if (picked & ((uint32_t)1 << i)) continue;
        if (!usable(saved[i])) continue;
        out_order[n_out++] = i;
    }
    return n_out;
}
