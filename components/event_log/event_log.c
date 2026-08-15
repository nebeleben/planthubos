#include "event_log.h"
#include "timekeeper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "event_log";
#define EVENTS_PATH "/storage/events.bin"

static event_t s_slots[EVENT_SLOTS];
static event_ring_t s_ring;
static SemaphoreHandle_t s_mutex;
static event_hook_t s_sse_hook;
static event_hook_t s_mqtt_hook;

void event_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    memset(s_slots, 0, sizeof(s_slots));
    FILE *f = fopen(EVENTS_PATH, "rb");
    if (f) {
        /* A missing or short file just leaves the tail zeroed (empty
         * slots, seq 0) -- fread returning short is not an error here. */
        fread(s_slots, sizeof(event_t), EVENT_SLOTS, f);
        fclose(f);
    }
    event_ring_init(&s_ring, s_slots);
    ESP_LOGI(TAG, "loaded, next_seq=%u", (unsigned)s_ring.next_seq);
}

uint32_t event_log_append(uint8_t level, uint32_t rule_id, const char *msg)
{
    uint32_t ts = timekeeper_now();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t seq = event_ring_append(&s_ring, ts, rule_id, level, msg);
    uint32_t idx = (seq - 1) % EVENT_SLOTS;
    event_t written = s_slots[idx];

    /* Persist only the single changed slot, not the whole file -- events
     * append frequently and EVENT_SLOTS * sizeof(event_t) is not free to
     * rewrite every time. */
    FILE *f = fopen(EVENTS_PATH, "r+b");
    if (!f) f = fopen(EVENTS_PATH, "w+b");
    if (f) {
        if (fseek(f, (long)(idx * sizeof(event_t)), SEEK_SET) == 0) {
            if (fwrite(&written, sizeof(event_t), 1, f) != 1) {
                ESP_LOGW(TAG, "event %u: short write", (unsigned)seq);
            } else {
                fflush(f);
            }
        }
        fclose(f);
    } else {
        ESP_LOGW(TAG, "event %u: failed to open %s for write", (unsigned)seq, EVENTS_PATH);
    }
    xSemaphoreGive(s_mutex);

    /* Hooks run outside the mutex: the SSE/MQTT hooks may block on network
     * I/O or take their own locks, and holding event_log's mutex across
     * that would serialize unrelated appends behind however long a push
     * takes. */
    if (s_sse_hook) s_sse_hook(&written);
    if (s_mqtt_hook) s_mqtt_hook(&written);

    return seq;
}

size_t event_log_read(uint32_t after, event_t *out, size_t max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = event_ring_read(&s_ring, after, out, max);
    xSemaphoreGive(s_mutex);
    return n;
}

uint32_t event_log_last_seq(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t last = s_ring.next_seq - 1;
    xSemaphoreGive(s_mutex);
    return last;
}

void event_log_set_hooks(event_hook_t sse_hook, event_hook_t mqtt_hook)
{
    s_sse_hook = sse_hook;
    s_mqtt_hook = mqtt_hook;
}
