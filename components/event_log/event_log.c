#include "event_log.h"
#include "timekeeper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <errno.h>
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
    if (!s_mutex) {
        /* Out of memory this early is effectively fatal for the device as
         * a whole, but there's no reason event_log itself should crash --
         * every mutex use below is guarded, so callers just get an
         * unprotected, RAM-only ring instead of a NULL-handle crash. */
        ESP_LOGE(TAG, "failed to create mutex; event persistence disabled");
    }

    memset(s_slots, 0, sizeof(s_slots));
    FILE *f = fopen(EVENTS_PATH, "rb");
    if (f) {
        /* A missing or short file just leaves the tail zeroed (empty
         * slots, seq 0) -- fread returning short is not an error here. */
        fread(s_slots, sizeof(event_t), EVENT_SLOTS, f);
        fclose(f);
    }
    /* A torn write (unclean power loss mid-record) can leave a slot with
     * garbage -- including a bogus seq that event_ring_init would
     * otherwise trust and use to desync the whole ring's numbering. Scrub
     * before init. */
    event_ring_sanitize(s_slots);
    event_ring_init(&s_ring, s_slots);
    ESP_LOGI(TAG, "loaded, next_seq=%u", (unsigned)s_ring.next_seq);
}

uint32_t event_log_append(uint8_t level, uint32_t rule_id, const char *msg)
{
    uint32_t ts = timekeeper_now();

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t seq = event_ring_append(&s_ring, ts, rule_id, level, msg);
    uint32_t idx = (seq - 1) % EVENT_SLOTS;
    event_t written = s_slots[idx];

    if (s_mutex) {
        /* Persist only the single changed slot, not the whole file --
         * events append frequently and EVENT_SLOTS * sizeof(event_t) is
         * not free to rewrite every time.
         *
         * Only fall back to creating the file (w+b, which truncates) when
         * it genuinely doesn't exist yet (ENOENT) -- any other fopen
         * failure (e.g. a transient VFS error) skips persisting this
         * append rather than truncating away already-durable history; the
         * RAM ring (and thus event_log_read()) stays correct either way,
         * just un-persisted for this one event.
         *
         * Because seq assigns idx = (seq-1) % EVENT_SLOTS sequentially
         * (0,1,2,...,EVENT_SLOTS-1,0,1,...), the ENOENT/create path can
         * now only ever be taken on the very first append ever made
         * (idx 0) -- every append after that finds an existing file whose
         * size already covers its target offset (append grows the file by
         * exactly one record each time, and a wrap rewrites an offset
         * already inside the file). So there's no sparse/short-file gap
         * to worry about here. */
        FILE *f = fopen(EVENTS_PATH, "r+b");
        if (!f && errno == ENOENT) f = fopen(EVENTS_PATH, "w+b");
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
            ESP_LOGW(TAG, "event %u: failed to open %s for write (errno=%d)",
                     (unsigned)seq, EVENTS_PATH, errno);
        }
    }
    if (s_mutex) xSemaphoreGive(s_mutex);

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
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = event_ring_read(&s_ring, after, out, max);
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
}

uint32_t event_log_last_seq(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t last = s_ring.next_seq - 1;
    if (s_mutex) xSemaphoreGive(s_mutex);
    return last;
}

void event_log_set_hooks(event_hook_t sse_hook, event_hook_t mqtt_hook)
{
    s_sse_hook = sse_hook;
    s_mqtt_hook = mqtt_hook;
}
