#include "integr_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "integr_config";
#define NS "planthub"
#define KEY "integr"
#define INTEGR_CONFIG_FORMAT 1

/* On-disk shape: a leading format byte (for future migrations, same idea as
 * swarm_store's blobs) followed by the config verbatim. All fields of
 * integr_config_t are bool/char[] (1-byte alignment throughout), so this
 * wrapper is naturally packed on every toolchain this project builds with;
 * __attribute__((packed)) makes that explicit rather than incidental. */
typedef struct __attribute__((packed)) {
    uint8_t format;
    integr_config_t cfg;
} integr_blob_t;

static integr_config_t s_cfg;
static SemaphoreHandle_t s_mutex;

static bool nul_terminated(const char *buf, size_t len)
{
    return memchr(buf, '\0', len) != NULL;
}

/* org/bucket end up unescaped in a URL query string when influx.c builds
 * the write request -- restricting them to a safe charset up front is
 * simpler and just as correct as escaping later. */
static bool charset_ok(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok) return false;
    }
    return true;
}

/* token ends up unescaped in an HTTP header value, so unlike org/bucket the
 * rule can't be a narrow charset -- a real InfluxDB v2 token is base64 and
 * routinely contains '/', '+', and a trailing '=' (often "=="), all of
 * which the old charset_ok() rejected, making every genuine token invalid.
 * The actual header-injection risk is CR/LF (or any other control
 * character) letting the token value inject extra header lines, so the
 * rule here is just: printable ASCII, no control characters. */
static bool token_charset_ok(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) return false;
    }
    return true;
}

esp_err_t integr_config_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    memset(&s_cfg, 0, sizeof(s_cfg));  /* off-by-default: enabled=false, all strings "" */

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* Namespace not created yet (fresh device, or app_config_init()'s
         * NVS init is the only thing that has ever touched it) -- defaults
         * are already in place above. Not an error. */
        return ESP_OK;
    }

    integr_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, KEY, &blob, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Fresh install: quiet, defaults stand. */
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "integr config blob read failed (%s); using defaults", esp_err_to_name(err));
    } else if (len != sizeof(blob) || blob.format != INTEGR_CONFIG_FORMAT) {
        ESP_LOGW(TAG, "integr config blob has unexpected size %d (expected %d) or format %u "
                      "(expected %u); using defaults",
                 (int)len, (int)sizeof(blob), blob.format, (unsigned)INTEGR_CONFIG_FORMAT);
    } else {
        s_cfg = blob.cfg;
    }
    nvs_close(h);
    return ESP_OK;
}

void integr_config_get(integr_config_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

esp_err_t integr_config_set(const integr_config_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;

    /* Every string field must be NUL-terminated within its own buffer,
     * regardless of which section is enabled -- a non-terminated field
     * would otherwise get persisted and later read back (or logged, or
     * used to build a request) as if it were a valid C string. */
    if (!nul_terminated(c->mqtt.uri, sizeof(c->mqtt.uri)) ||
        !nul_terminated(c->mqtt.user, sizeof(c->mqtt.user)) ||
        !nul_terminated(c->mqtt.pass, sizeof(c->mqtt.pass)) ||
        !nul_terminated(c->influx.url, sizeof(c->influx.url)) ||
        !nul_terminated(c->influx.org, sizeof(c->influx.org)) ||
        !nul_terminated(c->influx.bucket, sizeof(c->influx.bucket)) ||
        !nul_terminated(c->influx.token, sizeof(c->influx.token))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (c->mqtt.enabled && strncmp(c->mqtt.uri, "mqtt://", 7) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (c->influx.enabled) {
        bool url_ok = strncmp(c->influx.url, "http://", 7) == 0;
#ifdef CONFIG_PLANTHUB_INFLUX_TLS
        if (!url_ok) url_ok = strncmp(c->influx.url, "https://", 8) == 0;
#endif
        if (!url_ok || c->influx.org[0] == '\0' || c->influx.bucket[0] == '\0' ||
            c->influx.token[0] == '\0' ||
            !charset_ok(c->influx.org) || !charset_ok(c->influx.bucket) ||
            !token_charset_ok(c->influx.token)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    /* Disabled sections are stored as-is (no format/content checks beyond
     * the NUL-termination rule above) -- users can save credentials before
     * flipping enabled on. */

    integr_blob_t blob = { .format = INTEGR_CONFIG_FORMAT, .cfg = *c };

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "integr_config_set: NVS write failed (%s); RAM cache unchanged", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = *c;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
