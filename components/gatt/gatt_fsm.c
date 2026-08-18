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
    f->fail = GF_NONE;

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

/* Width of a param/confirm encoding id: 0 u8, 1 u16le, 2 u16be. 0 means
 * "not an encoding this firmware knows", which every caller treats as a
 * refusal rather than as a default. */
static uint8_t enc_width(uint8_t encoding)
{
    if (encoding == 0) return 1;
    if (encoding == 1 || encoding == 2) return 2;
    return 0;
}

/* Shared failure for gatt_fsm_init_command(): leave the machine terminal
 * and say why, so nothing is half-initialized and GE_START can only ever
 * return GA_NONE. */
static void cmd_refuse(gatt_fsm_t *f, gatt_fail_t why)
{
    memset(f, 0, sizeof(*f));
    f->state = GS_FAILED;
    f->fail = why;
    f->is_command = true;
}

void gatt_fsm_init_command(gatt_fsm_t *f, const uint8_t *e, uint16_t len, uint16_t param)
{
    memset(f, 0, sizeof(*f));
    f->state = GS_IDLE;
    f->fail = GF_NONE;
    f->is_command = true;

    if (e == NULL) { cmd_refuse(f, GF_BAD_ACTION); return; }

    size_t off = 0;
    /* u8 action_count. Only the first entry is executed (see the header's
     * doc comment), but a count of zero means there is no entry at all. */
    if (off + 1 > len || e[off] < 1) { cmd_refuse(f, GF_BAD_ACTION); return; }
    off += 1;

    /* Fixed part: action_id(1) param_max(2) flags(1) write_uuid16(2)
     * write_len(1) -- the same 7 bytes psvm.c's own validator counts. */
    if (off + 7 > len) { cmd_refuse(f, GF_BAD_ACTION); return; }
    uint8_t  action_id  = e[off];
    uint16_t param_max  = rd_u16(e + off + 1);
    uint8_t  aflags     = e[off + 3];
    uint16_t write_uuid = rd_u16(e + off + 4);
    uint8_t  write_len  = e[off + 6];
    off += 7;

    if (write_len < 1 || write_len > GATT_FSM_WRITE_MAX) { cmd_refuse(f, GF_BAD_ACTION); return; }
    if (off + (size_t)write_len > len) { cmd_refuse(f, GF_BAD_ACTION); return; }
    memcpy(f->cmd_write, e + off, write_len);
    off += write_len;

    if (off + 2 > len) { cmd_refuse(f, GF_BAD_ACTION); return; }
    uint8_t param_offset   = e[off];
    uint8_t param_encoding = e[off + 1];
    off += 2;

    if (param_offset != 0xFF || param_encoding != 0xFF) {
        /* A matched pair or nothing (psvm.h's own rule). Half a pair means
         * either a parameter with no place to go or a place with no
         * encoding to write it in; both are refusals, never a guess. */
        if (param_offset == 0xFF || param_encoding == 0xFF) { cmd_refuse(f, GF_BAD_ACTION); return; }
        uint8_t width = enc_width(param_encoding);
        if (width == 0) { cmd_refuse(f, GF_BAD_ACTION); return; }
        /* The bound psvm_validate() already enforced, re-derived here
         * against the buffer that actually exists. Checked even though a
         * validated blob cannot violate it: this parser's contract,
         * inherited from gatt_fsm_init(), is to survive a plan it did not
         * validate itself.
         *
         * REFUSED, not accommodated (fix round 1, Critical 1). An earlier
         * cut grew the payload to param_offset + width when a hand-posted
         * entry declared a write_len that stopped short of its own
         * parameter. It was memory-safe (write_len is already bounded to
         * GATT_FSM_WRITE_MAX and cmd_write is that size), but it meant this
         * parser ACCEPTED an entry psvm_validate() rejects -- one on-blob
         * format with two authorities disagreeing about it, which is the
         * exact shape M5a's gate defects grew from -- and it put bytes on
         * the air that no author wrote: write_len 1 with a u16be at offset
         * 6 became an 8-byte write, five of them invented zeros. The
         * payload is exactly write_len bytes; the splice writes inside it
         * or the command does not run. */
        if ((uint16_t)param_offset + width > write_len) {
            cmd_refuse(f, GF_BAD_ACTION);
            return;
        }
        /* param_max is the entry's own declared ceiling -- already the
         * tighter of the wrapper's and the firmware's (psvm.c refuses a
         * wrapper that tries to loosen it), and already applied by
         * actor_table_check() at the queue's door. Re-checked here because
         * this is the last code that runs before the bytes go on the air,
         * and the thing on the other end is a valve. */
        if (param > param_max) { cmd_refuse(f, GF_PARAM_OVER_MAX); return; }

        /* The compiler emits write_len as "constant prefix + parameter
         * width", with zero placeholder bytes standing in for the
         * parameter (webui/src/lib/psc/codegen.js), so this overwrites
         * those placeholders in place. */
        switch (param_encoding) {
        case 0:  f->cmd_write[param_offset] = (uint8_t)param; break;
        case 1:  f->cmd_write[param_offset]     = (uint8_t)(param & 0xFF);
                 f->cmd_write[param_offset + 1] = (uint8_t)(param >> 8);
                 break;
        default: f->cmd_write[param_offset]     = (uint8_t)(param >> 8);
                 f->cmd_write[param_offset + 1] = (uint8_t)(param & 0xFF);
                 break;
        }
    }

    /* The command's write IS write[0] -- see gatt_fsm_t's command-mode
     * comment for why this reuses the read plan's slots rather than adding
     * a parallel sequence. data stays NULL: act_write() reads a command's
     * payload out of cmd_write. */
    f->write_count = 1;
    f->write[0].uuid16 = write_uuid;
    f->write[0].len = write_len;
    f->write[0].data = NULL;
    f->cmd_action_id = action_id;

    if (aflags & 0x02) {   /* has confirm */
        /* u16 uuid16, u8 min_len, u8 offset, u8 encoding, u8 op, u16 value */
        if (off + 8 > len) { cmd_refuse(f, GF_BAD_ACTION); return; }
        uint8_t min_len = e[off + 2];
        /* Clamped exactly as gatt_fsm_init() clamps a read's min_len, and
         * for the same reason stated there. */
        if (min_len < 1) min_len = 1;
        if (min_len > GATT_FSM_SLOT) min_len = GATT_FSM_SLOT;
        if (enc_width(e[off + 4]) == 0) { cmd_refuse(f, GF_BAD_ACTION); return; }
        if (e[off + 5] > 1) { cmd_refuse(f, GF_BAD_ACTION); return; }

        f->read_count = 1;
        f->read_uuid[0] = rd_u16(e + off);
        f->read_min_len[0] = min_len;
        f->cmd_confirm_offset   = e[off + 3];
        f->cmd_confirm_encoding = e[off + 4];
        f->cmd_confirm_op       = e[off + 5];
        f->cmd_confirm_value    = rd_u16(e + off + 6);
        off += 8;
    }
}

