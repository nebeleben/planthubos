/* actor_persist.c -- the guard table across a reboot (M5b whole-branch
 * review, ruling FINAL-persist). See actor_persist.h for why this exists,
 * what it is keyed on and the write policy; this file's own split is the
 * one pending_close.c already established in this component:
 *
 *   - pure C99 serialise/deserialise/lookup, no ESP-IDF anywhere, linked
 *     directly into tests/host/test_actor_persist.c and proved by direct
 *     execution (the DECIDING parts -- which rows survive a merge, and
 *     what a restored timestamp means -- live in actor_table.c, also pure,
 *     also directly tested);
 *   - an `#ifdef ESP_PLATFORM` LittleFS wrapper: load at boot, re-apply on
 *     declare, tmp + rename on change, every call on adv_decoder_task.
 */
#include "actor_persist.h"
#include "actor.h"
#include "action.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
static const char *TAG = "actor_persist";
#endif

/* ---------------------------------------------------------------------
 * Pure: bytes on disk. No ESP-IDF below this line until the impure
 * section further down.
 * --------------------------------------------------------------------- */

/* CRC-16/CCITT-FALSE, the same algorithm event_ring.c's event_record_crc()
 * and pending_close.c both use -- one integrity primitive in this
 * firmware, not three subtly different ones. */
static uint16_t crc16_update(uint16_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u)
                                              : (uint16_t)(crc << 1));
        }
    }
    return crc;
}

static bool key_is_set(const uint8_t key[ACTOR_DEVICE_KEY_LEN])
{
    for (int i = 0; i < ACTOR_DEVICE_KEY_LEN; i++)
        if (key[i] != 0) return true;
    return false;
}

size_t actor_persist_serialize(const actor_guard_row_t *rows, size_t n,
                                uint8_t *buf, size_t cap)
{
    if (!buf) return 0;
    if (n > 0 && !rows) return 0;
    if (n > ACTOR_GUARD_ROWS_MAX) return 0;

    size_t total = ACTOR_PERSIST_HEADER_LEN + n * ACTOR_PERSIST_RECORD_LEN;
    if (total > cap) return 0;   /* no partial write */

    buf[0] = (uint8_t)ACTOR_PERSIST_FMT;
    buf[1] = (uint8_t)n;
    /* buf[2..3] (crc) filled last, once there are record bytes to cover. */

    size_t off = ACTOR_PERSIST_HEADER_LEN;
    for (size_t i = 0; i < n; i++) {
        memcpy(buf + off, rows[i].key, ACTOR_DEVICE_KEY_LEN);
        off += ACTOR_DEVICE_KEY_LEN;
        buf[off++] = rows[i].action_id;
        buf[off++] = rows[i].lockout ? 1u : 0u;
        buf[off++] = (uint8_t)(rows[i].cooldown_s & 0xFFu);
        buf[off++] = (uint8_t)((rows[i].cooldown_s >> 8) & 0xFFu);
        buf[off++] = rows[i].max_per_hour;
        buf[off++] = rows[i].window_count;
        buf[off++] = (uint8_t)(rows[i].window_start_s & 0xFFu);
        buf[off++] = (uint8_t)((rows[i].window_start_s >> 8) & 0xFFu);
        buf[off++] = (uint8_t)((rows[i].window_start_s >> 16) & 0xFFu);
        buf[off++] = (uint8_t)((rows[i].window_start_s >> 24) & 0xFFu);
        buf[off++] = (uint8_t)(rows[i].last_fire_s & 0xFFu);
        buf[off++] = (uint8_t)((rows[i].last_fire_s >> 8) & 0xFFu);
        buf[off++] = (uint8_t)((rows[i].last_fire_s >> 16) & 0xFFu);
        buf[off++] = (uint8_t)((rows[i].last_fire_s >> 24) & 0xFFu);
    }

    uint16_t crc = crc16_update(0xFFFFu, &buf[1], 1);
    crc = crc16_update(crc, &buf[ACTOR_PERSIST_HEADER_LEN],
                        total - ACTOR_PERSIST_HEADER_LEN);
    buf[2] = (uint8_t)(crc & 0xFFu);
    buf[3] = (uint8_t)((crc >> 8) & 0xFFu);

    return total;
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t actor_persist_deserialize(const uint8_t *buf, size_t len,
                                  actor_guard_row_t *out, size_t cap)
{
    if (!buf || len < ACTOR_PERSIST_HEADER_LEN) return 0;
    if (buf[0] != (uint8_t)ACTOR_PERSIST_FMT) return 0;

    uint8_t count = buf[1];
    uint16_t stored_crc = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));

    size_t total = ACTOR_PERSIST_HEADER_LEN + (size_t)count * ACTOR_PERSIST_RECORD_LEN;
    /* Exact match, not "at least": a truncated file and one with trailing
     * garbage are both refused outright, never read partially. */
    if (len != total) return 0;
    if (count > cap || count > ACTOR_GUARD_ROWS_MAX) return 0;

    uint16_t crc = crc16_update(0xFFFFu, &buf[1], 1);
    crc = crc16_update(crc, &buf[ACTOR_PERSIST_HEADER_LEN],
                        total - ACTOR_PERSIST_HEADER_LEN);
    if (crc != stored_crc) return 0;

    /* Decoded into a scratch row first, and only committed to *out once
     * EVERY record has passed: a file whose CRC happens to match but whose
     * contents this firmware cannot make sense of (an action id from a
     * newer vocabulary, a row with no key) must yield nothing rather than
     * the prefix that happened to parse. */
    size_t off = ACTOR_PERSIST_HEADER_LEN;
    for (uint8_t i = 0; i < count; i++) {
        actor_guard_row_t r;
        memset(&r, 0, sizeof r);
        memcpy(r.key, buf + off, ACTOR_DEVICE_KEY_LEN);
        off += ACTOR_DEVICE_KEY_LEN;
        r.action_id = buf[off++];
        r.lockout = buf[off++] != 0;
        r.cooldown_s = (uint16_t)(buf[off] | ((uint16_t)buf[off + 1] << 8));
        off += 2;
        r.max_per_hour = buf[off++];
        r.window_count = buf[off++];
        r.window_start_s = rd_u32le(buf + off); off += 4;
        r.last_fire_s = rd_u32le(buf + off); off += 4;

        if (action_get(r.action_id) == NULL) return 0;
        if (!key_is_set(r.key)) return 0;

        if (out) out[i] = r;
    }
    return count;
}

