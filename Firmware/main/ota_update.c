// OTA firmware updates. Two paths into the same state machine:
//   - web upload: api_ota.c streams the raw POST body through
//     ota_begin/ota_write_chunk/ota_end_and_reboot (runs in the httpd task)
//   - URL pull: ota_from_url() spawns a worker that drives esp_https_ota
// Only one update may run at a time; s_lock guards all shared state. The
// device keeps displaying/serving while the image streams into the inactive
// slot, and reboots only after a fully validated flash.

#include "ota_update.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "display.h"
#include "fw_version.h"
#include "integrations.h"
#include "webhooks.h"

static const char *TAG = "ota";

#define OTA_URL_MAX_LEN     512    // sanity cap for /api/ota/url targets
// Request-line + header room for the longest URL we can end up fetching,
// which is a redirect target rather than anything the user typed. 2 KB
// leaves margin for GitHub lengthening its signed asset URLs again.
#define OTA_TX_BUF_SIZE     2048
#define OTA_HTTP_TIMEOUT_MS 30000
#define OTA_WORKER_STACK    8192   // TLS handshake inside the worker needs room
#define OTA_REBOOT_DELAY_MS 1500   // lets httpd flush + webhook task deliver

// ---- shared state ----------------------------------------------------------

// One state machine for both update paths. The upload path mutates from the
// httpd task, the URL path from its worker; s_lock guards every field below.
static SemaphoreHandle_t      s_lock;
static bool                   s_in_progress;
// Latched once a successful update schedules its reboot: the freshly
// activated slot is exactly what esp_ota_get_next_update_partition() would
// hand a second update during the grace window, which would erase it.
static bool                   s_reboot_pending;
static esp_ota_handle_t       s_handle;            // upload path only
static const esp_partition_t *s_target;            // upload path only
static size_t                 s_total;             // 0 = unknown size
static size_t                 s_written;
static int                    s_progress_pct   = -1;
static char                   s_last_result[96] = "none";

static void ota_lock(void)
{
    // Lazy init is race-free here: the first possible callers are the httpd
    // task (which serves all requests from a single task) and app_main's
    // ota_mark_valid() (which does not touch the shared state). The URL
    // worker only exists after a handler has already created the mutex.
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void ota_unlock(void)
{
    xSemaphoreGive(s_lock);
}

// Record the human-readable outcome surfaced by /api/ota/status. Call with
// the lock held.
static void set_result(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_result(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_last_result, sizeof(s_last_result), fmt, ap);
    va_end(ap);
}

// {"result":"ok","version":...} on success, {"result":"error","detail":...}
// on failure. webhooks_fire() copies the string and queues, so this is cheap
// enough to call with the lock held.
static void fire_result_webhook(bool ok, const char *version_or_detail)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "result", ok ? "ok" : "error");
    cJSON_AddStringToObject(d, ok ? "version" : "detail", version_or_detail);
    char *s = cJSON_PrintUnformatted(d);
    webhooks_fire(WEBHOOK_EVT_OTA_RESULT, s ? s : "{}");
    if (s) cJSON_free(s);
    cJSON_Delete(d);
}

// ---- reboot ----------------------------------------------------------------

static void reboot_task(void *arg)
{
    (void)arg;
    // Grace period so the final HTTP response reaches the client and the
    // webhook worker gets a chance to POST the result before the link dies.
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}

static void schedule_reboot(void)
{
    if (xTaskCreate(reboot_task, "ota_reboot", 2560, NULL, 5, NULL) != pdPASS) {
        // Out of memory this late is unlikely; reboot inline as a fallback.
        // The HTTP response may be lost but the flashed update is already safe.
        esp_restart();
    }
}

// ---- upload path -----------------------------------------------------------

