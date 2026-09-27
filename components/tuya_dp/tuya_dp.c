#include "tuya_dp.h"
#include <math.h>
#include <string.h>

/* tuya_dp_observe() is called from the swarm dispatch task; tuya_dp_map_set/
 * clear() (and, transitively, save()) are called from the httpd/API task --
 * guard every RAM-table accessor with a spinlock so a mapping write racing
 * an observe (or a save reading mid-write) can't tear, exactly like
 * wrapper_bind.c. Host build (test_tuya_dp) is single-threaded, so these are
 * a no-op there. */
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define TDP_ENTER() taskENTER_CRITICAL(&s_lock)
#define TDP_EXIT()  taskEXIT_CRITICAL(&s_lock)
#else
#define TDP_ENTER() ((void)0)
#define TDP_EXIT()  ((void)0)
#endif

/* --- observed store --- */
typedef struct {
    bool          in_use;
    device_id_t   id;
    uint32_t      last_activity_s;   /* device-level LRU key */
    uint8_t       dp_count;
    tuya_dp_obs_t dps[TUYA_DP_MAX_PER_DEVICE];
} tuya_dp_device_t;

static tuya_dp_device_t s_devices[TUYA_DP_MAX_DEVICES];

/* --- mapping table --- */
static tuya_dp_map_t s_map[TUYA_MAP_MAX];
static bool          s_map_used[TUYA_MAP_MAX];

void tuya_dp_reset(void) { memset(s_devices, 0, sizeof s_devices); }

static int dev_find(const device_id_t *id) {
    for (int i = 0; i < TUYA_DP_MAX_DEVICES; i++)
        if (s_devices[i].in_use && device_id_equal(&s_devices[i].id, id)) return i;
    return -1;
}

/* Finds id's slot, claims a free one, or evicts the least-recently-active
 * device (lowest last_activity_s) when the table is full -- unlike
 * registry.c (never deletes), this store is a bounded cache of what's been
 * seen recently, so eviction here is the point. */
static int dev_find_or_evict(const device_id_t *id) {
    int idx = dev_find(id);
    if (idx >= 0) return idx;

    for (int i = 0; i < TUYA_DP_MAX_DEVICES; i++) {
        if (!s_devices[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        idx = 0;
        for (int i = 1; i < TUYA_DP_MAX_DEVICES; i++)
            if (s_devices[i].last_activity_s < s_devices[idx].last_activity_s) idx = i;
    }
    memset(&s_devices[idx], 0, sizeof s_devices[idx]);
    s_devices[idx].in_use = true;
    s_devices[idx].id = *id;
    return idx;
}

void tuya_dp_observe(const device_id_t *id, uint8_t dp_id, uint8_t type,
                      int32_t value, uint32_t now_s) {
    if (!id) return;
    TDP_ENTER();
    int di = dev_find_or_evict(id);
    tuya_dp_device_t *d = &s_devices[di];
    d->last_activity_s = now_s;

    int slot = -1;
    for (int i = 0; i < d->dp_count; i++)
        if (d->dps[i].dp_id == dp_id) { slot = i; break; }
    if (slot < 0) {
        if (d->dp_count < TUYA_DP_MAX_PER_DEVICE) {
            slot = d->dp_count++;
        } else {
            /* per-device DP cap reached: evict this device's oldest DP
             * (lowest updated_s) to make room for the newly-observed one. */
            slot = 0;
            for (int i = 1; i < TUYA_DP_MAX_PER_DEVICE; i++)
                if (d->dps[i].updated_s < d->dps[slot].updated_s) slot = i;
        }
    }
    d->dps[slot].dp_id = dp_id;
    d->dps[slot].type = type;
    d->dps[slot].value = value;
    d->dps[slot].updated_s = now_s;
    TDP_EXIT();
}

int tuya_dp_list(const device_id_t *id, tuya_dp_obs_t *out, int max) {
    if (!id || !out || max <= 0) return 0;
    TDP_ENTER();
    int di = dev_find(id);
    int n = 0;
    if (di >= 0) {
        tuya_dp_device_t *d = &s_devices[di];
        n = d->dp_count < max ? d->dp_count : max;
        memcpy(out, d->dps, (size_t)n * sizeof(tuya_dp_obs_t));
    }
    TDP_EXIT();
    return n;
}

/* --- mapping --- */

static int map_find(const device_id_t *id, uint8_t dp_id) {
    for (int i = 0; i < TUYA_MAP_MAX; i++)
        if (s_map_used[i] && s_map[i].dp_id == dp_id && device_id_equal(&s_map[i].id, id))
            return i;
    return -1;
}

bool tuya_dp_map_set(const device_id_t *id, uint8_t dp_id, uint8_t cap_id, float scale) {
    if (!id || cap_id >= CAPABILITY_COUNT || !isfinite(scale)) return false;
    bool ok = false;
    TDP_ENTER();
    int i = map_find(id, dp_id);
    if (i < 0) { for (i = 0; i < TUYA_MAP_MAX && s_map_used[i]; i++) { } }
    if (i < TUYA_MAP_MAX) {
        s_map_used[i] = true;
        s_map[i].id = *id;
        s_map[i].dp_id = dp_id;
        s_map[i].cap_id = cap_id;
        s_map[i].scale = scale;
        ok = true;
    }
    TDP_EXIT();
    return ok;
}

bool tuya_dp_map_clear(const device_id_t *id, uint8_t dp_id) {
    if (!id) return false;
    bool ok = false;
    TDP_ENTER();
    int i = map_find(id, dp_id);
    if (i >= 0) { s_map_used[i] = false; memset(&s_map[i], 0, sizeof s_map[i]); ok = true; }
    TDP_EXIT();
    return ok;
}

bool tuya_dp_map_get(const device_id_t *id, uint8_t dp_id, uint8_t *cap_id_out, float *scale_out) {
    if (!id) return false;
    TDP_ENTER();
    int i = map_find(id, dp_id);
    bool ok = i >= 0;
    if (ok) {
        if (cap_id_out) *cap_id_out = s_map[i].cap_id;
        if (scale_out) *scale_out = s_map[i].scale;
    }
    TDP_EXIT();
    return ok;
}

int tuya_dp_map_list(tuya_dp_map_t *out, int max) {
    if (!out || max <= 0) return 0;
    TDP_ENTER();
    int n = 0;
    for (int i = 0; i < TUYA_MAP_MAX && n < max; i++)
        if (s_map_used[i]) out[n++] = s_map[i];
    TDP_EXIT();
    return n;
}

float tuya_dp_apply(int32_t value, float scale) { return (float)value * scale; }

/* --- persistence --- */

static uint16_t crc16_update(uint16_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1));
    }
    return crc;
}

