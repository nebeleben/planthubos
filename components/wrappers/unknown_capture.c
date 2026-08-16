/* unknown_capture.c -- pure core of the M3 Task 6 unknown-device capture
 * (spec section 5). See unknown_capture.h's top comment for why this is its
 * own file/header, and for the deliberate signature deviation from
 * task-6-brief.md's literal `adv_item_t*` (kept OUT of this file's includes
 * on purpose -- deliberately no ESP-IDF/FreeRTOS/file-I/O/adv_queue.h here,
 * same discipline wrapper_index.c and wrapper_arena.c already hold
 * themselves to, so tests/host/test_unknown_capture.c compiles and
 * exercises this file directly, no ESP-IDF toolchain required).
 *
 * Storage model: one flat static table, s_tbl[UNKNOWN_DEVICES], each slot
 * either free (in_use == false) or holding one tracked device's MAC, its
 * last_seen_s (also the LRU key -- see find_lru() below, spec section 5's
 * own "oldest device evicted first" needs no separate clock the way
 * wrapper_arena.c's s_clock does, because unknown_capture_add() is called
 * at most once per advertisement and last_seen_s IS the recency signal) and
 * up to UNKNOWN_SAMPLES samples kept oldest-first/newest-last in s[].
 *
 * Synchronization: the table and all helper functions are internally
 * protected by a mutex (s_mux). All critical functions (unknown_capture_add(),
 * unknown_capture_forget(), unknown_capture_list()) take the lock across
 * their entire bodies, so callers never need to synchronise. This module is
 * safe to call from any task.
 *
 * Measured byte cost (host test prints this; also checked by the
 * _Static_assert below so a struct-layout change can't silently drift the
 * spec section 7 budget line without a compile failure calling it out):
 *   sizeof(unknown_sample_t) = 40 B  (31 payload + 1 len + 1 rssi + 1 pad
 *                                     + 4 ts, uint32_t needs 4-B alignment)
 *   sizeof(unknown_dev_t)    = 96 B  (1 in_use + 6 mac + 1 pad + 4
 *                                     last_seen_s + 2*40 samples + 1 n
 *                                     + 3 tail pad, uint32_t/sample array
 *                                     both need 4-B alignment)
 *   s_tbl[8]                 = 768 B static -- the table itself
 *   StaticSemaphore_t s_mux  =  84 B static -- the mutex buffer. MEASURED,
 *                                     not assumed: `riscv32-esp-elf-nm
 *                                     --print-size` reports 0x54 for
 *                                     s_mux_buf on both esp32c3 and esp32c5
 *                                     builds (IDF v5.5.5).
 *   Total                    =  852 B static resident cost, against spec
 *                                     section 5's "~700 B" estimate -- the
 *                                     gap is alignment padding the spec's
 *                                     back-of-envelope 8*2*37 didn't count,
 *                                     plus this mutex, which section 5 did
 *                                     not anticipate at all.
 */
#include "unknown_capture.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_mux_buf;
static SemaphoreHandle_t s_mux;

static inline void cap_lock(void)
{
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
}

static inline void cap_unlock(void)
{
    if (s_mux) xSemaphoreGive(s_mux);
}

#else

/* On host (non-ESP), cap_lock/cap_unlock are no-ops so this file
 * compiles with plain cc and no ESP-IDF/FreeRTOS toolchain. */
static inline void cap_lock(void) { }
static inline void cap_unlock(void) { }

#endif

_Static_assert(sizeof(unknown_sample_t) == 40,
               "unknown_sample_t layout drifted from this file's documented 40 B");
_Static_assert(sizeof(unknown_dev_t) == 96,
               "unknown_dev_t layout drifted from this file's documented 96 B");

static unknown_dev_t s_tbl[UNKNOWN_DEVICES];

void unknown_capture_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_mux) {
        s_mux = xSemaphoreCreateMutexStatic(&s_mux_buf);
    }
#endif
    memset(s_tbl, 0, sizeof(s_tbl));
}

/* Helpers below (find_by_mac, find_free, find_lru, push_sample) are
 * lock-free and called only with s_mux already held by the caller. */

static int find_by_mac(const uint8_t mac[6])
{
    for (int i = 0; i < UNKNOWN_DEVICES; i++) {
        if (s_tbl[i].in_use && memcmp(s_tbl[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static int find_free(void)
{
    for (int i = 0; i < UNKNOWN_DEVICES; i++) {
        if (!s_tbl[i].in_use) return i;
    }
    return -1;
}

/* Index of the least-recently-seen tracked device. Only ever called with at
 * least one in_use entry (unknown_capture_add()'s only caller of this is
 * the "table is full" branch, and UNKNOWN_DEVICES > 0), so no empty-table
 * case to handle, unlike wrapper_arena.c's find_lru(). */
static int find_lru(void)
{
    int lru = 0;
    for (int i = 1; i < UNKNOWN_DEVICES; i++) {
        if (s_tbl[i].last_seen_s < s_tbl[lru].last_seen_s) lru = i;
    }
    return lru;
}

/* Appends one sample to `d`'s rolling window (see unknown_capture.h's
 * unknown_capture_add() doc comment for the exact oldest-first/newest-last
 * ordering contract this maintains). `len` is already clamped by the caller. */
static void push_sample(unknown_dev_t *d, const uint8_t *payload, uint8_t len,
                         int8_t rssi, uint32_t ts)
{
    if (d->n >= UNKNOWN_SAMPLES) {
        /* Full: drop the oldest (s[0]) by shifting everything else down one,
         * freeing s[UNKNOWN_SAMPLES - 1] for the new sample below. For
         * UNKNOWN_SAMPLES == 2 this is exactly one move (s[0] = s[1]). */
        for (uint8_t i = 0; (uint8_t)(i + 1) < UNKNOWN_SAMPLES; i++) {
            d->s[i] = d->s[i + 1];
        }
        d->n = UNKNOWN_SAMPLES - 1;
    }
    unknown_sample_t *s = &d->s[d->n++];
    memset(s->payload, 0, sizeof(s->payload));
    memcpy(s->payload, payload, len);
    s->len = len;
    s->rssi = rssi;
    s->ts = ts;
}

void unknown_capture_add(const uint8_t mac[6], const uint8_t *payload,
                          uint8_t len, int8_t rssi, uint32_t ts)
{
    cap_lock();

    if (len > ADV_PAYLOAD_MAX) len = ADV_PAYLOAD_MAX;

    int idx = find_by_mac(mac);
    if (idx < 0) {
        idx = find_free();
        if (idx < 0) idx = find_lru();   /* table full -- evict least-recently-seen */
        memset(&s_tbl[idx], 0, sizeof(s_tbl[idx]));
        s_tbl[idx].in_use = true;
        memcpy(s_tbl[idx].mac, mac, 6);
    }
    s_tbl[idx].last_seen_s = ts;
    push_sample(&s_tbl[idx], payload, len, rssi, ts);

    cap_unlock();
}

void unknown_capture_forget(const uint8_t mac[6])
{
    cap_lock();

    int idx = find_by_mac(mac);
    if (idx >= 0) memset(&s_tbl[idx], 0, sizeof(s_tbl[idx]));   /* in_use = false, slot freed */

    cap_unlock();
}

size_t unknown_capture_list(unknown_dev_t *out, size_t max)
{
    cap_lock();

    size_t n = 0;
    for (int i = 0; i < UNKNOWN_DEVICES && n < max; i++) {
        if (s_tbl[i].in_use) out[n++] = s_tbl[i];
    }

    cap_unlock();
    return n;
}
