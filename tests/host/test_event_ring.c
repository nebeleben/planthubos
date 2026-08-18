#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "event_log.h"

static event_t g_slots[EVENT_SLOTS];

int main(void)
{
    /* append -> read returns it */
    memset(g_slots, 0, sizeof(g_slots));
    event_ring_t r;
    event_ring_init(&r, g_slots);
    assert(r.next_seq == 1);

    uint32_t seq = event_ring_append(&r, 1700000000u, 42, 1, "hello");
    assert(seq == 1);

    event_t out[EVENT_SLOTS];
    size_t n = event_ring_read(&r, 0, out, EVENT_SLOTS);
    assert(n == 1);
    assert(out[0].seq == 1);
    assert(out[0].ts == 1700000000u);
    assert(out[0].rule_id == 42);
    assert(out[0].level == 1);
    assert(strcmp(out[0].msg, "hello") == 0);

    /* read(after=N) returns only newer */
    event_ring_append(&r, 1700000001u, 1, 0, "two");
    event_ring_append(&r, 1700000002u, 2, 0, "three");
    n = event_ring_read(&r, 1, out, EVENT_SLOTS);
    assert(n == 2);
    assert(out[0].seq == 2);
    assert(out[1].seq == 3);

    /* msg longer than EVENT_MSG_MAX truncated NUL-terminated */
    char long_msg[300];
    memset(long_msg, 'x', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    uint32_t long_seq = event_ring_append(&r, 1700000003u, 3, 0, long_msg);
    n = event_ring_read(&r, long_seq - 1, out, EVENT_SLOTS);
    assert(n == 1);
    assert(strlen(out[0].msg) == EVENT_MSG_MAX);
    for (int i = 0; i < EVENT_MSG_MAX; i++) assert(out[0].msg[i] == 'x');
    assert(out[0].msg[EVENT_MSG_MAX] == '\0');

    /* EVENT_SLOTS+1 appends overwrite oldest (read after=0 yields
     * EVENT_SLOTS entries, oldest seq==2) */
    memset(g_slots, 0, sizeof(g_slots));
    event_ring_t r2;
    event_ring_init(&r2, g_slots);
    for (int i = 0; i < EVENT_SLOTS + 1; i++) {
        char msg[16];
        snprintf(msg, sizeof(msg), "e%d", i);
        event_ring_append(&r2, 1700000000u + (uint32_t)i, (uint32_t)i, 0, msg);
    }
    n = event_ring_read(&r2, 0, out, EVENT_SLOTS);
    assert(n == EVENT_SLOTS);
    assert(out[0].seq == 2);
    assert(out[EVENT_SLOTS - 1].seq == EVENT_SLOTS + 1);

    /* event_ring_init on a prefilled array (simulated reboot: slots with
     * seqs 5..9) continues at next_seq 10 and read(after=7) returns 8,9 */
    event_t prefilled[EVENT_SLOTS];
    memset(prefilled, 0, sizeof(prefilled));
    for (uint32_t s = 5; s <= 9; s++) {
        event_t *slot = &prefilled[(s - 1) % EVENT_SLOTS];
        slot->seq = s;
        slot->ts = 1700000000u + s;
        slot->rule_id = s;
        slot->level = 0;
        snprintf(slot->msg, sizeof(slot->msg), "reboot-%u", s);
    }
    event_ring_t r3;
    event_ring_init(&r3, prefilled);
    assert(r3.next_seq == 10);
    n = event_ring_read(&r3, 7, out, EVENT_SLOTS);
    assert(n == 2);
    assert(out[0].seq == 8);
    assert(out[1].seq == 9);

    /* event_ring_sanitize: a torn write can leave a slot with garbage --
     * including a bogus seq that event_ring_init would otherwise trust
     * and use to desync the whole ring's numbering. A slot failing
     * validity (level > EVENT_LEVEL_MAX, no NUL at msg[EVENT_MSG_MAX], or a
     * crc mismatch) must be zeroed; valid slots must survive untouched. */
    event_t torn[EVENT_SLOTS];
    memset(torn, 0, sizeof(torn));

    /* valid slot, seq=1 */
    torn[0].seq = 1;
    torn[0].ts = 1700000000u;
    torn[0].rule_id = 11;
    torn[0].level = 0;
    snprintf(torn[0].msg, sizeof(torn[0].msg), "valid1");
    torn[0].crc = event_record_crc(&torn[0]);   /* hand-built: must set crc itself */

    /* torn write: bogus huge seq + invalid level (>1) -- must be zeroed */
    torn[1].seq = 0xFFFFFFFFu;
    torn[1].ts = 1700000001u;
    torn[1].rule_id = 12;
    torn[1].level = 7;
    snprintf(torn[1].msg, sizeof(torn[1].msg), "garbage-level");

    /* torn write: no NUL at msg[EVENT_MSG_MAX] -- must be zeroed */
    torn[2].seq = 3;
    torn[2].ts = 1700000002u;
    torn[2].rule_id = 13;
    torn[2].level = 0;
    memset(torn[2].msg, 'y', sizeof(torn[2].msg));   /* fills msg[EVENT_MSG_MAX] too, no NUL anywhere */

    /* valid slot, seq=4 (also the highest surviving seq) */
    torn[3].seq = 4;
    torn[3].ts = 1700000003u;
    torn[3].rule_id = 14;
    torn[3].level = 1;
    snprintf(torn[3].msg, sizeof(torn[3].msg), "valid4");
    torn[3].crc = event_record_crc(&torn[3]);   /* hand-built: must set crc itself */

    event_ring_sanitize(torn);
    assert(torn[0].seq == 1);
    assert(strcmp(torn[0].msg, "valid1") == 0);
    assert(torn[1].seq == 0);   /* zeroed: invalid level */
    assert(torn[1].level == 0);
    assert(torn[2].seq == 0);   /* zeroed: missing NUL terminator */
    assert(torn[3].seq == 4);
    assert(strcmp(torn[3].msg, "valid4") == 0);

    /* the bogus huge seq must not leak into next_seq once sanitized */
    event_ring_t r4;
    event_ring_init(&r4, torn);
    assert(r4.next_seq == 5);

    printf("test_event_ring: OK\n");
    return 0;
}
