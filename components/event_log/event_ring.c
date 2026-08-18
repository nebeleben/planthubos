#include "event_log.h"
#include <stddef.h>
#include <string.h>

/* Pure ring over a caller-provided EVENT_SLOTS array. Slot index for a
 * given seq is (seq-1) % EVENT_SLOTS -- seq 0 is reserved to mean "empty
 * slot" (see event_t.seq), so seqs start at 1 and wrap trivially. */

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xorout) over
 * the record's bytes up to (not including) the crc field itself. Any
 * deterministic function of the record would do here (the brief allows a
 * summing hash too) -- CRC-16 is used because a single flipped bit changes
 * it with very high probability, which is exactly the torn-write case this
 * replaces the old level-range inference for. */
uint16_t event_record_crc(const event_t *e)
{
    const uint8_t *p = (const uint8_t *)e;
    size_t n = offsetof(event_t, crc);
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u)
                                              : (uint16_t)(crc << 1));
        }
    }
    return crc;
}

void event_ring_sanitize(event_t *slots)
{
    for (uint32_t i = 0; i < EVENT_SLOTS; i++) {
        /* Cheap first reject (Task 5 brief): a level outside the defined
         * range cannot be a real record, checksum or not. */
        if (slots[i].level > EVENT_LEVEL_MAX) {
            memset(&slots[i], 0, sizeof(slots[i]));
            continue;
        }
        /* The guarantee: msg must be NUL-terminated where append() always
         * puts the NUL, and the stored crc must match the record's actual
         * bytes. Either failing means a torn write. */
        if (slots[i].msg[EVENT_MSG_MAX] != '\0' ||
            slots[i].crc != event_record_crc(&slots[i])) {
            memset(&slots[i], 0, sizeof(slots[i]));
        }
    }
}

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

    /* Last: crc covers everything set above (and nothing set after). */
    slot->crc = event_record_crc(slot);

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
