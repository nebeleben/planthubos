#include "wrapper_bind.h"
#include <string.h>

static wrapper_bind_t s_tbl[WRAPPER_BIND_MAX];
static bool           s_used[WRAPPER_BIND_MAX];

static uint16_t crc16_update(uint16_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1));
    }
    return crc;
}

static int find(const uint8_t mac[6]) {
    for (int i = 0; i < WRAPPER_BIND_MAX; i++)
        if (s_used[i] && memcmp(s_tbl[i].mac, mac, 6) == 0) return i;
    return -1;
}

void wrapper_bind_reset(void) { memset(s_tbl, 0, sizeof s_tbl); memset(s_used, 0, sizeof s_used); }

bool wrapper_bind_set(const uint8_t mac[6], uint16_t wrapper_id) {
    if (wrapper_id == 0) return false;
    int i = find(mac);
    if (i < 0) { for (i = 0; i < WRAPPER_BIND_MAX && s_used[i]; i++) { } if (i >= WRAPPER_BIND_MAX) return false; }
    s_used[i] = true; memcpy(s_tbl[i].mac, mac, 6); s_tbl[i].wrapper_id = wrapper_id;
    return true;
}

bool wrapper_bind_clear(const uint8_t mac[6]) {
    int i = find(mac); if (i < 0) return false;
    s_used[i] = false; memset(&s_tbl[i], 0, sizeof s_tbl[i]); return true;
}

uint16_t wrapper_bind_lookup(const uint8_t mac[6]) { int i = find(mac); return i < 0 ? 0 : s_tbl[i].wrapper_id; }

size_t wrapper_bind_list(wrapper_bind_t *out, size_t max) {
    size_t n = 0;
    for (int i = 0; i < WRAPPER_BIND_MAX && n < max; i++) if (s_used[i]) out[n++] = s_tbl[i];
    return n;
}

size_t wrapper_bind_prune(bool (*exists)(uint16_t)) {
    size_t removed = 0;
    for (int i = 0; i < WRAPPER_BIND_MAX; i++)
        if (s_used[i] && !exists(s_tbl[i].wrapper_id)) { s_used[i] = false; memset(&s_tbl[i], 0, sizeof s_tbl[i]); removed++; }
    return removed;
}

size_t wrapper_bind_serialize(uint8_t *buf, size_t cap) {
    if (!buf || cap < 4) return 0;
    size_t off = 4; uint8_t count = 0;
    for (int i = 0; i < WRAPPER_BIND_MAX; i++) {
        if (!s_used[i]) continue;
        if (off + 8 > cap) return 0;
        memcpy(&buf[off], s_tbl[i].mac, 6); off += 6;
        buf[off++] = (uint8_t)(s_tbl[i].wrapper_id & 0xFF);
        buf[off++] = (uint8_t)(s_tbl[i].wrapper_id >> 8);
        count++;
    }
    buf[0] = WRAPPER_BIND_FMT; buf[3] = count;
    uint16_t crc = crc16_update(0xFFFFu, &buf[0], 1);
    crc = crc16_update(crc, &buf[3], off - 3);
    buf[1] = (uint8_t)(crc & 0xFF); buf[2] = (uint8_t)(crc >> 8);
    return off;
}

bool wrapper_bind_deserialize(const uint8_t *buf, size_t len) {
    wrapper_bind_reset();
    if (!buf || len < 4 || buf[0] != WRAPPER_BIND_FMT) return false;
    uint16_t stored = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
    uint16_t crc = crc16_update(0xFFFFu, &buf[0], 1);
    crc = crc16_update(crc, &buf[3], len - 3);
    if (crc != stored) return false;
    uint8_t count = buf[3];
    if (count > WRAPPER_BIND_MAX) return false;
    size_t off = 4;
    for (uint8_t k = 0; k < count; k++) {
        if (off + 8 > len) { wrapper_bind_reset(); return false; }
        s_used[k] = true;
        memcpy(s_tbl[k].mac, &buf[off], 6); off += 6;
        s_tbl[k].wrapper_id = (uint16_t)(buf[off] | ((uint16_t)buf[off+1] << 8)); off += 2;
    }
    return true;
}

#ifdef ESP_PLATFORM
#include <stdio.h>
#include "esp_log.h"
static const char *TAG = "wrapper_bind";
#define WBIND_PATH     "/storage/wrapper_binds.bin"
#define WBIND_TMP_PATH "/storage/wrapper_binds.tmp"
static uint8_t s_io_buf[4 + WRAPPER_BIND_MAX * 8];

void wrapper_bind_load(void) {
    FILE *f = fopen(WBIND_PATH, "rb");
    if (!f) { wrapper_bind_reset(); return; }
    size_t n = fread(s_io_buf, 1, sizeof s_io_buf, f);
    fclose(f);
    if (!wrapper_bind_deserialize(s_io_buf, n)) ESP_LOGW(TAG, "binding file unreadable; ignoring");
}

bool wrapper_bind_save(void) {
    size_t len = wrapper_bind_serialize(s_io_buf, sizeof s_io_buf);
    if (len == 0) return false;
    FILE *f = fopen(WBIND_TMP_PATH, "wb");
    if (!f) return false;
    bool ok = fwrite(s_io_buf, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(WBIND_TMP_PATH); return false; }
    if (rename(WBIND_TMP_PATH, WBIND_PATH) != 0) { remove(WBIND_TMP_PATH); return false; }
    return true;
}
#endif
