#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "devname.h"
#include "settings.h"
#include "webhooks.h"
#include "wifi_pick.h"

static const char *TAG = "wifi_mgr";
static const char *NVS_NAMESPACE = "wifi";
// Pre-2.2 single-network keys. Read once for the migration, then kept in sync
// with slot 0 so a rollback to an older image still connects.
static const char *NVS_KEY_SSID  = "ssid";
static const char *NVS_KEY_PASS  = "pass";

#define AP_CHANNEL      1
#define AP_MAX_CONN     4
#define STA_CONN_TIMEOUT_MS  10000
#define STA_START_TIMEOUT_MS 3000
#define SCAN_MAX_RECORDS     20
#define RADIO_WAIT_MS        20000   // API scan vs. a running connect pass

// Runtime reconnect: back off between passes over the saved list, then keep
// retrying forever. Whenever the setup AP is up - raised at boot or after a long
// outage - the STA keeps trying alongside it, slowing to AP_RETRY_INTERVAL_MS
// only while somebody is actually on the portal: a router that is merely
// rebooting must not strand the display in setup mode.
#define RETRY_BACKOFF_STEPS   5
static const uint32_t RETRY_BACKOFF_MS[RETRY_BACKOFF_STEPS] =
    {2000, 5000, 15000, 30000, 60000};
#define RETRY_POLL_MS         2000
#define AP_FALLBACK_AFTER_US  (30LL * 60 * 1000000)   // 30 min offline
#define AP_RETRY_INTERVAL_MS  (5 * 60 * 1000)

#define BIT_STA_CONNECTED  BIT0
#define BIT_STA_FAIL       BIT1
#define BIT_STA_STARTED    BIT2

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_net_t;

static EventGroupHandle_t s_evt;
static wifi_mgr_status_t  s_status;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static TaskHandle_t       s_dns_task;
static volatile bool      s_dns_run;
static SemaphoreHandle_t  s_radio;      // serialises scans and connect attempts
static SemaphoreHandle_t  s_nets_lock;  // guards s_nets
static TaskHandle_t       s_retry_task; // runtime reconnect; poked on a new save
static wifi_net_t         s_nets[WIFI_MGR_MAX_NETS];
static bool               s_wifi_started;
static bool               s_ap_active;
static wifi_mgr_state_cb_t s_state_cb;
// Stored device name, "" when unset. Drives the netif hostname and the setup-AP
// SSID (devname.h resolves both, including the defaults for an unset name).
static char               s_devname[DEVNAME_MAX + 1];

// Hostname on both interfaces. Called before the radio comes up so the first
// DHCP request already carries it.
static void apply_hostname(void)
{
    const char *host = devname_hostname(s_devname);
    if (s_sta_netif) esp_netif_set_hostname(s_sta_netif, host);
    if (s_ap_netif)  esp_netif_set_hostname(s_ap_netif, host);
}

static void set_reported_ap_ssid(void)
{
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s",
             devname_ap_ssid(s_devname));
}

// ---- NVS: saved networks -------------------------------------------------

static void slot_key(char *buf, size_t len, const char *base, int slot)
{
    snprintf(buf, len, "%s%d", base, slot);
}

static void nets_read(nvs_handle_t h)
{
    for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
        char key[16];
        size_t len = sizeof(s_nets[i].ssid);
        slot_key(key, sizeof(key), NVS_KEY_SSID, i);
        if (nvs_get_str(h, key, s_nets[i].ssid, &len) != ESP_OK) {
            s_nets[i].ssid[0] = '\0';
            continue;
        }
        len = sizeof(s_nets[i].pass);
        slot_key(key, sizeof(key), NVS_KEY_PASS, i);
        if (nvs_get_str(h, key, s_nets[i].pass, &len) != ESP_OK) {
            s_nets[i].pass[0] = '\0';   // open network, or a half-written slot
        }
    }
}

