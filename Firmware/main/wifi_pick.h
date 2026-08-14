#pragma once

// Which saved WiFi network to try first. Pure and allocation-free (no
// ESP-IDF, no libc beyond string compare) so the ordering rules have
// host-side unit tests: Firmware/test/host/test_wifi_pick.c.

#define WIFI_PICK_MAX_SAVED  32   // bound of the internal "already picked" mask

typedef struct {
    const char *ssid;
    int         rssi;   // dBm as reported by the scan; higher is stronger
} wifi_pick_scan_t;

// Order the saved networks into the sequence a connect attempt should walk:
// first every saved SSID the scan actually saw, strongest first, then the
// ones the scan missed (hidden SSIDs, scan gaps) in stored order. Duplicate
// scan hits for one SSID (several APs, several bands) count as their
// strongest. Ties keep stored order.
//
// Writes indices into `saved` and returns how many it wrote. NULL or empty
// saved entries are skipped. Passing no scan results (n_scan = 0) yields the
// stored order unchanged.
int wifi_pick_order(const char *const *saved, int n_saved,
                    const wifi_pick_scan_t *scan, int n_scan,
                    int *out_order, int max_out);