/* id.kind(1) + id.addr[8] + dp_id(1) + cap_id(1) + scale as 4 raw bytes */
#define TDP_MAP_ENTRY_SIZE (1 + 8 + 1 + 1 + 4)

/* Takes the lock too: tuya_dp_map_save() (ESP-only, below) calls this to
 * read s_map/s_map_used, and without it that read could tear against a
 * concurrent set/clear. Safe to nest with deserialize's own lock below
 * since the two are never called from within each other. */
size_t tuya_dp_map_serialize(uint8_t *buf, size_t cap) {
    if (!buf || cap < 4) return 0;
    TDP_ENTER();
    size_t off = 4; uint8_t count = 0;
    bool overflow = false;
    for (int i = 0; i < TUYA_MAP_MAX; i++) {
        if (!s_map_used[i]) continue;
        if (off + TDP_MAP_ENTRY_SIZE > cap) { overflow = true; break; }
        buf[off++] = s_map[i].id.kind;
        memcpy(&buf[off], s_map[i].id.addr, sizeof s_map[i].id.addr); off += sizeof s_map[i].id.addr;
        buf[off++] = s_map[i].dp_id;
        buf[off++] = s_map[i].cap_id;
        memcpy(&buf[off], &s_map[i].scale, sizeof(float)); off += sizeof(float);
        count++;
    }
    if (!overflow) {
        buf[0] = TUYA_DP_MAP_FMT; buf[3] = count;
        uint16_t crc = crc16_update(0xFFFFu, &buf[0], 1);
        crc = crc16_update(crc, &buf[3], off - 3);
        buf[1] = (uint8_t)(crc & 0xFF); buf[2] = (uint8_t)(crc >> 8);
    }
    TDP_EXIT();
    return overflow ? 0 : off;
}

static void map_reset_locked(void) {
    memset(s_map, 0, sizeof s_map);
    memset(s_map_used, 0, sizeof s_map_used);
}

bool tuya_dp_map_deserialize(const uint8_t *buf, size_t len) {
    TDP_ENTER();
    map_reset_locked();
    if (!buf || len < 4 || buf[0] != TUYA_DP_MAP_FMT) { TDP_EXIT(); return false; }
    uint16_t stored = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
    uint16_t crc = crc16_update(0xFFFFu, &buf[0], 1);
    crc = crc16_update(crc, &buf[3], len - 3);
    if (crc != stored) { TDP_EXIT(); return false; }
    uint8_t count = buf[3];
    if (count > TUYA_MAP_MAX) { TDP_EXIT(); return false; }

    bool ok = true;
    size_t off = 4;
    for (uint8_t k = 0; k < count && ok; k++) {
        if (off + TDP_MAP_ENTRY_SIZE > len) { ok = false; break; }
        s_map_used[k] = true;
        s_map[k].id.kind = buf[off++];
        memcpy(s_map[k].id.addr, &buf[off], sizeof s_map[k].id.addr); off += sizeof s_map[k].id.addr;
        s_map[k].dp_id = buf[off++];
        s_map[k].cap_id = buf[off++];
        memcpy(&s_map[k].scale, &buf[off], sizeof(float)); off += sizeof(float);
    }
    if (!ok) map_reset_locked();
    TDP_EXIT();
    return ok;
}

#ifdef ESP_PLATFORM
#include <stdio.h>
#include "esp_log.h"
static const char *TAG = "tuya_dp";
#define TDP_MAP_PATH     "/storage/tuya_map.bin"
#define TDP_MAP_TMP_PATH "/storage/tuya_map.tmp"
static uint8_t s_io_buf[4 + TUYA_MAP_MAX * TDP_MAP_ENTRY_SIZE];

void tuya_dp_map_load(void) {
    FILE *f = fopen(TDP_MAP_PATH, "rb");
    if (!f) { TDP_ENTER(); map_reset_locked(); TDP_EXIT(); return; }
    size_t n = fread(s_io_buf, 1, sizeof s_io_buf, f);
    fclose(f);
    if (!tuya_dp_map_deserialize(s_io_buf, n)) ESP_LOGW(TAG, "map file unreadable; ignoring");
}

bool tuya_dp_map_save(void) {
    size_t len = tuya_dp_map_serialize(s_io_buf, sizeof s_io_buf);
    if (len == 0) return false;
    FILE *f = fopen(TDP_MAP_TMP_PATH, "wb");
    if (!f) return false;
    bool ok = fwrite(s_io_buf, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(TDP_MAP_TMP_PATH); return false; }
    if (rename(TDP_MAP_TMP_PATH, TDP_MAP_PATH) != 0) { remove(TDP_MAP_TMP_PATH); return false; }
    return true;
}
#endif
