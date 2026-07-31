#include "timekeeper.h"
#include "boottab.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "timekeeper";

ESP_EVENT_DEFINE_BASE(PLANTHUB_TIME_EVENT);

static boottab_t s_tab;
static char s_tab_path[64];
static uint16_t s_boot_id;
static bool s_synced;
static bool s_sntp_started;
static SemaphoreHandle_t s_mutex;

static uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

/* Runs on the lwip/tcpip task (SNTP uses udp_recv there). Do not do the
 * boottab write here -- just post the epoch to the default event loop and
 * let timekeeper_init's handler (running on the event loop task) call
 * timekeeper_set_epoch, keeping any LittleFS I/O off the network task. */
static void on_sntp_sync(struct timeval *tv)
{
    uint32_t epoch = (uint32_t)tv->tv_sec;
    esp_err_t err = esp_event_post(PLANTHUB_TIME_EVENT, TIME_EVENT_EPOCH_LEARNED,
                                    &epoch, sizeof(epoch), 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "failed to post epoch-learned event: %s", esp_err_to_name(err));
}

static void on_epoch_learned(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    timekeeper_set_epoch(*(uint32_t *)data);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_sntp_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
        s_sntp_started = false;
    }
}

esp_err_t timekeeper_init(const char *base_path)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    snprintf(s_tab_path, sizeof(s_tab_path), "%s/boottab.bin", base_path);
    boottab_load(&s_tab, s_tab_path);

    nvs_handle_t h;
    esp_err_t err = nvs_open("planthub", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint16_t prev = 0;
    nvs_get_u16(h, "boot_id", &prev);
    s_boot_id = prev + 1;
    if (s_boot_id == 0xFFFF) s_boot_id = 1;   /* 0xFFFF is the empty-slot marker */
    err = nvs_set_u16(h, "boot_id", s_boot_id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "boot_id=%u, boottab entries=%u", s_boot_id, s_tab.count);
    err = esp_event_handler_register(PLANTHUB_TIME_EVENT, TIME_EVENT_EPOCH_LEARNED, on_epoch_learned, NULL);
    if (err != ESP_OK) return err;
    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
}

uint16_t timekeeper_boot_id(void) { return s_boot_id; }

void timekeeper_set_epoch(uint32_t epoch_s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_synced) {
        uint32_t up = uptime_s();
        uint32_t offset = epoch_s > up ? epoch_s - up : 0;
        if (offset && boottab_add(&s_tab, s_tab_path, s_boot_id, offset) == 0) {
            s_synced = true;
            ESP_LOGI(TAG, "time synced: epoch=%lu (offset %lu)",
                     (unsigned long)epoch_s, (unsigned long)offset);
        }
    }
    xSemaphoreGive(s_mutex);
}

bool timekeeper_resolve(uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = boottab_resolve(&s_tab, boot_id, rel_s, epoch_out);
    xSemaphoreGive(s_mutex);
    return ok;
}

bool timekeeper_synced(void) { return s_synced; }

uint32_t timekeeper_now(void)
{
    uint32_t epoch;
    if (!s_synced) return 0;
    return timekeeper_resolve(s_boot_id, uptime_s(), &epoch) ? epoch : 0;
}