esp_err_t ota_begin(size_t image_size_or_zero)
{
    ota_lock();
    if (s_in_progress || s_reboot_pending) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        set_result("no OTA slot found (partition table without ota_0/ota_1?)");
        ESP_LOGE(TAG, "%s", s_last_result);
        ota_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (image_size_or_zero > next->size) {
        set_result("image (%u bytes) larger than slot %s (%u bytes)",
                   (unsigned)image_size_or_zero, next->label, (unsigned)next->size);
        ESP_LOGE(TAG, "%s", s_last_result);
        ota_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    // Stop driving the strip before the first flash operation, not after.
    // Writing flash stalls the RMT interrupt that clocks the WS2812B chain
    // mid-frame, which scrambles the panel and spikes the current far enough
    // to cut the supply out - and with OTA_WITH_SEQUENTIAL_WRITES below,
    // erases happen throughout the transfer rather than once at the start,
    // so there is no safe window later on.
    display_freeze_for_update();

    // Unknown size: OTA_WITH_SEQUENTIAL_WRITES erases flash as data arrives
    // instead of erasing the whole 6 MB slot up front, which would stall the
    // first chunk for seconds and can trip the sender's HTTP timeout.
    size_t declared = image_size_or_zero ? image_size_or_zero
                                         : (size_t)OTA_WITH_SEQUENTIAL_WRITES;
    esp_err_t err = esp_ota_begin(next, declared, &s_handle);
    if (err != ESP_OK) {
        set_result("esp_ota_begin failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_result);
        display_resume_after_update();
        ota_unlock();
        return err;
    }

    s_in_progress  = true;
    s_target       = next;
    s_total        = image_size_or_zero;
    s_written      = 0;
    s_progress_pct = image_size_or_zero ? 0 : -1;
    set_result("in progress");
    ESP_LOGI(TAG, "update started: slot %s, %u bytes declared",
             next->label, (unsigned)image_size_or_zero);
    ota_unlock();
    return ESP_OK;
}

esp_err_t ota_write_chunk(const void *data, size_t len)
{
    ota_lock();
    if (!s_in_progress || !s_target) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        set_result("flash write failed at byte %u: %s",
                   (unsigned)s_written, esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s_last_result);
        ota_unlock();
        return err;
    }

    s_written += len;
    if (s_total > 0) {
        s_progress_pct = (int)((s_written * 100) / s_total);
    }
    ota_unlock();
    return ESP_OK;
}

esp_err_t ota_end_and_reboot(void)
{
    ota_lock();
    if (!s_in_progress || !s_target) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    // esp_ota_end frees the handle on every outcome, so there is no
    // esp_ota_abort in the failure path below.
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            set_result("image validation failed: not a bootable app image "
                       "for this chip (wrong file?)");
        } else {
            set_result("esp_ota_end failed: %s", esp_err_to_name(err));
        }
        goto fail;
    }

    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        set_result("esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        goto fail;
    }

    // The new image's app descriptor is in flash now; read its version back
    // for the result webhook and the status line.
    esp_app_desc_t desc;
    const char *new_version = "unknown";
    if (esp_ota_get_partition_description(s_target, &desc) == ESP_OK) {
        new_version = desc.version;
    }
    set_result("ok: %s flashed to %s, rebooting", new_version, s_target->label);
    ESP_LOGI(TAG, "%s", s_last_result);

    s_in_progress    = false;
    s_target         = NULL;
    s_progress_pct   = 100;
    s_reboot_pending = true;   // no new update may start before the restart
    fire_result_webhook(true, new_version);
    ota_unlock();

    schedule_reboot();
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "%s", s_last_result);
    s_in_progress  = false;
    s_target       = NULL;
    s_progress_pct = -1;
    // No reboot is coming, so give the panel back. On the success path above
    // the freeze is deliberately left in place: the strip holds the notice
    // through the restart until the new image clears it.
    display_resume_after_update();
    fire_result_webhook(false, s_last_result);
    ota_unlock();
    return err;
}

void ota_abort_update(void)
{
    ota_lock();
    // Only the upload path owns s_handle/s_target; a URL-pull worker cleans
    // up after itself, so leave its state alone.
    if (s_in_progress && s_target) {
        esp_ota_abort(s_handle);
        // Keep whatever error set_result() already recorded; only note the
        // abort when the transfer died without a prior error message.
        if (strcmp(s_last_result, "in progress") == 0) {
            set_result("aborted before completion");
        }
        ESP_LOGW(TAG, "update aborted: %s", s_last_result);
        s_in_progress  = false;
        s_handle       = 0;
        s_target       = NULL;
        s_progress_pct = -1;
        display_resume_after_update();
    }
    ota_unlock();
}

// ---- URL pull path ---------------------------------------------------------

typedef struct {
    char url[OTA_URL_MAX_LEN + 1];
} url_ctx_t;

