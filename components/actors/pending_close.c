/* pending_close.c -- persistence and boot replay for a hub-owned actuator
 * close (M5b Task 9). See actor.h's "Pending close" section for the full
 * contract; this file's own top-level split is the same one actor.c
 * already established for this component:
 *
 *   - a small RAM table plus pure C99 operations on it (arm/clear/due/
 *     retry/boot_load/serialize/deserialize) -- no ESP-IDF anywhere in this
 *     half, so tests/host/test_pending_close.c links this file directly
 *     with plain `cc` and proves it by direct execution;
 *   - an impure wrapper (pending_close_init()/pending_close_service()/
 *     pending_close_note_result(), plus the LittleFS read/write) that is
 *     `#ifdef ESP_PLATFORM`-gated, exactly like actor_request()'s
 *     alert_post()/dispatch-hook calls are.
 *
 * Deliberately does NOT persist on every retry -- only pending_close_arm()
 * (a fresh obligation) and pending_close_clear() (confirmed done, or given
 * up) touch flash. Retries/backoff live in RAM only; a reboot mid-retry
 * just restarts the retry count against the SAME on-disk deadline, which is
 * harmless because boot replay ignores that deadline anyway (see
 * pending_close_is_boot_due()). */
#include "actor.h"
#include "alert.h"
#include "event_log.h" /* EVENT_LEVEL_CRITICAL, used in the gated section below --
                         * same reason actor.c includes this directly rather than
                         * relying on a transitive pull-in. */

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
static const char *TAG = "pending_close";
/* Forward declaration: defined in the impure section further down (it
 * needs pending_close_serialize(), which is pure and comes later in this
 * file), but called from pending_close_arm()/pending_close_clear() above
 * it. */
static void pending_close_save(void);
#endif

/* ---------------------------------------------------------------------
 * Pure RAM table -- no ESP-IDF below this line until the impure section.
 * --------------------------------------------------------------------- */

static pending_close_t s_table[PENDING_CLOSE_MAX];

static int table_find(int dev_idx)
{
    for (int i = 0; i < PENDING_CLOSE_MAX; i++)
        if (s_table[i].dev_idx == (int8_t)dev_idx) return i;
    return -1;
}

static void table_upsert(int dev_idx, uint8_t close_action, uint32_t deadline_s, uint8_t retries)
{
    int i = table_find(dev_idx);
    if (i < 0) i = table_find(-1); /* first free row */
    if (i < 0) return;             /* table full: see actor.h's comment on
                                     * PENDING_CLOSE_MAX == ACTOR_MAX_DEVICES
                                     * -- unreachable given that cap, but a
                                     * dropped obligation must never silently
                                     * clobber an existing row */

    s_table[i].dev_idx = (int8_t)dev_idx;
    s_table[i].close_action = close_action;
    s_table[i].deadline_s = deadline_s;
    s_table[i].retries = retries;
}

static void table_remove(int dev_idx)
{
    int i = table_find(dev_idx);
    if (i < 0) return;
    s_table[i].dev_idx = -1;
    s_table[i].close_action = 0;
    s_table[i].deadline_s = 0;
    s_table[i].retries = 0;
}

static void table_reset(void)
{
    for (int i = 0; i < PENDING_CLOSE_MAX; i++) {
        s_table[i].dev_idx = -1;
        s_table[i].close_action = 0;
        s_table[i].deadline_s = 0;
        s_table[i].retries = 0;
    }
}

/* Bounded exponential backoff: 5s, 10s, 20s, 40s, 80s, capped at 300s (5
 * min). Generous enough that a peripheral advertising every few seconds is
 * rediscovered (and so declared as an actuator again, see
 * actor_table_add()) well within one step; short enough that
 * PENDING_CLOSE_MAX_RETRIES exhausts in well under half an hour rather than
 * leaving an operator waiting on an alert that may never come. */
static uint32_t backoff_s(uint8_t retries)
{
    uint32_t d = 5u;
    for (uint8_t i = 0; i < retries && d < 300u; i++) d *= 2u;
    return d > 300u ? 300u : d;
}

void pending_close_arm(int dev_idx, uint8_t close_action, uint32_t deadline_s)
{
    if (dev_idx < 0) return;
    table_upsert(dev_idx, close_action, deadline_s, 0);
#ifdef ESP_PLATFORM
    pending_close_save();
#endif
}

void pending_close_clear(int dev_idx)
{
    if (dev_idx < 0) return;
    table_remove(dev_idx);
#ifdef ESP_PLATFORM
    pending_close_save();
#endif
}

int pending_close_due(uint32_t now_s, pending_close_t *out)
{
    int best = -1;
    for (int i = 0; i < PENDING_CLOSE_MAX; i++) {
        if (s_table[i].dev_idx < 0) continue;
        if (s_table[i].deadline_s > now_s) continue;
        if (best < 0 || s_table[i].deadline_s < s_table[best].deadline_s) best = i;
    }
    if (best < 0) return 0;
    if (out) *out = s_table[best];
    return 1;
}