int actor_persist_find(const actor_guard_row_t *rows, size_t n,
                        const uint8_t key[ACTOR_DEVICE_KEY_LEN], size_t from)
{
    if (!rows || !key) return -1;
    for (size_t i = from; i < n; i++) {
        if (memcmp(rows[i].key, key, ACTOR_DEVICE_KEY_LEN) == 0) return (int)i;
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * Impure wrapper -- LittleFS and the in-RAM image. Gated exactly like
 * pending_close.c's own; the whole file still compiles (though this half
 * is unreachable) under the host test's plain `cc`.
 * --------------------------------------------------------------------- */

#ifdef ESP_PLATFORM

#define ACTOR_PERSIST_PATH     "/storage/actor_guards.bin"
#define ACTOR_PERSIST_TMP_PATH "/storage/actor_guards.tmp"

/* The image last written to (or read from) flash. 16 rows x 24 B = 384 B
 * of static RAM. Held rather than re-read per save because it is also what
 * makes rule 3 of actor_table_guard_merge() work: the guards of a device
 * that has not advertised since this boot are not in the live table, so
 * without this they would be erased from flash the first time any OTHER
 * device fired. */
static actor_guard_row_t s_img[ACTOR_GUARD_ROWS_MAX];
static size_t            s_img_n;

static void actor_persist_save(void)
{
    uint8_t buf[ACTOR_PERSIST_BUF_MAX];
    size_t len = actor_persist_serialize(s_img, s_img_n, buf, sizeof buf);

    if (s_img_n == 0) {
        /* Nothing to remember. remove()'s only failure here is ENOENT. */
        remove(ACTOR_PERSIST_PATH);
        return;
    }
    if (len == 0) {
        ESP_LOGE(TAG, "serialize failed for %u guard row(s); not persisted",
                 (unsigned)s_img_n);
        return;
    }

    /* tmp + rename, the discipline plants.c and pending_close.c both use:
     * rename() is atomic on LittleFS, so a power loss leaves either the
     * old complete file or the new one, never a half-written guard table.
     * fopen("wb") on the real path would truncate first and lose every
     * device's guards on a bad moment. */
    FILE *f = fopen(ACTOR_PERSIST_TMP_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "could not open %s for write (errno=%d); a reboot now would "
                       "lose %u guard row(s)", ACTOR_PERSIST_TMP_PATH, errno,
                 (unsigned)s_img_n);
        return;
    }
    size_t wrote = fwrite(buf, 1, len, f);
    if (wrote != len) {
        ESP_LOGW(TAG, "short write to %s persisting %u guard row(s)",
                 ACTOR_PERSIST_TMP_PATH, (unsigned)s_img_n);
        fclose(f);
        remove(ACTOR_PERSIST_TMP_PATH);
        return;
    }
    if (fflush(f) != 0) {
        ESP_LOGW(TAG, "fflush(%s) failed (errno=%d)", ACTOR_PERSIST_TMP_PATH, errno);
        fclose(f);
        remove(ACTOR_PERSIST_TMP_PATH);
        return;
    }
    if (fclose(f) != 0) {
        ESP_LOGW(TAG, "fclose(%s) failed (errno=%d)", ACTOR_PERSIST_TMP_PATH, errno);
        remove(ACTOR_PERSIST_TMP_PATH);
        return;
    }
    if (rename(ACTOR_PERSIST_TMP_PATH, ACTOR_PERSIST_PATH) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (errno=%d); a reboot now would lose "
                      "%u guard row(s)", ACTOR_PERSIST_TMP_PATH, ACTOR_PERSIST_PATH,
                 errno, (unsigned)s_img_n);
        remove(ACTOR_PERSIST_TMP_PATH);
    }
}