static void url_worker(void *arg)
{
    url_ctx_t *ctx = (url_ctx_t *)arg;
    char version[32] = "unknown";

    // Same reason as the upload path: esp_https_ota writes flash as the
    // download arrives, and a frame in flight while it does is what corrupts
    // the panel and cuts the supply out.
    display_freeze_for_update();

    esp_http_client_config_t http_cfg = {
        .url               = ctx->url,
        .crt_bundle_attach = esp_crt_bundle_attach,   // https: IDF cert bundle
        .timeout_ms        = OTA_HTTP_TIMEOUT_MS,
        // Sized for the REDIRECT target, not for the URL we were handed. A
        // GitHub release asset URL is ~80 characters and answers 302 to a
        // signed release-assets.githubusercontent.com URL whose query alone
        // (SAS signature plus a ~330-character jwt) runs to ~800 bytes; the
        // whole request line measured 880. esp_http_client formats the
        // request line into this buffer and fails with "Out of buffer" when
        // it does not fit, so anything smaller breaks every GitHub update.
        .buffer_size_tx    = OTA_TX_BUF_SIZE,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ota_lock();
        set_result("download failed to start: %s", esp_err_to_name(err));
        ota_unlock();
        goto done;
    }

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(handle, &desc) == ESP_OK) {
        snprintf(version, sizeof(version), "%s", desc.version);
    }

    // Incremental perform loop (instead of the one-shot esp_https_ota())
    // so download progress lands in ota_status_t for the web UI to poll.
    int total = esp_https_ota_get_image_size(handle);
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (total > 0) {
            int read = esp_https_ota_get_image_len_read(handle);
            ota_lock();
            s_progress_pct = (read * 100) / total;
            ota_unlock();
        }
    }

    if (err != ESP_OK) {
        ota_lock();
        set_result("download/flash failed: %s", esp_err_to_name(err));
        ota_unlock();
        esp_https_ota_abort(handle);
        goto done;
    }
    if (!esp_https_ota_is_complete_data_received(handle)) {
        ota_lock();
        set_result("connection dropped mid-download");
        ota_unlock();
        esp_https_ota_abort(handle);
        err = ESP_FAIL;
        goto done;
    }

    // Validates the image and sets the boot partition; frees the handle on
    // every outcome (no abort afterwards).
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ota_lock();
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            set_result("image validation failed: not a bootable app image "
                       "for this chip (wrong file?)");
        } else {
            set_result("esp_https_ota_finish failed: %s", esp_err_to_name(err));
        }
        ota_unlock();
    }

done:
    ota_lock();
    bool ok = (err == ESP_OK);
    if (ok) {
        s_progress_pct = 100;
        set_result("ok: %s downloaded and flashed, rebooting", version);
        ESP_LOGI(TAG, "%s", s_last_result);
        fire_result_webhook(true, version);
    } else {
        s_progress_pct = -1;
        ESP_LOGE(TAG, "%s", s_last_result);
        // Nothing is going to reboot, so the panel comes back. Left frozen on
        // the success path, so the notice survives into the restart.
        display_resume_after_update();
        fire_result_webhook(false, s_last_result);
    }
    s_in_progress = false;
    if (ok) {
        s_reboot_pending = true;   // block new updates until the restart
    }
    ota_unlock();

    free(ctx);
    if (ok) {
        schedule_reboot();   // same flush grace period as the upload path
    }
    vTaskDelete(NULL);
}

esp_err_t ota_from_url(const char *url)
{
    if (!url || url[0] == '\0' || strlen(url) > OTA_URL_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_lock();
    if (s_in_progress || s_reboot_pending) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    url_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }
    snprintf(ctx->url, sizeof(ctx->url), "%s", url);

    // Claim the state machine before the worker exists so a concurrent
    // upload cannot sneak in between unlock and task start.
    s_in_progress  = true;
    s_target       = NULL;   // URL path never touches s_handle/s_target
    s_handle       = 0;
    s_total        = 0;
    s_written      = 0;
    s_progress_pct = -1;
    set_result("in progress");

    if (xTaskCreate(url_worker, "ota_url", OTA_WORKER_STACK, ctx, 5, NULL) != pdPASS) {
        s_in_progress = false;
        set_result("could not start download task (out of memory)");
        ESP_LOGE(TAG, "%s", s_last_result);
        free(ctx);
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }

    // Log only scheme+host: query strings on artifact URLs often carry
    // access tokens that don't belong in the log buffer.
    char redacted[96];
    const char *path_start = strstr(ctx->url, "://");
    path_start = path_start ? strchr(path_start + 3, '/') : NULL;
    snprintf(redacted, sizeof(redacted), "%.*s",
             path_start ? (int)(path_start - ctx->url) : 80, ctx->url);
    ESP_LOGI(TAG, "URL update started: %s/...", redacted);
    ota_unlock();
    return ESP_OK;
}