// Write every slot from RAM: occupied slots as strings, empty ones erased, and
// slot 0 mirrored onto the legacy keys.
static esp_err_t nets_write(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
        char ks[16], kp[16];
        slot_key(ks, sizeof(ks), NVS_KEY_SSID, i);
        slot_key(kp, sizeof(kp), NVS_KEY_PASS, i);
        if (s_nets[i].ssid[0] != '\0') {
            nvs_set_str(h, ks, s_nets[i].ssid);
            nvs_set_str(h, kp, s_nets[i].pass);
        } else {
            nvs_erase_key(h, ks);
            nvs_erase_key(h, kp);
        }
    }
    if (s_nets[0].ssid[0] != '\0') {
        nvs_set_str(h, NVS_KEY_SSID, s_nets[0].ssid);
        nvs_set_str(h, NVS_KEY_PASS, s_nets[0].pass);
    } else {
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
    }

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Load the slots and reconcile them with the legacy pair. nets_write() keeps
// that pair equal to slot 0, so any disagreement means an older image wrote it
// last - a pre-2.2 build knows only those keys - and its credentials are the
// newer ones. This also covers the plain first-boot-after-update migration,
// where slot 0 is empty. Devices in the field must not lose their credentials
// in either direction.
static void nets_load(void)
{
    memset(s_nets, 0, sizeof(s_nets));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    nets_read(h);

    wifi_net_t legacy;
    memset(&legacy, 0, sizeof(legacy));
    size_t len = sizeof(legacy.ssid);
    bool have_legacy = (nvs_get_str(h, NVS_KEY_SSID, legacy.ssid, &len) == ESP_OK &&
                        legacy.ssid[0] != '\0');
    if (have_legacy) {
        len = sizeof(legacy.pass);
        if (nvs_get_str(h, NVS_KEY_PASS, legacy.pass, &len) != ESP_OK) {
            legacy.pass[0] = '\0';   // open network
        }
    }
    nvs_close(h);

    if (!have_legacy) return;
    if (strcmp(legacy.ssid, s_nets[0].ssid) == 0 &&
        strcmp(legacy.pass, s_nets[0].pass) == 0) {
        return;   // mirror in step
    }

    // Promote it to slot 0. An SSID already in the list moves up (that also
    // picks up a password changed on the older image); anything else pushes
    // the rest down and drops the oldest slot.
    int from = WIFI_MGR_MAX_NETS - 1;
    for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
        if (strcmp(s_nets[i].ssid, legacy.ssid) == 0) { from = i; break; }
    }
    for (int i = from; i > 0; i--) s_nets[i] = s_nets[i - 1];
    s_nets[0] = legacy;

    ESP_LOGI(TAG, "adopted stored credentials for '%s' into slot 0",
             s_nets[0].ssid);
    if (nets_write() != ESP_OK) {
        ESP_LOGW(TAG, "could not persist slot 0; retrying next boot");
    }
}

static int nets_count_locked(void)
{
    int n = 0;
    for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
        if (s_nets[i].ssid[0] != '\0') n++;
    }
    return n;
}

// Compacted copy of the saved list. `out` must hold WIFI_MGR_MAX_NETS entries.
static int nets_snapshot(wifi_net_t *out)
{
    int n = 0;
    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
        if (s_nets[i].ssid[0] == '\0') continue;
        out[n++] = s_nets[i];
    }
    xSemaphoreGive(s_nets_lock);
    return n;
}

// ---- Event handlers -----------------------------------------------------

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // The connect attempts are driven explicitly (one per saved network),
        // so this only unblocks the attempt loop.
        xEventGroupSetBits(s_evt, BIT_STA_STARTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_status.connected = false;
        xEventGroupSetBits(s_evt, BIT_STA_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&evt->ip_info.ip));
        s_status.connected = true;
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            s_status.rssi = info.rssi;
            strncpy(s_status.ssid, (const char *)info.ssid, sizeof(s_status.ssid) - 1);
        }
        xEventGroupSetBits(s_evt, BIT_STA_CONNECTED);

        // Tell the outside world where the device landed (DHCP renews and
        // reconnects included). webhooks_fire copies the payload, so a stack
        // buffer is fine here.
        char payload[32];
        snprintf(payload, sizeof(payload), "{\"ip\":\"%s\"}", s_status.ip);
        webhooks_fire(WEBHOOK_EVT_IP_CHANGE, payload);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        // Associated but with no address: an expired or NAKed DHCP lease, which
        // produces no disconnect event. esp_netif also posts this a while after
        // an ordinary disconnect and while the setup AP is up, so only act when
        // we still believe we are an online STA - and leave the event group
        // alone, or a late event would abort a running connect attempt.
        if (s_status.mode == WIFI_MGR_MODE_STA) {
            ESP_LOGW(TAG, "STA lost its IP address; a new connect pass will run");
            s_status.connected = false;
            s_status.ip[0] = '\0';
        }
    }
}

