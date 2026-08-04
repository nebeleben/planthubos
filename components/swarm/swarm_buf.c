/* Ring-buffer mechanics for a node's undelivered-reading backlog. See
 * swarm_buf.h for the threading contract (none -- caller-owned) and why
 * this was split out of swarm.c: it is the one piece of M5b logic that had
 * no test, precisely because it used to live inline in an ESP-IDF-only
 * file. Pure C, no ESP-IDF/FreeRTOS dependency, so tests/host/run.sh can
 * build and run it directly on the host. */
#include "swarm_buf.h"
#include <string.h>

void swarm_buf_init(swarm_buf_t *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
}

void swarm_buf_push(swarm_buf_t *b, const swarm_reading_t *r, int64_t now_us)
{
    if (!b || !r) return;

    if (b->count == SWARM_NODE_BUFFER_LEN) {
        /* Full: overwrite the oldest slot in place, then advance head --
         * that slot becomes the newest entry and the next one becomes the
         * new oldest. count stays at SWARM_NODE_BUFFER_LEN throughout. */
        b->dropped++;
        b->entries[b->head].r = *r;
        b->entries[b->head].captured_us = now_us;
        b->head = (b->head + 1) % SWARM_NODE_BUFFER_LEN;
    } else {
        int idx = (b->head + b->count) % SWARM_NODE_BUFFER_LEN;
        b->entries[idx].r = *r;
        b->entries[idx].captured_us = now_us;
        b->count++;
    }
}

bool swarm_buf_pop(swarm_buf_t *b, swarm_buf_entry_t *out)
{
    if (!b || !out || b->count == 0) return false;
    *out = b->entries[b->head];
    b->head = (b->head + 1) % SWARM_NODE_BUFFER_LEN;
    b->count--;
    return true;
}

int swarm_buf_count(const swarm_buf_t *b)
{
    return b ? b->count : 0;
}

uint32_t swarm_buf_dropped(const swarm_buf_t *b)
{
    return b ? b->dropped : 0;
}

uint16_t swarm_buf_recompute_age(uint16_t base_age_s, int64_t captured_us, int64_t now_us)
{
    int64_t extra_us = now_us - captured_us;
    uint32_t extra_s = (extra_us > 0) ? (uint32_t)(extra_us / 1000000) : 0;
    uint32_t total_s = (uint32_t)base_age_s + extra_s;
    return (total_s > UINT16_MAX) ? UINT16_MAX : (uint16_t)total_s;
}