// ---- status / rollback -----------------------------------------------------

void ota_get_status(ota_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    // FW_VERSION is the human-facing release number; the app descriptor
    // version (derived from CMake) is only a fallback if it is left empty.
    snprintf(out->running_version, sizeof(out->running_version), "%s", FW_VERSION);
    if (out->running_version[0] == '\0') {
        snprintf(out->running_version, sizeof(out->running_version), "%s",
                 esp_app_get_description()->version);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    snprintf(out->running_slot, sizeof(out->running_slot), "%s",
             running ? running->label : "unknown");

    ota_lock();
    snprintf(out->last_result, sizeof(out->last_result), "%s", s_last_result);
    out->in_progress  = s_in_progress;
    out->progress_pct = s_progress_pct;
    ota_unlock();
}

void ota_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        // Factory image or no otadata yet: nothing to confirm.
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "first boot after update: image on %s marked valid, "
                 "rollback cancelled (%s)", running->label, esp_err_to_name(err));
    }
}

// ---- GitHub release check ---------------------------------------------------
//
// GET https://api.github.com/repos/<CONFIG_LEDDISP_GITHUB_REPO>/releases/latest
// and offer the led_display.bin asset when its tag is newer than FW_VERSION.
// The response for a single release is a few KB; 16 KB leaves margin for long
// release notes without letting GitHub fill the heap.

#define GH_RESP_MAX   16384
#define GH_ASSET_NAME "led_display.bin"

// Compare dotted numeric versions ("2.1.0" vs "2.0.3"), ignoring a leading
// 'v' and any suffix after the numbers. >0 when a is newer than b.
static int version_cmp(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V') a++;
    if (*b == 'v' || *b == 'V') b++;
    for (int i = 0; i < 3; i++) {
        long na = strtol(a, (char **)&a, 10);
        long nb = strtol(b, (char **)&b, 10);
        if (na != nb) return na > nb ? 1 : -1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

esp_err_t ota_check_github(ota_check_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    snprintf(out->current, sizeof(out->current), "%s", FW_VERSION);

    esp_http_client_config_t cfg = {
        .url               = "https://api.github.com/repos/"
                             CONFIG_LEDDISP_GITHUB_REPO "/releases/latest",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
        .buffer_size       = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(out->error, sizeof(out->error), "http client init failed");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "User-Agent", "led-display");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    char *body = NULL;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(out->error, sizeof(out->error), "GitHub unreachable: %s",
                 esp_err_to_name(err));
        goto out;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        snprintf(out->error, sizeof(out->error),
                 "GitHub replied %d (repo private or no release yet?)", status);
        err = ESP_FAIL;
        goto out;
    }

    body = malloc(GH_RESP_MAX + 1);
    if (!body) {
        snprintf(out->error, sizeof(out->error), "out of memory");
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    int total = 0;
    while (total < GH_RESP_MAX) {
        int n = esp_http_client_read(client, body + total, GH_RESP_MAX - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        snprintf(out->error, sizeof(out->error), "unparseable GitHub response");
        err = ESP_FAIL;
        goto out;
    }

    const cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    if (cJSON_IsString(tag)) {
        const char *t = tag->valuestring;
        if (*t == 'v' || *t == 'V') t++;
        snprintf(out->latest, sizeof(out->latest), "%s", t);
    }
    const cJSON *assets = cJSON_GetObjectItem(root, "assets");
    const cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        const cJSON *name = cJSON_GetObjectItem(asset, "name");
        const cJSON *url  = cJSON_GetObjectItem(asset, "browser_download_url");
        if (cJSON_IsString(name) && cJSON_IsString(url) &&
            strcmp(name->valuestring, GH_ASSET_NAME) == 0) {
            snprintf(out->asset_url, sizeof(out->asset_url), "%s", url->valuestring);
            break;
        }
    }
    cJSON_Delete(root);

    if (out->latest[0] == '\0') {
        snprintf(out->error, sizeof(out->error), "release has no tag_name");
        err = ESP_FAIL;
    } else if (out->asset_url[0] == '\0') {
        snprintf(out->error, sizeof(out->error),
                 "release has no %s asset", GH_ASSET_NAME);
        err = ESP_FAIL;
    } else {
        out->update_available = version_cmp(out->latest, FW_VERSION) > 0;
        err = ESP_OK;
    }

out:
    free(body);
    esp_http_client_cleanup(client);
    return err;
}
