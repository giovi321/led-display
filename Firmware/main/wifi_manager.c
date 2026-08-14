#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "webhooks.h"

static const char *TAG          = "wifi_mgr";
static const char *NVS_NAMESPACE = "wifi";
static const char *NVS_KEY_SSID  = "ssid";
static const char *NVS_KEY_PASS  = "pass";

#define AP_SSID         "LEDDisplay-Setup"
#define AP_CHANNEL      1
#define AP_MAX_CONN     4
#define STA_CONN_TIMEOUT_MS  10000

#define BIT_STA_CONNECTED  BIT0
#define BIT_STA_FAIL       BIT1

static EventGroupHandle_t s_evt;
static wifi_mgr_status_t  s_status;
static esp_netif_t       *s_sta_netif;
static esp_netif_t       *s_ap_netif;
static TaskHandle_t       s_dns_task;
static volatile bool      s_dns_run;

// ---- NVS helpers --------------------------------------------------------

static esp_err_t nvs_read_creds(char *ssid, size_t ssid_len,
                                char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(h, NVS_KEY_PASS, pass, &pass_len);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PASS, pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_erase_creds(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---- Event handlers -----------------------------------------------------

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
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
    if (s_dns_task) return;
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    s_dns_run = true;
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096,
                (void *)(uintptr_t)ip.ip.addr, 5, &s_dns_task);
}

// ---- STA / AP startup --------------------------------------------------

static bool try_sta(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

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
    ESP_LOGW(TAG, "STA connect to %s timed out", ssid);
    esp_wifi_stop();
    return false;
}

static void start_ap(void)
{
    wifi_config_t cfg = {0};
    strncpy((char *)cfg.ap.ssid, AP_SSID, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len       = strlen(AP_SSID);
    cfg.ap.channel        = AP_CHANNEL;
    cfg.ap.max_connection = AP_MAX_CONN;
    cfg.ap.authmode       = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_status.mode = WIFI_MGR_MODE_AP;
    strncpy(s_status.ssid, AP_SSID, sizeof(s_status.ssid) - 1);
    strncpy(s_status.ip, "192.168.4.1", sizeof(s_status.ip) - 1);
    s_status.connected = true;

    dns_start();
    ESP_LOGI(TAG, "AP started (SSID=%s, ip=192.168.4.1)", AP_SSID);
}

// ---- Public API --------------------------------------------------------

esp_err_t wifi_manager_start(void)
{
    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = WIFI_MGR_MODE_BOOT;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, NULL));

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have_creds = (nvs_read_creds(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK)
                      && ssid[0] != '\0';

    if (have_creds && try_sta(ssid, pass)) {
        return ESP_OK;
    }

    if (have_creds) {
        // STA failed - drop into AP mode without erasing creds (user keeps them).
        esp_wifi_stop();
    }
    start_ap();
    return ESP_OK;
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

    wifi_scan_config_t cfg = {0};
    // esp_wifi_scan_start requires the radio to be up; works in both AP and STA mode.
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) return err;

    uint16_t want = *count;
    err = esp_wifi_scan_get_ap_records(&want, out_records);
    *count = want;
    return err;
}

esp_err_t wifi_manager_save_and_reboot(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_write_creds(ssid, password);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "credentials saved, rebooting...");
    // Give the HTTP handler a beat to flush its response.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  // unreachable
}

esp_err_t wifi_manager_reset_and_reboot(void)
{
    nvs_erase_creds();
    ESP_LOGI(TAG, "WiFi credentials erased, rebooting into AP mode...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}
