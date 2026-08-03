#pragma once
#include "swarm_frame.h"
#include <stdbool.h>
#include <stdint.h>

/* Pure-C ring buffer for a node's undelivered-reading backlog -- extracted
 * out of swarm.c (M5b) so it is host-testable (tests/host/test_swarm_buf.c)
 * without any FreeRTOS/ESP-IDF dependency. This file owns only the
 * mechanics (push/pop/evict/age); swarm.c's forward_task() still owns all
 * policy (when to buffer, when to drain, locking/threading -- there is
 * none here on purpose, see below).
 *
 * Threading: a swarm_buf_t instance is NOT internally synchronised. In
 * production exactly one instance exists (swarm.c's s_node_buf) and it is
 * touched only by forward_task(), a single dedicated task -- see that
 * file's header comment for why that needs no lock of its own. A caller
 * with multiple writers/readers must add its own external locking; this
 * module intentionally has no opinion on that, which is also what makes it
 * trivial to construct fresh, independent instances in a host test. */

#define SWARM_NODE_BUFFER_LEN 32

typedef struct {
    swarm_reading_t r;
    int64_t         captured_us;  /* esp_timer_get_time() (or, in a host test,
                                    * whatever monotonic clock the caller
                                    * chooses) when this entry was last
                                    * (re)buffered; age_s is recomputed from
                                    * this at pop/transmit time, not stored
                                    * pre-added, so a re-buffered entry's age
                                    * keeps compounding correctly across
                                    * repeated buffer/retry cycles. */
} swarm_buf_entry_t;

typedef struct {
    swarm_buf_entry_t entries[SWARM_NODE_BUFFER_LEN];
    int               head;     /* index of the oldest entry */
    int               count;    /* number of valid entries, 0..SWARM_NODE_BUFFER_LEN */
    uint32_t          dropped;  /* running count of oldest-entries evicted because the ring was full */
} swarm_buf_t;

/* Zeroes a buffer to the empty state. Safe to call on an already-used
 * buffer to reset it (dropped is reset to 0 too). */
void swarm_buf_init(swarm_buf_t *b);

/* Pushes a newly-failed reading. When full, evicts the oldest entry to make
 * room (that slot becomes the newest entry, count stays at
 * SWARM_NODE_BUFFER_LEN) and increments `dropped` -- callers that want a
 * log line for the eviction should check swarm_buf_dropped() themselves
 * before and after, or just log unconditionally at DEBUG (see swarm.c). */
void swarm_buf_push(swarm_buf_t *b, const swarm_reading_t *r, int64_t now_us);

/* Pops the oldest buffered entry (FIFO), if any. Returns false and leaves
 * *out untouched when the buffer is empty. Does NOT recompute age_s -- call
 * swarm_buf_recompute_age() with the popped captured_us at actual transmit
 * time, since "now" is only meaningful right before the send happens. */
bool swarm_buf_pop(swarm_buf_t *b, swarm_buf_entry_t *out);

int      swarm_buf_count(const swarm_buf_t *b);
uint32_t swarm_buf_dropped(const swarm_buf_t *b);

/* Recomputes the age (in seconds) of a reading that already carried
 * base_age_s of staleness at the moment it was captured (captured_us), as
 * of now_us. Adds the elapsed time to base_age_s rather than replacing it,
 * so age compounds correctly across repeated buffer/retry cycles, and
 * clamps the result to UINT16_MAX (swarm_reading_t.age_s's full range,
 * ~18h) rather than silently wrapping -- data_core's own much shorter
 * max-age drop threshold makes anything near that clamp moot in practice,
 * but wrapping back to a small number would make a very stale reading look
 * fresh, which is the one outcome worth explicitly avoiding here. A
 * negative/zero elapsed (clock oddities, or now_us == captured_us) adds
 * nothing rather than underflowing. */
uint16_t swarm_buf_recompute_age(uint16_t base_age_s, int64_t captured_us, int64_t now_us);
