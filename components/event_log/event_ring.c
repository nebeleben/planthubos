#include "event_log.h"
#include <string.h>

/* Pure ring over a caller-provided EVENT_SLOTS array. Slot index for a
 * given seq is (seq-1) % EVENT_SLOTS -- seq 0 is reserved to mean "empty
 * slot" (see event_t.seq), so seqs start at 1 and wrap trivially. */

void event_ring_init(event_ring_t *r, event_t *slots)
{
    r->slots = slots;
    uint32_t max_seq = 0;
    for (uint32_t i = 0; i < EVENT_SLOTS; i++) {
        if (slots[i].seq > max_seq) max_seq = slots[i].seq;
    }
    r->next_seq = max_seq + 1;
}

uint32_t event_ring_append(event_ring_t *r, uint32_t ts, uint32_t rule_id,
                           uint8_t level, const char *msg)
{
    uint32_t seq = r->next_seq++;
    event_t *slot = &r->slots[(seq - 1) % EVENT_SLOTS];

    slot->seq = seq;
    slot->ts = ts;
    slot->rule_id = rule_id;
    slot->level = level;

    size_t len = strlen(msg);
    if (len > EVENT_MSG_MAX) len = EVENT_MSG_MAX;
    memcpy(slot->msg, msg, len);
    slot->msg[len] = '\0';

    return seq;
}

size_t event_ring_read(const event_ring_t *r, uint32_t after,
                       event_t *out, size_t max)
{
    if (r->next_seq <= 1) return 0;   /* nothing ever appended */
    uint32_t last_seq = r->next_seq - 1;

    uint32_t oldest = (last_seq > EVENT_SLOTS) ? (last_seq - EVENT_SLOTS + 1) : 1;
    uint32_t start = (after + 1 > oldest) ? after + 1 : oldest;

    size_t count = 0;
    for (uint32_t seq = start; seq <= last_seq && count < max; seq++) {
        out[count++] = r->slots[(seq - 1) % EVENT_SLOTS];
    }
    return count;
}