bool pending_close_retry(int dev_idx, uint32_t now_s)
{
    int i = table_find(dev_idx);
    if (i < 0) return false;
    if (s_table[i].retries >= PENDING_CLOSE_MAX_RETRIES) return false;
    s_table[i].retries++;
    s_table[i].deadline_s = now_s + backoff_s(s_table[i].retries);
    return true;
}

bool pending_close_is_boot_due(const pending_close_t *r)
{
    return r != NULL && r->dev_idx >= 0;
}

void pending_close_boot_load(const pending_close_t *recs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!pending_close_is_boot_due(&recs[i])) continue;
        /* deadline_s=0 -- actor_now_s() is already > 0 shortly after boot
         * (actor.h's own doc comment on that clock), so this is due on the
         * very first pending_close_due() call regardless of what the file
         * recorded, per this module's boot-replay contract. */
        table_upsert(recs[i].dev_idx, recs[i].close_action, 0, recs[i].retries);
    }
}

size_t pending_close_active_count(void)
{
    size_t n = 0;
    for (int i = 0; i < PENDING_CLOSE_MAX; i++)
        if (s_table[i].dev_idx >= 0) n++;
    return n;
}

/* ---------------------------------------------------------------------
 * Serialisation: pure, wire format `{ u8 fmt=1; u8 count; u16 crc }` then
 * `count` x 7-byte records (dev_idx, close_action, deadline_s LE32,
 * retries). CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), same algorithm
 * event_ring.c's event_record_crc() uses, covering `count` and every
 * record byte (not `fmt`, not the crc field itself).
 * --------------------------------------------------------------------- */

#define PENDING_CLOSE_HEADER_LEN 4u
#define PENDING_CLOSE_RECORD_LEN 7u
#define PENDING_CLOSE_FMT        1u

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

size_t pending_close_serialize(const pending_close_t *recs, size_t n, uint8_t *buf, size_t cap)
{
    if (!buf) return 0;
    if (n > 0 && !recs) return 0;
    if (n > 0xFFu) return 0; /* count is a single wire byte */

    size_t total = PENDING_CLOSE_HEADER_LEN + n * PENDING_CLOSE_RECORD_LEN;
    if (total > cap) return 0; /* no partial write */

    buf[0] = PENDING_CLOSE_FMT;
    buf[1] = (uint8_t)n;
    /* buf[2..3] (crc) filled last, once the record bytes exist to cover. */

    size_t off = PENDING_CLOSE_HEADER_LEN;
    for (size_t i = 0; i < n; i++) {
        buf[off++] = (uint8_t)recs[i].dev_idx;
        buf[off++] = recs[i].close_action;
        buf[off++] = (uint8_t)(recs[i].deadline_s & 0xFFu);
        buf[off++] = (uint8_t)((recs[i].deadline_s >> 8) & 0xFFu);
        buf[off++] = (uint8_t)((recs[i].deadline_s >> 16) & 0xFFu);
        buf[off++] = (uint8_t)((recs[i].deadline_s >> 24) & 0xFFu);
        buf[off++] = recs[i].retries;
    }

    uint16_t crc = crc16_update(0xFFFFu, &buf[1], 1);
    crc = crc16_update(crc, &buf[PENDING_CLOSE_HEADER_LEN], total - PENDING_CLOSE_HEADER_LEN);
    buf[2] = (uint8_t)(crc & 0xFFu);
    buf[3] = (uint8_t)((crc >> 8) & 0xFFu);

    return total;
}

size_t pending_close_deserialize(const uint8_t *buf, size_t len, pending_close_t *out, size_t cap)
{
    if (!buf || len < PENDING_CLOSE_HEADER_LEN) return 0;
    if (buf[0] != PENDING_CLOSE_FMT) return 0;

    uint8_t count = buf[1];
    uint16_t stored_crc = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));

    size_t total = PENDING_CLOSE_HEADER_LEN + (size_t)count * PENDING_CLOSE_RECORD_LEN;
    /* Exact match, not "at least": a truncated file and a file with
     * trailing garbage are both rejected outright, never read partially. */
    if (len != total) return 0;
    if (count > cap) return 0;

    uint16_t crc = crc16_update(0xFFFFu, &buf[1], 1);
    crc = crc16_update(crc, &buf[PENDING_CLOSE_HEADER_LEN], total - PENDING_CLOSE_HEADER_LEN);
    if (crc != stored_crc) return 0;

    size_t off = PENDING_CLOSE_HEADER_LEN;
    for (uint8_t i = 0; i < count; i++) {
        pending_close_t r;
        r.dev_idx = (int8_t)buf[off++];
        r.close_action = buf[off++];
        uint32_t d = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
                     ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
        off += 4;
        r.deadline_s = d;
        r.retries = buf[off++];
        if (out) out[i] = r;
    }
    return count;
}

/* ---------------------------------------------------------------------
 * Impure wrapper -- LittleFS, actor_request(), alert_post(). Gated exactly
 * like actor.c's own alert_post()/dispatch-hook calls; the whole file still
 * compiles (though this half is unreachable) under plain `cc`.
 * --------------------------------------------------------------------- */

#ifdef ESP_PLATFORM

