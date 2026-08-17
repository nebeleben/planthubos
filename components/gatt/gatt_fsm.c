#include "gatt_fsm.h"
#include <string.h>

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void gatt_fsm_init(gatt_fsm_t *f, const uint8_t *plan, size_t plan_len)
{
    memset(f, 0, sizeof(*f));
    f->state = GS_IDLE;

    size_t off = 0;
    uint8_t read_count = 0, write_count = 0;

    if (off + 6 <= plan_len) {
        read_count = plan[off];
        write_count = plan[off + 1];
        /* interval_s (u32 LE) is parsed only to keep offsets aligned with
         * the on-blob layout -- this state machine never uses it (Task 5's
         * scheduler owns the interval gate). */
        off += 6;
    } else {
        off = plan_len;
    }

    if (read_count > GATT_FSM_MAX_READS) read_count = GATT_FSM_MAX_READS;
    if (write_count > GATT_FSM_MAX_WRITES) write_count = GATT_FSM_MAX_WRITES;

    uint8_t reads_parsed = 0;
    for (uint8_t i = 0; i < read_count; i++) {
        /* {u16 uuid16, u8 min_len} -- 3 bytes, not 2. */
        if (off + 3 > plan_len) break;
        f->read_uuid[i] = rd_u16(plan + off);
        /* Clamped, not rejected: psvm_validate() has already refused a
         * min_len outside 1..GATT_FSM_SLOT, and this parser's contract is
         * to survive a truncated or hostile plan without reading past it
         * rather than to re-adjudicate one. */
        uint8_t min_len = plan[off + 2];
        if (min_len < 1) min_len = 1;
        if (min_len > GATT_FSM_SLOT) min_len = GATT_FSM_SLOT;
        f->read_min_len[i] = min_len;
        off += 3;
        reads_parsed++;
    }
    f->read_count = reads_parsed;

    uint8_t writes_parsed = 0;
    for (uint8_t i = 0; i < write_count; i++) {
        if (off + 3 > plan_len) break;
        uint16_t uuid16 = rd_u16(plan + off);
        uint8_t wlen = plan[off + 2];
        off += 3;
        if (wlen > GATT_FSM_WRITE_MAX) break;
        if (off + (size_t)wlen > plan_len) break;
        f->write[i].uuid16 = uuid16;
        f->write[i].len = wlen;
        f->write[i].data = plan + off;
        off += wlen;
        writes_parsed++;
    }
    f->write_count = writes_parsed;
}

static gatt_act_t act(gatt_act_kind_t kind)
{
    gatt_act_t a = { .kind = kind, .uuid16 = 0, .data = NULL, .len = 0 };
    return a;
}

static gatt_act_t act_write(const gatt_fsm_t *f, uint8_t idx)
{
    gatt_act_t a = { .kind = GA_WRITE, .uuid16 = f->write[idx].uuid16,
                      .data = f->write[idx].data, .len = f->write[idx].len };
    return a;
}

static gatt_act_t act_read(const gatt_fsm_t *f, uint8_t idx)
{
    gatt_act_t a = { .kind = GA_READ, .uuid16 = f->read_uuid[idx], .data = NULL, .len = 0 };
    return a;
}

static gatt_act_t act_decode(const gatt_fsm_t *f)
{
    gatt_act_t a = { .kind = GA_DECODE, .uuid16 = 0, .data = f->buf,
                      .len = (uint8_t)((uint16_t)f->read_count * GATT_FSM_SLOT) };
    return a;
}

/* Common continuation once the connection is up (GS_CONNECTING +
 * GE_CONNECTED): every read and write is addressed by uuid16, resolved by
 * the caller's server-side UUID lookup on THIS connection, so there is no
 * discovery step to wait for first (removed during the M5a hardware gate
 * -- see gatt_fsm_init()'s doc comment). A plan always has at least one
 * read (psvm.h's on-blob layout caps read_count at 1..GATT_FSM_MAX_READS),
 * so falling through to GS_READING here always has a first read to
 * issue. */
static gatt_act_t begin_writes_or_reads(gatt_fsm_t *f)
{
    if (f->write_count > 0) {
        f->state = GS_WRITING;
        f->write_idx = 0;
        return act_write(f, 0);
    }
    f->state = GS_READING;
    f->read_idx = 0;
    return act_read(f, 0);
}

