/* wrapper_arena.c -- pure core of the M3 Task 5 shared wrapper-bytecode
 * arena (spec section 2). See wrapper_arena.h's top comment for why this is
 * its own file/header rather than folded into wrapper_index.h, and for the
 * loader-injection contract this file relies on to stay free of any file-I/O
 * include (deliberately no ESP-IDF/FreeRTOS/stdio here -- same discipline
 * wrapper_index.c already holds itself to, for the same reason:
 * tests/host/test_wrapper_arena.c compiles and exercises this file
 * directly, no ESP-IDF toolchain required).
 *
 * Storage model: one flat byte buffer (s_buf, WRAPPER_ARENA_SIZE bytes) plus
 * a small directory (s_dir[0..s_count)) of {id, offset, len, last_used}.
 * Resident blobs are kept PACKED, contiguous, in [0, s_used) at all times --
 * every eviction below immediately memmove()s the freed gap closed and
 * shifts every later entry's offset down by the evicted length -- so the
 * free tail [s_used, WRAPPER_ARENA_SIZE) is always exactly the arena's
 * entire actual free space, never fragmented into unusable slivers.
 * wrapper_arena_get() only ever needs to know free_bytes = SIZE - s_used;
 * this is what makes "packed by actual size" (spec section 2) affordable
 * without a real allocator.
 */
#include "wrapper_arena.h"
#include <string.h>

typedef struct {
    uint16_t id;
    uint16_t offset;
    uint16_t len;
    uint32_t last_used;
} arena_entry_t;

static uint8_t          s_buf[WRAPPER_ARENA_SIZE];
static arena_entry_t    s_dir[WRAPPERS_MAX];
static uint8_t          s_count;
static uint16_t         s_used;
static uint32_t         s_clock;
static wrapper_loader_t s_loader;

void wrapper_arena_set_loader(wrapper_loader_t loader)
{
    s_loader = loader;
}

void wrapper_arena_init(void)
{
    s_count = 0;
    s_used = 0;
    s_clock = 0;
    memset(s_dir, 0, sizeof(s_dir));
    /* s_loader is deliberately left untouched -- see wrapper_arena_set_loader()'s
     * doc comment: it's a one-time wiring, independent of "clear the
     * resident cache", which is all init()/evict_all() below do. */
}

void wrapper_arena_evict_all(void)
{
    s_count = 0;
    s_used = 0;
    /* s_clock is NOT reset here (unlike init()): leaving it monotonic costs
     * nothing (it only ever governs relative LRU order among entries that
     * exist NOW, and s_count==0 just discarded all of them) and avoids ever
     * having two independently-reloaded blobs tie on last_used. */
}

static int find_entry(uint16_t id)
{
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_dir[i].id == id) return i;
    }
    return -1;
}

/* Index of the least-recently-used resident entry, or -1 when empty. */
static int find_lru(void)
{
    if (s_count == 0) return -1;
    uint8_t lru = 0;
    for (uint8_t i = 1; i < s_count; i++) {
        if (s_dir[i].last_used < s_dir[lru].last_used) lru = i;
    }
    return lru;
}

/* Evicts s_dir[idx]: closes the gap it leaves in s_buf (memmove the bytes
 * after it down), shifts every later-offset entry's `offset` down by the
 * evicted length, then removes it from the directory, keeping s_dir packed
 * in [0, count) -- same idiom wrapper_index_remove() uses for its own
 * table. */
static void evict_index(uint8_t idx)
{
    arena_entry_t victim = s_dir[idx];
    uint16_t tail_start = (uint16_t)(victim.offset + victim.len);
    uint16_t tail_len = (uint16_t)(s_used - tail_start);
    if (tail_len > 0) {
        memmove(s_buf + victim.offset, s_buf + tail_start, tail_len);
    }
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_dir[i].offset > victim.offset) {
            s_dir[i].offset = (uint16_t)(s_dir[i].offset - victim.len);
        }
    }
    s_used = (uint16_t)(s_used - victim.len);
    for (uint8_t j = idx; (uint8_t)(j + 1) < s_count; j++) {
        s_dir[j] = s_dir[j + 1];
    }
    s_count--;
}

const uint8_t *wrapper_arena_get(uint16_t id, size_t *len_out)
{
    int hit = find_entry(id);
    if (hit >= 0) {
        s_dir[hit].last_used = ++s_clock;
        if (len_out) *len_out = s_dir[hit].len;
        return s_buf + s_dir[hit].offset;
    }
    if (!s_loader) return NULL;

    for (;;) {
        size_t free_bytes = (size_t)WRAPPER_ARENA_SIZE - s_used;
        size_t need = 0;   /* 0 = "loader couldn't/didn't report a real size" */
        if (s_loader(id, s_buf + s_used, free_bytes, &need)) {
            if (s_count >= WRAPPERS_MAX) return NULL;   /* directory full -- defensive, can't happen: s_count never exceeds the number of indexed wrappers, itself <= WRAPPERS_MAX */
            arena_entry_t *e = &s_dir[s_count++];
            e->id = id;
            e->offset = s_used;
            e->len = (uint16_t)need;
            e->last_used = ++s_clock;
            s_used = (uint16_t)(s_used + need);
            if (len_out) *len_out = need;
            return s_buf + e->offset;
        }
        /* Miss. `need` (when the loader could report it) tells us whether
         * evicting anything could ever help: a blob genuinely bigger than
         * the WHOLE arena is refused immediately, with every currently
         * resident entry left untouched -- "refused rather than evicting
         * everything" (task-5-brief.md's own words for this exact case). */
        if (need > (size_t)WRAPPER_ARENA_SIZE) return NULL;
        if (s_count == 0) return NULL;   /* nothing left to evict; refuse (also covers "need unknown, non-capacity failure" -- see wrapper_arena.h) */
        evict_index((uint8_t)find_lru());
    }
}
