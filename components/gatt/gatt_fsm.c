#include "gatt_fsm.h"
#include <string.h>

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

void gatt_fsm_init(gatt_fsm_t *f, const uint8_t *plan, size_t plan_len, bool have_handles)
{
    memset(f, 0, sizeof(*f));
    f->state = GS_IDLE;
    f->have_handles = have_handles;

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
        if (off + 2 > plan_len) break;
        f->read_uuid[i] = rd_u16(plan + off);
        off += 2;
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

/* Common continuation once the connection has usable handles, whether
 * because discovery just finished (GS_DISCOVERING + GE_DISCOVERED) or
 * because the caller's warm cache made discovery unnecessary in the first
 * place (GS_CONNECTING + GE_CONNECTED with have_handles). A plan always
 * has at least one read (psvm.h's on-blob layout caps read_count at
 * 1..GATT_FSM_MAX_READS), so falling through to GS_READING here always has
 * a first read to issue. */
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
        case GE_CONNECTED:
            if (!f->have_handles) {
                f->state = GS_DISCOVERING;
                return act(GA_DISCOVER);
            }
            return begin_writes_or_reads(f);
        case GE_DISCONNECTED: return fail_report(f);
        case GE_TIMEOUT:
        case GE_ERROR:        return fail_disconnect(f);
        default:               return act(GA_NONE);
        }

    case GS_DISCOVERING:
        switch (ev->kind) {
        case GE_DISCOVERED:    return begin_writes_or_reads(f);
        case GE_DISCONNECTED:  return fail_report(f);
        case GE_TIMEOUT:
        case GE_ERROR:         return fail_disconnect(f);
        default:                return act(GA_NONE);
        }

    case GS_WRITING:
        switch (ev->kind) {
        case GE_WRITE_OK:
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
            uint8_t *slot = &f->buf[(size_t)f->read_idx * GATT_FSM_SLOT];
            memset(slot, 0, GATT_FSM_SLOT);
            uint8_t n = ev->len;
            if (n > GATT_FSM_SLOT) n = GATT_FSM_SLOT;
            if (ev->data != NULL && n > 0) memcpy(slot, ev->data, n);
            f->read_idx++;
            if (f->read_idx < f->read_count)
                return act_read(f, f->read_idx);
            f->state = GS_DONE;
            return act_decode(f);
        }
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