// ---- Captive-portal DNS hijack -----------------------------------------
//
// Tiny UDP server on port 53 that answers every A query with the AP's IP.
// Just enough RFC1035 to make phones decide they have internet on the AP.

static void dns_hijack_task(void *arg)
{
    uint32_t ap_ip = (uint32_t)(uintptr_t)arg;  // packed v4 address

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket() failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns: bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    // Receive timeout so stopping the AP after a reconnect actually ends this
    // task instead of leaving it parked in recvfrom().
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                        (struct sockaddr *)&src, &src_len);
        if (n < 12) continue;

        // Build a minimal A-record response pointing at `ap_ip`.
        buf[2] |= 0x80;             // QR = response
        buf[3] = (buf[3] & 0x0F) | 0x80;  // RA bit
        buf[6] = 0; buf[7] = 1;      // ANCOUNT = 1
        buf[8] = 0; buf[9] = 0;      // NSCOUNT
        buf[10] = 0; buf[11] = 0;    // ARCOUNT

        int qoff = 12;
        while (qoff < n && buf[qoff] != 0) qoff += buf[qoff] + 1;
        qoff += 1 + 4;  // skip null + QTYPE + QCLASS

        if (qoff + 16 > (int)sizeof(buf)) continue;

        // Answer: name pointer to offset 12, TYPE A, CLASS IN, TTL 60, RDLEN 4, RDATA = ap_ip.
        buf[qoff++] = 0xC0; buf[qoff++] = 0x0C;
        buf[qoff++] = 0x00; buf[qoff++] = 0x01;
        buf[qoff++] = 0x00; buf[qoff++] = 0x01;
        buf[qoff++] = 0x00; buf[qoff++] = 0x00; buf[qoff++] = 0x00; buf[qoff++] = 0x3C;
        buf[qoff++] = 0x00; buf[qoff++] = 0x04;
        buf[qoff++] = (ap_ip >>  0) & 0xFF;
        buf[qoff++] = (ap_ip >>  8) & 0xFF;
        buf[qoff++] = (ap_ip >> 16) & 0xFF;
        buf[qoff++] = (ap_ip >> 24) & 0xFF;

        sendto(sock, buf, qoff, 0, (struct sockaddr *)&src, src_len);
    }
    close(sock);
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static void dns_start(void)
{
    // Re-arm the flag first: a task still winding down from a previous AP
    // session then just keeps serving (the AP address never changes).
    s_dns_run = true;
    if (s_dns_task) return;
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096,
                (void *)(uintptr_t)ip.ip.addr, 5, &s_dns_task);
}

// ---- STA / AP startup --------------------------------------------------

// Bring the radio up with the STA interface active. While the setup AP is
// running the mode stays AP+STA so a reconnect attempt cannot take the portal
// down under a user who is mid-setup.
static esp_err_t sta_radio_up(void)
{
    esp_err_t err = esp_wifi_set_mode(s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    if (s_wifi_started) return ESP_OK;

    xEventGroupClearBits(s_evt, BIT_STA_STARTED);
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;
    xEventGroupWaitBits(s_evt, BIT_STA_STARTED, pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(STA_START_TIMEOUT_MS));
    return ESP_OK;
}

static bool try_sta(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    // Cancel whatever the previous attempt left running; esp_wifi_connect()
    // rejects a config change mid-association, and the disconnect is async.
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config for %s failed: %s", ssid, esp_err_to_name(err));
        return false;
    }
    xEventGroupClearBits(s_evt, BIT_STA_CONNECTED | BIT_STA_FAIL);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect to %s failed: %s", ssid, esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_evt,
                                           BIT_STA_CONNECTED | BIT_STA_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(STA_CONN_TIMEOUT_MS));
    if (bits & BIT_STA_CONNECTED) {
        ESP_LOGI(TAG, "connected to %s, ip=%s", ssid, s_status.ip);
        s_status.mode = WIFI_MGR_MODE_STA;
        strncpy(s_status.ssid, ssid, sizeof(s_status.ssid) - 1);
        return true;
    }
    ESP_LOGW(TAG, "STA connect to %s did not complete", ssid);
    esp_wifi_disconnect();
    return false;
}