void actor_persist_init(void)
{
    s_img_n = 0;
    memset(s_img, 0, sizeof s_img);

    uint8_t buf[ACTOR_PERSIST_BUF_MAX];
    FILE *f = fopen(ACTOR_PERSIST_PATH, "rb");
    if (!f) {
        if (errno != ENOENT) {
            ESP_LOGW(TAG, "%s: open for read failed (errno=%d); guards start "
                          "unconfigured this run", ACTOR_PERSIST_PATH, errno);
        }
        return;
    }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    s_img_n = actor_persist_deserialize(buf, n, s_img, ACTOR_GUARD_ROWS_MAX);
    if (s_img_n > 0) {
        ESP_LOGI(TAG, "%u guard row(s) restored from before this boot; each is "
                      "re-applied when its device is next seen", (unsigned)s_img_n);
    } else if (n > 0) {
        /* Read something and could make nothing of it: loud, because the
         * operator's stop button is in that file. */
        ESP_LOGW(TAG, "%s: %u byte(s) unreadable (bad format, length or crc); "
                      "guards start unconfigured this run",
                 ACTOR_PERSIST_PATH, (unsigned)n);
        memset(s_img, 0, sizeof s_img);
    }
}

void actor_persist_sync(void)
{
    /* Peeked, not taken: this makes the IMAGE current, it does not
     * discharge the obligation to put it on flash. actor_persist_service()
     * still sees the flag on its next pass, merges again (idempotent) and
     * writes. Splitting it this way keeps every LittleFS access for this
     * file on the one call site at the top of the decoder loop, rather
     * than adding one nested under decode_adv_item(). */
    if (!actor_guards_dirty()) return;
    s_img_n = actor_guards_merge(s_img, s_img_n, ACTOR_GUARD_ROWS_MAX);
}

void actor_persist_restore_device(int dev_idx, const uint8_t key[ACTOR_DEVICE_KEY_LEN])
{
    if (dev_idx < 0 || key == NULL) return;

    unsigned applied = 0;
    int at = actor_persist_find(s_img, s_img_n, key, 0);
    while (at >= 0) {
        /* Fails harmlessly for a row naming an action this device no
         * longer declares -- the row is dropped from the file by rule 1 of
         * actor_table_guard_merge() on the next save. */
        if (actor_guards_apply(dev_idx, &s_img[at])) applied++;
        at = actor_persist_find(s_img, s_img_n, key, (size_t)at + 1);
    }
    if (applied > 0) {
        ESP_LOGI(TAG, "device %d: %u guard row(s) restored (lockout, cooldown, "
                      "hourly cap and the budget already spent)", dev_idx, applied);
    }
}

void actor_persist_service(void)
{
    /* Cleared BEFORE the snapshot, so a change landing between the two is
     * captured by this pass anyway, and one landing after it re-sets the
     * flag for the next pass. Never the other way round, which could drop
     * an update. */
    if (!actor_guards_take_dirty()) return;

    s_img_n = actor_guards_merge(s_img, s_img_n, ACTOR_GUARD_ROWS_MAX);
    actor_persist_save();
}

#endif /* ESP_PLATFORM */