/* Shared tail for GE_TIMEOUT/GE_ERROR in any state with a connection open:
 * the link may still be up, so the caller must actively disconnect it. */
static gatt_act_t fail_disconnect(gatt_fsm_t *f)
{
    f->state = GS_FAILED;
    return act(GA_DISCONNECT);
}

/* Shared tail for an unsolicited GE_DISCONNECTED: the link is already
 * down, so there is nothing to command, only to report. */
static gatt_act_t fail_report(gatt_fsm_t *f)
{
    f->state = GS_FAILED;
    return act(GA_REPORT_FAIL);
}

gatt_act_t gatt_fsm_step(gatt_fsm_t *f, const gatt_ev_t *ev)
{
    switch (f->state) {

    case GS_IDLE:
        if (ev->kind == GE_START) {
            f->state = GS_CONNECTING;
            return act(GA_CONNECT);
        }
        return act(GA_NONE);

    case GS_CONNECTING:
        switch (ev->kind) {
        case GE_CONNECTED:     return begin_writes_or_reads(f);
        case GE_DISCONNECTED: return fail_report(f);
        case GE_TIMEOUT:
        case GE_ERROR:        return fail_disconnect(f);
        default:               return act(GA_NONE);
        }

    case GS_WRITING:
        switch (ev->kind) {
        case GE_WRITE_OK:
            /* Same identity check as GE_READ_OK below: a completion for
             * anything other than the write currently awaited -- a
             * same-state duplicate of the write just finished, or a
             * uuid16 this plan never named -- is ignored rather than
             * mistaken for the next write's completion (see
             * gatt_ev_t.handle's doc comment). Skipping a real write
             * silently is the same wrong-value hazard as skipping a real
             * read: a device left unwoken or in its default mode still
             * answers the reads that follow with plausible bytes. */
            if (ev->handle != f->write[f->write_idx].uuid16) return act(GA_NONE);

            f->write_idx++;
            if (f->write_idx < f->write_count)
                return act_write(f, f->write_idx);
            f->state = GS_READING;
            f->read_idx = 0;
            return act_read(f, 0);
        case GE_DISCONNECTED:  return fail_report(f);
        case GE_TIMEOUT:
        case GE_ERROR:         return fail_disconnect(f);
        default:                return act(GA_NONE);
        }

    case GS_READING:
        switch (ev->kind) {
        case GE_READ_OK: {
            /* A completion for anything other than the read currently
             * awaited -- a same-state duplicate of the read just
             * finished, or a uuid16 this plan never named at all -- is
             * ignored rather than mistaken for the next read's
             * completion (see gatt_ev_t.handle's doc comment). */
            if (ev->handle != f->read_uuid[f->read_idx]) return act(GA_NONE);

            /* Too short to decode. Zero-padding the slot would hand the
             * wrapper bytes the peer never sent and emit a plausible wrong
             * value with fails at 0 -- exactly the silent-wrong-value shape
             * spec section 4 exists to prevent, reached by a short
             * characteristic rather than by handle drift. Fail the attempt
             * instead, so it is visible as a failure. */
            if (ev->len < f->read_min_len[f->read_idx]) return fail_disconnect(f);

            uint8_t *slot = &f->buf[(size_t)f->read_idx * GATT_FSM_SLOT];
            memset(slot, 0, GATT_FSM_SLOT);
            uint8_t n = ev->len;
            if (n > GATT_FSM_SLOT) n = GATT_FSM_SLOT;
            if (ev->data != NULL && n > 0) memcpy(slot, ev->data, n);
            f->read_idx++;
            if (f->read_idx < f->read_count)
                return act_read(f, f->read_idx);
            f->state = GS_DECODING;
            return act_decode(f);
        }
        case GE_DISCONNECTED:  return fail_report(f);
        case GE_TIMEOUT:
        case GE_ERROR:         return fail_disconnect(f);
        default:                return act(GA_NONE);
        }

    case GS_DECODING:
        switch (ev->kind) {
        case GE_DECODED:
            f->state = GS_DONE;
            return act(GA_DISCONNECT);
        case GE_DISCONNECTED:  return fail_report(f);
        case GE_TIMEOUT:
        case GE_ERROR:         return fail_disconnect(f);
        default:                return act(GA_NONE);
        }

    case GS_DONE:
    case GS_FAILED:
    default:
        return act(GA_NONE);
    }
}