// Try the saved networks once, best candidate first: a scan decides which are
// in range (strongest wins), then whatever the scan missed gets a blind
// attempt for the sake of hidden SSIDs. Caller holds s_radio.
static bool connect_saved(void)
{
    wifi_net_t nets[WIFI_MGR_MAX_NETS];
    int n = nets_snapshot(nets);
    if (n == 0) return false;

    if (sta_radio_up() != ESP_OK) {
        ESP_LOGE(TAG, "could not bring the STA interface up");
        return false;
    }

    static wifi_ap_record_t recs[SCAN_MAX_RECORDS];
    wifi_pick_scan_t seen[SCAN_MAX_RECORDS];
    const char *ssids[WIFI_MGR_MAX_NETS];
    int n_seen = 0;

    for (int i = 0; i < n; i++) ssids[i] = nets[i].ssid;

    uint16_t found = SCAN_MAX_RECORDS;
    wifi_scan_config_t scan_cfg = {0};
    if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK &&
        esp_wifi_scan_get_ap_records(&found, recs) == ESP_OK) {
        for (uint16_t i = 0; i < found && n_seen < SCAN_MAX_RECORDS; i++) {
            seen[n_seen].ssid = (const char *)recs[i].ssid;
            seen[n_seen].rssi = recs[i].rssi;
            n_seen++;
        }
    } else {
        ESP_LOGW(TAG, "scan failed; trying saved networks in stored order");
    }

    int order[WIFI_MGR_MAX_NETS];
    int n_try = wifi_pick_order(ssids, n, seen, n_seen, order, WIFI_MGR_MAX_NETS);
    for (int i = 0; i < n_try; i++) {
        if (try_sta(nets[order[i]].ssid, nets[order[i]].pass)) return true;
    }
    return false;
}

static void start_ap(bool keep_sta)
{
    // The SSID is the device name (or the historical default when unset), read
    // fresh on every raise so a rename applies the next time the setup network
    // appears - no reboot, and no renaming out from under a user who is on the
    // portal right now.
    set_reported_ap_ssid();
    const char *ssid = s_status.ap_ssid;

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len       = strlen(ssid);
    cfg.ap.channel        = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CONN;
    cfg.ap.authmode       = WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_mode(keep_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (err == ESP_OK && !s_wifi_started) {
        err = esp_wifi_start();
        if (err == ESP_OK) s_wifi_started = true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
        return;
    }

    s_ap_active        = true;
    s_status.mode      = WIFI_MGR_MODE_AP;
    s_status.rssi      = 0;
    s_status.connected = true;   // reachable, on its own network
    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", ssid);
    strncpy(s_status.ip, "192.168.4.1", sizeof(s_status.ip) - 1);

    dns_start();
    ESP_LOGI(TAG, "AP started (SSID=%s, ip=192.168.4.1%s)", ssid,
             keep_sta ? ", STA retries continue" : "");
}

static void stop_ap(void)
{
    if (!s_ap_active) return;
    s_dns_run   = false;
    s_ap_active = false;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not drop AP mode: %s", esp_err_to_name(err));
    }
}

static void notify_state(void)
{
    if (!s_state_cb) return;
    wifi_mgr_status_t st;
    wifi_manager_get_status(&st);
    s_state_cb(&st);
}

// ---- runtime reconnect --------------------------------------------------