static gatt_act_t act(gatt_act_kind_t kind)
{
    gatt_act_t a = { .kind = kind, .uuid16 = 0, .data = NULL, .len = 0 };
    return a;
}

static gatt_act_t act_write(const gatt_fsm_t *f, uint8_t idx)
{
    /* A connect plan's write payload lives in the plan buffer the caller
     * still owns; a command's lives in cmd_write, spliced at init. */
    const uint8_t *data = f->is_command ? f->cmd_write : f->write[idx].data;
    gatt_act_t a = { .kind = GA_WRITE, .uuid16 = f->write[idx].uuid16,
                      .data = data, .len = f->write[idx].len };
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
 * issue. A COMMAND always takes the first branch instead: it always has
 * exactly one write (gatt_fsm_init_command() refuses an entry without one
 * before the machine ever leaves GS_FAILED), and its optional confirm read
 * only happens after that write completes. */
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

/* A command's write has completed. Either its confirm read follows on the
 * SAME connection (spec section 2's `confirm`), or the command is over --
 * completed UNCONFIRMED, which is a third outcome and not a quiet success:
 * gatt_fsm_t.confirmed stays false and the safety core treats it
 * accordingly. */
static gatt_act_t command_after_write(gatt_fsm_t *f)
{
    if (f->read_count > 0) {
        f->state = GS_READING;
        f->read_idx = 0;
        return act_read(f, 0);
    }
    f->state = GS_DONE;
    return act(GA_DISCONNECT);
}

/* The confirm read has landed in slot 0 (already checked against
 * read_min_len and zero-padded to the slot). `got` is how many bytes the
 * peer ACTUALLY sent, capped to the slot -- the padding must not be
 * compared, which is the whole point of passing it separately. */
static gatt_act_t command_confirm(gatt_fsm_t *f, uint8_t got)
{
    uint8_t width = enc_width(f->cmd_confirm_encoding);

    /* confirm_min_len is the wrapper author's own claim about how much the
     * read must return, and psvm_validate() does NOT check it covers the
     * byte the require addresses. So a peer can satisfy the declared
     * minimum and still leave the compared byte unsent, where the slot's
     * zero padding would answer in its place -- a confirmation (or a
     * failure) manufactured out of bytes the device never sent, which is
     * the exact silent-wrong-value shape the short-read rule exists to
     * prevent. Same rule, same reason, so the same failure: GF_SHORT_READ,
     * not a second rule of its own. */
    if (width == 0 || (uint16_t)f->cmd_confirm_offset + width > got) {
        f->fail = GF_SHORT_READ;
        return fail_disconnect(f);
    }

    const uint8_t *d = &f->buf[f->cmd_confirm_offset];
    uint16_t v;
    if (f->cmd_confirm_encoding == 0)      v = d[0];
    else if (f->cmd_confirm_encoding == 1) v = (uint16_t)(d[0] | ((uint16_t)d[1] << 8));
    else                                    v = (uint16_t)(((uint16_t)d[0] << 8) | d[1]);

    /* Reported whether or not the require holds -- see cmd_state's own
     * doc comment. */
    f->cmd_state = v;
    f->cmd_state_valid = true;

    bool satisfied = (f->cmd_confirm_op == 0) ? (v == f->cmd_confirm_value)
                                              : (v != f->cmd_confirm_value);
    if (!satisfied) {
        f->fail = GF_CONFIRM_FAILED;
        return fail_disconnect(f);
    }
    f->confirmed = true;
    f->state = GS_DONE;
    return act(GA_DISCONNECT);
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
            if (f->is_command) return command_after_write(f);
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
            if (ev->len < f->read_min_len[f->read_idx]) {
                f->fail = GF_SHORT_READ;
                return fail_disconnect(f);
            }

            uint8_t *slot = &f->buf[(size_t)f->read_idx * GATT_FSM_SLOT];
            memset(slot, 0, GATT_FSM_SLOT);
            uint8_t n = ev->len;
            if (n > GATT_FSM_SLOT) n = GATT_FSM_SLOT;
            if (ev->data != NULL && n > 0) memcpy(slot, ev->data, n);
            f->read_idx++;
            if (f->read_idx < f->read_count)
                return act_read(f, f->read_idx);
            /* A command's single read is its confirm read: evaluate the
             * require here rather than handing the buffer to a decode
             * there is no wrapper program for. */
            if (f->is_command) return command_confirm(f, n);
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