#define PENDING_CLOSE_PATH        "/storage/pending_close.bin"
#define PENDING_CLOSE_ATTEMPT_TTL 30u /* actor_request() deadline for a
                                        * single dispatch attempt to reach
                                        * the queue -- not this obligation's
                                        * own retry policy, which is
                                        * pending_close_retry()'s job */

static void pending_close_save(void)
{
    pending_close_t recs[PENDING_CLOSE_MAX];
    size_t n = 0;
    for (int i = 0; i < PENDING_CLOSE_MAX; i++)
        if (s_table[i].dev_idx >= 0) recs[n++] = s_table[i];

    if (n == 0) {
        /* remove()'s only failure mode here is ENOENT (nothing to delete),
         * which is not worth logging -- an obligation-free hub that has
         * never armed one, or that just cleared its last one twice in a
         * row (defensive, cannot happen via this file's own callers), both
         * land here silently. */
        remove(PENDING_CLOSE_PATH);
        return;
    }

    uint8_t buf[PENDING_CLOSE_HEADER_LEN + PENDING_CLOSE_MAX * PENDING_CLOSE_RECORD_LEN];
    size_t len = pending_close_serialize(recs, n, buf, sizeof buf);
    if (len == 0) {
        /* Unreachable given n <= PENDING_CLOSE_MAX and buf sized for
         * exactly that many records, but a safety obligation must never
         * silently vanish -- loud rather than a "cannot happen" comment
         * alone. */
        ESP_LOGE(TAG, "serialize failed for %u pending close(s); not persisted", (unsigned)n);
        return;
    }

    FILE *f = fopen(PENDING_CLOSE_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "could not open %s for write (errno=%d); a reboot now "
                       "would lose %u pending close obligation(s)",
                 PENDING_CLOSE_PATH, errno, (unsigned)n);
        return;
    }
    size_t wrote = fwrite(buf, 1, len, f);
    fclose(f);
    if (wrote != len) {
        ESP_LOGW(TAG, "short write persisting %u pending close obligation(s)", (unsigned)n);
    }
}

void pending_close_service(void)
{
    uint32_t now = actor_now_s();
    pending_close_t rec;
    while (pending_close_due(now, &rec)) {
        /* ACTOR_SRC_SAFETY: exempt from lockout/cooldown/rate (actor_table.h),
         * still bound by the parameter bound and by "unknown device" -- both
         * of those simply make this attempt fail, which pending_close_retry()
         * below treats exactly like any other failed attempt. */
        actor_request(rec.dev_idx, rec.close_action, 0, ACTOR_SRC_SAFETY,
                       now + PENDING_CLOSE_ATTEMPT_TTL);

        if (!pending_close_retry(rec.dev_idx, now)) {
            /* Retries exhausted. The engine can only ever try; this is the
             * honest end of that path -- loud, not a silent give-up. */
            alert_post(EVENT_LEVEL_CRITICAL, ALERT_CODE_CLOSE_UNCONFIRMED,
                       rec.dev_idx, rec.close_action, rec.retries);
            pending_close_clear(rec.dev_idx);
        }
    }
}

#endif /* ESP_PLATFORM */

/* Unconditional: table_reset() (pure) must run on every build, including
 * the host test, which calls this as its per-test reset hook (see
 * test_pending_close.c's top comment) -- only the LittleFS read and the
 * immediate replay attempt below are target-only. */
void pending_close_init(void)
{
    table_reset();
#ifdef ESP_PLATFORM
    uint8_t buf[PENDING_CLOSE_HEADER_LEN + PENDING_CLOSE_MAX * PENDING_CLOSE_RECORD_LEN];
    FILE *f = fopen(PENDING_CLOSE_PATH, "rb");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        pending_close_t recs[PENDING_CLOSE_MAX];
        size_t count = pending_close_deserialize(buf, n, recs, PENDING_CLOSE_MAX);
        if (count > 0) {
            pending_close_boot_load(recs, count);
            ESP_LOGW(TAG, "%u pending close obligation(s) from before this boot; "
                          "retrying now", (unsigned)count);
        }
    } else if (errno != ENOENT) {
        ESP_LOGW(TAG, "%s: open for read failed (errno=%d); any pending close "
                      "obligation from before this boot is unknown this run",
                 PENDING_CLOSE_PATH, errno);
    }

    /* Attempt everything that just came due, immediately -- boot replay's
     * whole point. Safe to call unconditionally even when nothing loaded:
     * pending_close_service() is a no-op over an empty table. */
    pending_close_service();
#endif
}

#ifdef ESP_PLATFORM
void pending_close_note_result(int dev_idx, bool ok, bool confirmed)
{
    /* Only a CONFIRMED close ends the obligation -- spec section 4.4 treats
     * a write that landed but was not confirmed (ok=true, confirmed=false)
     * as still open, and a failed dispatch (ok=false) is exactly what
     * pending_close_service()'s own retry/backoff already expects and will
     * pick up on its next pass. Neither needs anything done here; only the
     * success case does. */
    if (ok && confirmed) pending_close_clear(dev_idx);
}
#endif /* ESP_PLATFORM */
