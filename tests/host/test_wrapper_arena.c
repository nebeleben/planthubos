/* Host test for wrapper_arena.c (M3 Task 5, spec section 2 "wrapper
 * arena"). Uses a fake loader (an in-memory table of fabricated blobs) so
 * the arena's packing/eviction/LRU logic is exercised with zero file I/O --
 * exactly the "loader injected as a function pointer" design
 * task-5-brief.md's Step 1 asks for. WRAPPER_ARENA_SIZE here is the
 * fallback default (2048 B, wrapper_arena.h's #ifndef CONFIG_..., same
 * value as the esp32c3 Kconfig default) since this plain-`cc` build never
 * defines CONFIG_PLANTHUB_WRAPPER_ARENA. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wrapper_arena.h"

typedef struct { uint16_t id; size_t len; uint8_t fill; } fake_blob_t;

/* Five 300 B blobs (1500 B total, comfortably under the 2048 B default
 * arena), a 600 B blob to force an eviction, and a 3000 B blob -- bigger
 * than the WHOLE arena -- to exercise the "refused rather than evicting
 * everything" case. */
static const fake_blob_t FAKE_BLOBS[] = {
    { 1, 300, 0x11 }, { 2, 300, 0x22 }, { 3, 300, 0x33 },
    { 4, 300, 0x44 }, { 5, 300, 0x55 }, { 6, 600, 0x66 },
    { 100, 3000, 0x99 },
};
#define FAKE_BLOB_COUNT (sizeof(FAKE_BLOBS) / sizeof(FAKE_BLOBS[0]))

static int g_loader_calls;

static bool fake_loader(uint16_t id, uint8_t *buf, size_t cap, size_t *len_out)
{
    g_loader_calls++;
    for (size_t i = 0; i < FAKE_BLOB_COUNT; i++) {
        if (FAKE_BLOBS[i].id != id) continue;
        size_t need = FAKE_BLOBS[i].len;
        if (need > cap) {
            /* Mirrors wrapper_store_read_psbc()'s real contract: a "doesn't
             * fit in cap" failure still reports the TRUE size via len_out,
             * so the arena can distinguish "would fit after evicting" from
             * "will never fit". */
            if (len_out) *len_out = need;
            return false;
        }
        memset(buf, FAKE_BLOBS[i].fill, need);
        if (len_out) *len_out = need;
        return true;
    }
    return false;   /* unknown id -- like a missing file; len_out left at 0 */
}

static void assert_blob(uint16_t id, size_t expect_len, uint8_t expect_fill)
{
    size_t len = 0;
    const uint8_t *p = wrapper_arena_get(id, &len);
    assert(p != NULL);
    assert(len == expect_len);
    for (size_t i = 0; i < len; i++) assert(p[i] == expect_fill);
}

/* True iff calling wrapper_arena_get(id, ...) triggers at least one loader
 * call (i.e. id was NOT resident going in). Leaves the blob resident
 * afterward either way. */
static bool requires_reload(uint16_t id)
{
    int before = g_loader_calls;
    size_t len = 0;
    const uint8_t *p = wrapper_arena_get(id, &len);
    assert(p != NULL);
    return g_loader_calls != before;
}

int main(void)
{
    wrapper_arena_set_loader(fake_loader);
    wrapper_arena_init();

    /* --- several small blobs coexist --- */
    assert_blob(1, 300, 0x11);
    assert_blob(2, 300, 0x22);
    assert_blob(3, 300, 0x33);
    assert_blob(4, 300, 0x44);
    assert_blob(5, 300, 0x55);
    assert(g_loader_calls == 5);
    /* all five still individually correct, simultaneously resident */
    assert_blob(1, 300, 0x11);
    assert_blob(3, 300, 0x33);
    assert_blob(5, 300, 0x55);

    /* --- re-getting a resident blob does not re-load --- */
    int before = g_loader_calls;
    assert_blob(1, 300, 0x11);
    assert(g_loader_calls == before);   /* loader NOT called */

    /* bump id1's recency (most-recently-used) so the LRU test below has an
     * unambiguous target: after this, insertion/recency order is
     * 2 < 3 < 4 < 5 < 1, so id2 is the least-recently-used entry. */

    /* --- a blob larger than the arena is refused rather than evicting
     * everything --- */
    {
        size_t len = 0xDEAD;
        const uint8_t *p = wrapper_arena_get(100, &len);
        assert(p == NULL);
    }
    /* every previously resident entry is untouched -- no reload needed */
    assert(!requires_reload(2));
    assert(!requires_reload(3));
    assert(!requires_reload(4));
    assert(!requires_reload(5));
    assert(!requires_reload(1));

    /* re-establish LRU order after the requires_reload() probes above
     * (each was itself a resident hit, so it also bumped recency) --
     * probed in order 2,3,4,5,1, so that IS already the LRU order we want:
     * id2 is least-recently-used, id1 most. */

    /* --- LRU eviction order is correct --- */
    /* used=1500/2048, free=548 -- id6 (600 B) doesn't fit without evicting
     * at least one 300 B entry; the LRU one (id2) must be the one evicted. */
    assert_blob(6, 600, 0x66);
    /* Check the untouched entries FIRST: id2 is checked LAST because
     * reloading it (it must -- id2 was evicted to make room for id6) itself
     * needs 300 B out of only 248 B free at this point (2048 - 1800), so it
     * triggers a SECOND eviction of its own (now-LRU) target -- a real,
     * correct cascading effect of a genuinely tight arena, not a bug, but
     * one that would corrupt any assertion checked AFTER it. */
    assert(!requires_reload(3));                 /* everyone else still resident */
    assert(!requires_reload(4));
    assert(!requires_reload(5));
    assert(!requires_reload(1));
    assert(!requires_reload(6));
    assert(requires_reload(2));                  /* id2 was evicted -- reload needed */

    /* --- evict_all empties it --- */
    wrapper_arena_evict_all();
    assert(requires_reload(1));
    assert(requires_reload(3));
    assert(requires_reload(4));
    assert(requires_reload(5));
    assert(requires_reload(6));

    printf("test_wrapper_arena: OK\n");
    return 0;
}