// Associated AND holding an address: without the address check a device whose
// DHCP lease went away stays "online" forever and no pass ever runs.
static bool sta_online(void)
{
    if (!s_status.connected || s_status.mode != WIFI_MGR_MODE_STA) return false;
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

// Is somebody associated with the setup AP right now? A connect pass scans,
// which stalls the portal, so retries slow right down while it is in use.
static bool ap_has_clients(void)
{
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
}

// Started in every case, including a boot that ended in the setup AP. Policy:
// retry the saved networks forever with a growing backoff (2 s to 60 s), or at
// AP_RETRY_INTERVAL_MS while somebody is on the portal; after
// AP_FALLBACK_AFTER_US of continuous failure also raise the setup AP, keeping
// the STA retries running so a returning router is picked up and the AP goes
// away again. A newly saved network pokes the task so it is tried at once.
static void reconnect_task(void *arg)
{
    (void)arg;
    bool     was_online = sta_online();
    int      attempt    = 0;
    int64_t  down_us    = 0;

    while (1) {
        if (sta_online()) {
            if (!was_online) {
                was_online = true;
                ESP_LOGI(TAG, "STA link back up (%s)", s_status.ssid);
                notify_state();
            }
            attempt = 0;
            down_us = 0;
            vTaskDelay(pdMS_TO_TICKS(RETRY_POLL_MS));
            continue;
        }

        was_online = false;
        if (down_us == 0) {
            down_us = esp_timer_get_time();
            ESP_LOGW(TAG, "no STA link; retrying saved networks");
        }

        uint32_t wait = (s_ap_active && ap_has_clients()) ? AP_RETRY_INTERVAL_MS
                                                         : RETRY_BACKOFF_MS[attempt];
        // Notify instead of a plain delay: wifi_manager_saved_add() pokes this
        // task so a credential entered on the portal is attempted immediately.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
        if (sta_online()) continue;
        if (attempt < RETRY_BACKOFF_STEPS - 1) attempt++;

        bool ok = false;
        if (xSemaphoreTake(s_radio, portMAX_DELAY) == pdTRUE) {
            ok = connect_saved();
            xSemaphoreGive(s_radio);
        }

        if (ok) {
            if (s_ap_active) {
                xSemaphoreTake(s_radio, portMAX_DELAY);
                stop_ap();
                xSemaphoreGive(s_radio);
                ESP_LOGI(TAG, "reconnected; setup AP stopped");
            }
            was_online = true;
            attempt    = 0;
            down_us    = 0;
            notify_state();
            continue;
        }

        if (!s_ap_active && esp_timer_get_time() - down_us > AP_FALLBACK_AFTER_US) {
            ESP_LOGW(TAG, "offline for %lld s: raising the setup AP",
                     (long long)((esp_timer_get_time() - down_us) / 1000000));
            xSemaphoreTake(s_radio, portMAX_DELAY);
            start_ap(true);
            xSemaphoreGive(s_radio);
            notify_state();
        }
    }
}

// ---- Public API --------------------------------------------------------

esp_err_t wifi_manager_start(void)
{
    s_evt       = xEventGroupCreate();
    s_radio     = xSemaphoreCreateMutex();
    s_nets_lock = xSemaphoreCreateMutex();
    if (!s_evt || !s_radio || !s_nets_lock) return ESP_ERR_NO_MEM;

    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = WIFI_MGR_MODE_BOOT;
    settings_load_device_name(s_devname, sizeof(s_devname));
    set_reported_ap_ssid();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    // Before esp_wifi_start()/connect: the DHCP client sends the hostname with
    // its first request, so setting it later would not name the lease.
    apply_hostname();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                        &event_handler, NULL, NULL));

    nets_load();

    bool connected = false;
    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    int saved = nets_count_locked();
    xSemaphoreGive(s_nets_lock);

    if (saved > 0) {
        xSemaphoreTake(s_radio, portMAX_DELAY);
        connected = connect_saved();
        xSemaphoreGive(s_radio);
    } else {
        ESP_LOGI(TAG, "no saved networks");
    }

    if (!connected) {
        // Nothing reachable yet: raise the setup AP, in AP+STA so the reconnect
        // task can keep trying the saved networks underneath it and the portal's
        // scan works (esp_wifi_scan_start needs the STA interface). A router
        // that boots slower than the display must not strand us in setup mode.
        if (s_wifi_started) esp_wifi_stop();
        s_wifi_started = false;
        start_ap(true);
    }

    // Runs in every case, including the fallback above: it is the only code
    // that retries the STA, and the only thing that takes the setup AP down
    // again once a saved network answers.
    // 5 KB: the reconnect pass keeps a snapshot of the saved list plus the
    // scan-ordering arrays on its stack, and the state callback renders text.
    xTaskCreate(reconnect_task, "wifi_retry", 5120, NULL, 3, &s_retry_task);
    return ESP_OK;
}

void wifi_manager_set_state_cb(wifi_mgr_state_cb_t cb)
{
    s_state_cb = cb;
}

