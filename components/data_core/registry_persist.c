#include "registry_persist.h"
#include "capability.h"
#include <string.h>

/* CRC-16/CCITT-FALSE, the same algorithm actor_persist.c and event_ring.c
 * use, so the whole firmware has one integrity primitive. */
static uint16_t crc16_update(uint16_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u)
                                             : (uint16_t)(crc << 1));
    }
    return crc;
}

/* Per-device fixed prefix: kind(1) addr(8) via_valid(1) via(6) rssi(1) cap_count(1). */
#define ROW_FIXED 18

size_t registry_persist_serialize(const registry_t *r, uint32_t written_epoch,
                                  uint8_t *buf, size_t cap)
{
    if (!r || !buf || cap < REGISTRY_PERSIST_HEADER_LEN) return 0;

    size_t off = REGISTRY_PERSIST_HEADER_LEN;
    uint8_t count = 0;
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        const device_entry_t *d = &r->devices[i];
        if (!d->in_use) continue;

        uint8_t ncap = 0;
        for (int c = 0; c < CAPABILITY_COUNT; c++) if (d->caps[c].valid) ncap++;
        if (off + ROW_FIXED + (size_t)ncap * 3 > cap) return 0;

        buf[off++] = d->id.kind;
        memcpy(&buf[off], d->id.addr, 8); off += 8;
        buf[off++] = d->via_node_valid ? 1 : 0;
        memcpy(&buf[off], d->via_node, 6); off += 6;
        buf[off++] = (uint8_t)d->best_rssi;
        buf[off++] = ncap;
        for (int c = 0; c < CAPABILITY_COUNT; c++) {
            if (!d->caps[c].valid) continue;
            buf[off++] = (uint8_t)c;
            uint16_t raw = (uint16_t)d->caps[c].raw;
            buf[off++] = (uint8_t)(raw & 0xFF);
            buf[off++] = (uint8_t)(raw >> 8);
        }
        count++;
    }

    buf[0] = REGISTRY_PERSIST_FMT;
    buf[3] = (uint8_t)(written_epoch & 0xFF);
    buf[4] = (uint8_t)((written_epoch >> 8) & 0xFF);
    buf[5] = (uint8_t)((written_epoch >> 16) & 0xFF);
    buf[6] = (uint8_t)((written_epoch >> 24) & 0xFF);
    buf[7] = count;

    /* CRC covers every byte except the crc field (buf[1..2]). */
    uint16_t crc = crc16_update(0xFFFFu, &buf[0], 1);
    crc = crc16_update(crc, &buf[3], off - 3);
    buf[1] = (uint8_t)(crc & 0xFF);
    buf[2] = (uint8_t)(crc >> 8);
    return off;
}

bool registry_persist_deserialize(const uint8_t *buf, size_t len,
                                  registry_t *out, uint32_t *epoch_out)
{
    if (out) registry_init(out);
    if (!buf || !out || len < REGISTRY_PERSIST_HEADER_LEN) return false;
    if (buf[0] != REGISTRY_PERSIST_FMT) return false;

    uint16_t stored = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
    uint16_t crc = crc16_update(0xFFFFu, &buf[0], 1);
    crc = crc16_update(crc, &buf[3], len - 3);
    if (crc != stored) return false;

    uint32_t epoch = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8) |
                     ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);
    uint8_t count = buf[7];
    if (count > REGISTRY_MAX_DEVICES) return false;

    size_t off = REGISTRY_PERSIST_HEADER_LEN;
    for (uint8_t i = 0; i < count; i++) {
        if (off + ROW_FIXED > len) { registry_init(out); return false; }
        device_entry_t *d = &out->devices[i];
        d->in_use = true;
        d->snapshot_only = true;      /* stale until first live report */
        d->last_seen_s = 0;
        d->id.kind = buf[off++];
        memcpy(d->id.addr, &buf[off], 8); off += 8;
        d->via_node_valid = buf[off++] != 0;
        memcpy(d->via_node, &buf[off], 6); off += 6;
        d->best_rssi = (int8_t)buf[off++];
        uint8_t ncap = buf[off++];
        if (off + (size_t)ncap * 3 > len) { registry_init(out); return false; }
        for (uint8_t k = 0; k < ncap; k++) {
            uint8_t cap_id = buf[off++];
            uint16_t raw = (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
            off += 2;
            if (cap_id >= CAPABILITY_COUNT) { registry_init(out); return false; }
            d->caps[cap_id].raw = (int16_t)raw;
            d->caps[cap_id].updated_s = 0;
            d->caps[cap_id].valid = true;
        }
    }

    if (epoch_out) *epoch_out = epoch;
    return true;
}

#ifdef ESP_PLATFORM
#include <stdio.h>
#include "esp_log.h"

static const char *TAG = "registry_persist";
#define REGISTRY_PERSIST_PATH     "/storage/registry_snap.bin"
#define REGISTRY_PERSIST_TMP_PATH "/storage/registry_snap.tmp"

/* Only the boot-time load reaches this (single-threaded, before tasks
 * start), so a module-static read buffer keeps ~1 KB off the caller's
 * stack. The save path serializes into the caller's own buffer. */
static uint8_t s_buf[REGISTRY_PERSIST_MAX_BYTES];

bool registry_persist_write(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return false;
    /* tmp + rename: rename() is atomic on LittleFS, so a power loss leaves
     * either the old snapshot or the new one, never a half-written file. */
    FILE *f = fopen(REGISTRY_PERSIST_TMP_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "open tmp failed"); return false; }
    bool ok = fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(REGISTRY_PERSIST_TMP_PATH); ESP_LOGW(TAG, "write failed"); return false; }
    if (rename(REGISTRY_PERSIST_TMP_PATH, REGISTRY_PERSIST_PATH) != 0) {
        remove(REGISTRY_PERSIST_TMP_PATH);
        ESP_LOGW(TAG, "rename failed");
        return false;
    }
    return true;
}

int registry_persist_load(registry_t *out, uint32_t *epoch_out)
{
    if (epoch_out) *epoch_out = 0;
    FILE *f = fopen(REGISTRY_PERSIST_PATH, "rb");
    if (!f) { if (out) registry_init(out); return -1; }   /* absent: first boot */
    size_t len = fread(s_buf, 1, sizeof s_buf, f);
    fclose(f);
    if (!registry_persist_deserialize(s_buf, len, out, epoch_out)) {
        ESP_LOGW(TAG, "snapshot unreadable/corrupt; ignoring");
        return -1;
    }
    int n = 0;
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) if (out->devices[i].in_use) n++;
    ESP_LOGI(TAG, "restored %d device(s) from snapshot", n);
    return n;
}
#endif