void wifi_manager_set_device_name(const char *name)
{
    snprintf(s_devname, sizeof(s_devname), "%s", name ? name : "");
    apply_hostname();
    // While the setup AP is up, keep reporting the SSID it is actually
    // broadcasting: restarting the radio would drop whoever is on the portal.
    // The new name is picked up the next time the AP is raised.
    if (!s_ap_active) set_reported_ap_ssid();
    ESP_LOGI(TAG, "device name '%s' (hostname %s, setup AP %s)",
             s_devname, devname_hostname(s_devname), s_status.ap_ssid);
}

void wifi_manager_get_status(wifi_mgr_status_t *out)
{
    if (!out) return;
    *out = s_status;

    if (s_status.mode == WIFI_MGR_MODE_STA && s_status.connected) {
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            out->rssi = info.rssi;
        }
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK) {
            snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
        }
    }
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *out_records, uint16_t *count)
{
    if (!out_records || !count || *count == 0) return ESP_ERR_INVALID_ARG;

    // A reconnect pass owns the radio for up to a minute; don't let two scans
    // overlap (esp_wifi rejects that) and don't block the handler forever.
    if (xSemaphoreTake(s_radio, pdMS_TO_TICKS(RADIO_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_scan_config_t cfg = {0};
    // esp_wifi_scan_start is supported only in STA and AP+STA mode (ESP-IDF
    // wifi-driver/station-scenarios); in AP-only it fails. Both AP paths raise
    // AP+STA for this reason.
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err == ESP_OK) {
        uint16_t want = *count;
        err = esp_wifi_scan_get_ap_records(&want, out_records);
        *count = want;
    }
    xSemaphoreGive(s_radio);
    return err;
}

int wifi_manager_saved_list(wifi_mgr_saved_t *out, int max)
{
    if (!out || max <= 0) return 0;

    int n = 0;
    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    for (int i = 0; i < WIFI_MGR_MAX_NETS && n < max; i++) {
        if (s_nets[i].ssid[0] == '\0') continue;
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", s_nets[i].ssid);
        out[n].connected = s_status.mode == WIFI_MGR_MODE_STA && s_status.connected &&
                           strcmp(s_nets[i].ssid, s_status.ssid) == 0;
        n++;
    }
    xSemaphoreGive(s_nets_lock);
    return n;
}

esp_err_t wifi_manager_saved_add(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) > 32 || (password && strlen(password) > 64)) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
            if (s_nets[i].ssid[0] == '\0') { slot = i; break; }
        }
    }
    if (slot < 0) {
        xSemaphoreGive(s_nets_lock);
        return ESP_ERR_NO_MEM;
    }
    snprintf(s_nets[slot].ssid, sizeof(s_nets[slot].ssid), "%s", ssid);
    snprintf(s_nets[slot].pass, sizeof(s_nets[slot].pass), "%s",
             password ? password : "");
    esp_err_t err = nets_write();
    xSemaphoreGive(s_nets_lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved network '%s' (slot %d)", ssid, slot);
        // Cut the retry wait short: a network added from the setup portal has to
        // be tried now, not after the current backoff.
        if (s_retry_task) xTaskNotifyGive(s_retry_task);
    }
    return err;
}

esp_err_t wifi_manager_saved_delete(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < WIFI_MGR_MAX_NETS; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        xSemaphoreGive(s_nets_lock);
        return ESP_ERR_NOT_FOUND;
    }
    // Close the gap so slot 0 (the rollback mirror) always holds a network
    // while any are left.
    for (int i = slot; i < WIFI_MGR_MAX_NETS - 1; i++) {
        s_nets[i] = s_nets[i + 1];
    }
    memset(&s_nets[WIFI_MGR_MAX_NETS - 1], 0, sizeof(s_nets[0]));
    esp_err_t err = nets_write();
    xSemaphoreGive(s_nets_lock);

    if (err == ESP_OK) ESP_LOGI(TAG, "forgot network '%s'", ssid);
    return err;
}

esp_err_t wifi_manager_save_and_reboot(const char *ssid, const char *password)
{
    esp_err_t err = wifi_manager_saved_add(ssid, password);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "credentials saved, rebooting...");
    // Give the HTTP handler a beat to flush its response.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

esp_err_t wifi_manager_reset_and_reboot(void)
{
    xSemaphoreTake(s_nets_lock, portMAX_DELAY);
    memset(s_nets, 0, sizeof(s_nets));
    esp_err_t err = nets_write();
    xSemaphoreGive(s_nets_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "erasing saved networks failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "all saved networks erased, rebooting into AP mode...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
