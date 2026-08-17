/* psvm.c -- PSBC v1 bytecode validator + interpreter.
 * Pure C99 + libc, no ESP-IDF includes (host-testable, spec section 2/3). */
#include "psvm.h"
#include "action.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define PSVM_HEADER_LEN 18u

/* ---- little-endian readers ---- */
static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static int16_t rd_i16(const uint8_t *p) {
    return (int16_t)rd_u16(p);
}
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- const pool helpers ---- */

/* Walks const_count entries starting at *offset, bounds-checking against len.
 * Advances *offset to just past the pool on success. */
static psvm_err_t validate_consts(const uint8_t *blob, size_t len, size_t *offset,
                                  uint16_t const_count) {
    size_t o = *offset;
    for (uint16_t i = 0; i < const_count; i++) {
        if (o + 1 > len) return PSVM_ERR_TRUNCATED;
        uint8_t tag = blob[o];
        if (tag == 0) {
            if (o + 5 > len) return PSVM_ERR_TRUNCATED;
            o += 5;
        } else if (tag == 1) {
            if (o + 3 > len) return PSVM_ERR_TRUNCATED;
            uint16_t l = rd_u16(blob + o + 1);
            if (o + 3 + (size_t)l > len) return PSVM_ERR_TRUNCATED;
            o += 3 + (size_t)l;
        } else {
            return PSVM_ERR_HEADER;
        }
    }
    *offset = o;
    return PSVM_OK;
}

/* Tag of the idx-th const entry, assuming the pool already validated well-formed. */
static uint8_t const_tag_at(const uint8_t *blob, size_t consts_off, uint16_t idx) {
    size_t o = consts_off;
    for (uint16_t i = 0; i < idx; i++) {
        uint8_t tag = blob[o];
        if (tag == 0) o += 5;
        else { uint16_t l = rd_u16(blob + o + 1); o += 3 + (size_t)l; }
    }
    return blob[o];
}

/* Full entry (tag + value) of the idx-th const, assuming a validated pool. */
static void const_entry(const psvm_prog_t *p, uint16_t idx, uint8_t *tag_out,
                        float *f_out, const char **s_out, uint16_t *slen_out) {
    const uint8_t *o = p->consts;
    for (uint16_t i = 0; i < idx; i++) {
        uint8_t tag = o[0];
        if (tag == 0) o += 5;
        else { uint16_t l = rd_u16(o + 1); o += 3 + (size_t)l; }
    }
    uint8_t tag = o[0];
    if (tag_out) *tag_out = tag;
    if (tag == 0) {
        float f;
        memcpy(&f, o + 1, 4);
        if (f_out) *f_out = f;
    } else {
        uint16_t l = rd_u16(o + 1);
        if (slen_out) *slen_out = l;
        if (s_out) *s_out = (const char *)(o + 3);
    }
}

/* Scans a (already length-bounded) code section for EMIT (0x69, wrapper
 * dialect) instructions and range-checks each one's inline capability-id
 * operand against caps_max -- the same bound the ref table's own capability
 * field is checked against just above, for the OTHER place a capability id
 * appears in a blob. Added on M3 Task 5 review (round 2): previously only
 * the ref table was checked, so a wrapper naming a nonexistent capability
 * in an EMIT installed cleanly and then failed identically, silently, at
 * every single run forever -- letting it fail at RUN time instead of
 * catching it here, at INSTALL time, is strictly worse for the wrapper's
 * author (no feedback at all) and opened a way to bypass
 * data_core_submit_cap()'s per-(device,capability) warn throttle (an
 * invalid cap_id has no valid bitmask slot to remember against).
 *
 * Walks the stream using each opcode's own operand width -- the exact
 * widths psvm_run()'s interpreter switch below uses for the SAME code
 * region -- so an operand byte is never mistaken for an opcode. This is a
 * second place those widths are now spelled out (psvm_run()'s switch is the
 * other); both live in this one file, which is the authority on the M3
 * wrapper opcode set (spec section 3), so keeping them in sync is a local,
 * contained concern, not a cross-file duplication risk. An opcode this
 * table doesn't recognise, or a truncated final instruction, just stops the
 * scan -- this function's only job is the EMIT check; a genuinely malformed
 * or unknown opcode is still caught exactly as before, by psvm_run()'s own
 * PSVM_ERR_BADOP at run time (this does not become a second, stricter
 * bytecode verifier). Applies regardless of `dialect`: EMIT is a
 * wrapper-only opcode in practice (a rules-dialect compiler never emits it),
 * so this is a no-op scan for rules bytecode either way, but nothing here
 * or in psvm_run() itself actually gates opcode interpretation by dialect,
 * so checking unconditionally costs nothing and closes the loophole
 * regardless of how it might be reached. */
static psvm_err_t validate_emit_caps(const uint8_t *code, uint16_t code_len, uint8_t caps_max) {
    uint16_t pc = 0;
    while (pc < code_len) {
        uint8_t op = code[pc];
        uint16_t width;
        switch (op) {
        case 0x01: case 0x02: case 0x40: case 0x41:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:
            width = 3; break;
        case 0x67:
            width = 5; break;
        case 0x50: case 0x51:
            width = 2; break;
        case 0x69:
            if ((size_t)pc + 2 > code_len) return PSVM_OK;   /* truncated -- psvm_run() catches it */
            if (code[pc + 1] > caps_max) return PSVM_ERR_REF;
            width = 2;
            break;
        case 0x00: case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x30: case 0x31: case 0x32:
        case 0x68: case 0x6A: case 0x6B: case 0x6C: case 0xFF:
            width = 1; break;
        default:
            return PSVM_OK;   /* unrecognised opcode -- not this pass's job */
        }
        pc = (uint16_t)(pc + width);
    }
    return PSVM_OK;
}

psvm_err_t psvm_validate(const uint8_t *blob, size_t len, uint8_t dialect,
                         uint8_t caps_max, uint32_t builtins_impl, psvm_prog_t *out) {
    if (len < PSVM_HEADER_LEN) return PSVM_ERR_TRUNCATED;
    if (memcmp(blob, "PSBC", 4) != 0) return PSVM_ERR_HEADER;
    if (blob[4] != PSVM_FMT_VER || blob[5] != dialect) return PSVM_ERR_HEADER;
    uint16_t flags = rd_u16(blob + 6);
    if (flags & ~(uint16_t)(PSVM_FLAG_CONNECT_PLAN | PSVM_FLAG_ACTION_TABLE)) return PSVM_ERR_HEADER;
    /* A rules program has no radio: letting dialect=1 declare a GATT connect
     * plan would be a category error, so it's refused outright at the
     * header, before anything else in the blob is even looked at. */
    if ((flags & PSVM_FLAG_CONNECT_PLAN) && dialect == PSVM_DIALECT_RULES) return PSVM_ERR_HEADER;
    /* Same reasoning for the action table: a rules program has no
     * actuators either, refused at the header before anything past it is
     * looked at. */
    if ((flags & PSVM_FLAG_ACTION_TABLE) && dialect == PSVM_DIALECT_RULES) return PSVM_ERR_HEADER;

    uint32_t builtins = rd_u32(blob + 8);
    if (builtins & ~builtins_impl) return PSVM_ERR_LIMITS;

    uint16_t const_count = rd_u16(blob + 12);
    uint16_t ref_count = rd_u16(blob + 14);
    uint16_t code_len = rd_u16(blob + 16);

    if (const_count > 256) return PSVM_ERR_LIMITS;
    if (ref_count > PSVM_MAX_REFS) return PSVM_ERR_LIMITS;

    size_t offset = PSVM_HEADER_LEN;
    size_t consts_off = offset;
    psvm_err_t e = validate_consts(blob, len, &offset, const_count);
    if (e != PSVM_OK) return e;

    size_t refs_off = offset;
    if (refs_off + (size_t)ref_count * 5u > len) return PSVM_ERR_TRUNCATED;
    for (uint16_t i = 0; i < ref_count; i++) {
        const uint8_t *r = blob + refs_off + (size_t)i * 5u;
        uint8_t kind = r[0];
        uint16_t name_const = rd_u16(r + 1);
        uint8_t capability = r[3];
        uint8_t field = r[4];
        if (kind > 1) return PSVM_ERR_REF;
        if (name_const >= const_count) return PSVM_ERR_REF;
        if (const_tag_at(blob, consts_off, name_const) != 1) return PSVM_ERR_REF;
        if (capability > caps_max) return PSVM_ERR_REF;
        if (field > 1) return PSVM_ERR_REF;
    }
    offset = refs_off + (size_t)ref_count * 5u;

    size_t code_off = offset;
    if (code_off + (size_t)code_len > len) return PSVM_ERR_TRUNCATED;

    psvm_err_t emit_err = validate_emit_caps(blob + code_off, code_len, caps_max);
    if (emit_err != PSVM_OK) return emit_err;

    /* M5a connect plan (PSVM_FLAG_CONNECT_PLAN): a trailing section right
     * after the code, present only when the flag is set (psvm.h's own doc
     * comment on the flag has the on-blob layout and the compatibility
     * argument for why it rides as a trailing section behind a flag bit).
     * Every field is bounds-checked against `len` before it is trusted --
     * this validator is the only thing standing between a crafted blob from
     * an authenticated HTTP client and the interpreter, and "authenticated"
     * is not "well-formed". */
    size_t plan_off = code_off + (size_t)code_len;
    const uint8_t *plan_ptr = NULL;
    uint16_t plan_section_len = 0;
    if (flags & PSVM_FLAG_CONNECT_PLAN) {
        size_t po = plan_off;
        if (po + 6 > len) return PSVM_ERR_TRUNCATED;   /* read_count, write_count, interval_s (u32) */
        uint8_t plan_read_count = blob[po];
        uint8_t plan_write_count = blob[po + 1];
        uint32_t interval_s = rd_u32(blob + po + 2);
        po += 6;
        /* At least one read. A zero-read plan is not merely useless: the
         * engine refuses to start an attempt for it, so it never reaches
         * the scheduler that would have moved the interval gate, and
         * gatt_sched_due() therefore stays true forever -- every
         * advertisement from that device (about one a second, for the life
         * of the boot) pays an arena fetch, a psvm_validate() and a plan
         * copy to be refused again. The compiler already requires a read
         * ("connect block must declare at least one read", parser.js), so
         * this rejects only a hand-built or corrupted blob. */
        if (plan_read_count < 1 || plan_read_count > PSVM_PLAN_MAX_READS) return PSVM_ERR_LIMITS;
        if (plan_write_count > PSVM_PLAN_MAX_WRITES) return PSVM_ERR_LIMITS;
        /* interval_s is u32 (psvm.h's PSVM_FLAG_CONNECT_PLAN doc comment has
         * the reasoning): both bounds are representable and enforced. */
        if (interval_s < 60 || interval_s > 86400) return PSVM_ERR_LIMITS;

        /* Each read entry is {u16 uuid16, u8 min_len}. min_len must be
         * 1..PSVM_PLAN_SLOT: 0 would let an empty response count as a
         * successful read, and anything past a slot could never be
         * satisfied, so both are rejected here rather than being clamped
         * into something that silently behaves differently from what the
         * wrapper's author wrote. */
        if (po + (size_t)plan_read_count * 3u > len) return PSVM_ERR_TRUNCATED;
        for (uint8_t i = 0; i < plan_read_count; i++) {
            uint8_t min_len = blob[po + (size_t)i * 3u + 2u];
            if (min_len < 1 || min_len > PSVM_PLAN_SLOT) return PSVM_ERR_LIMITS;
        }
        po += (size_t)plan_read_count * 3u;

        for (uint8_t i = 0; i < plan_write_count; i++) {
            if (po + 3 > len) return PSVM_ERR_TRUNCATED;   /* uuid16 + len */
            uint8_t wlen = blob[po + 2];
            if (wlen < 1 || wlen > PSVM_PLAN_WRITE_MAX) return PSVM_ERR_LIMITS;
            po += 3;
            if (po + (size_t)wlen > len) return PSVM_ERR_TRUNCATED;
            po += wlen;
        }

        plan_ptr = blob + plan_off;
        plan_section_len = (uint16_t)(po - plan_off);
    }

    /* M5b action table (PSVM_FLAG_ACTION_TABLE): a second trailing section,
     * after the connect plan when both are present (psvm.h's own doc
     * comment on the flag has the on-blob layout). act_off is the end of
     * the plan section when one was parsed above, or plan_off (== right
     * after the code) when it was not -- plan_off/plan_section_len are
     * already 0/unset in that case, so this one line covers both. Every
     * field is bounds-checked against `len` as it is derived, exactly like
     * the connect-plan block just above: never trust action_count or
     * write_len alone. */
    size_t act_off = plan_off + (size_t)plan_section_len;
    const uint8_t *act_ptr = NULL;
    uint16_t act_section_len = 0;
    if (flags & PSVM_FLAG_ACTION_TABLE) {
        size_t ao = act_off;
        if (ao + 1 > len) return PSVM_ERR_TRUNCATED;
        uint8_t count = blob[ao];
        ao += 1;
        if (count < 1 || count > PSVM_ACTION_MAX) return PSVM_ERR_LIMITS;

        for (uint8_t i = 0; i < count; i++) {
            /* Fixed part up to and including write_len: id(1) param_max(2)
             * flags(1) write_uuid16(2) write_len(1) = 7 bytes. */
            if (ao + 7 > len) return PSVM_ERR_TRUNCATED;
            uint8_t  id        = blob[ao];
            uint16_t param_max = rd_u16(blob + ao + 1);
            uint8_t  aflags    = blob[ao + 3];
            uint8_t  wlen      = blob[ao + 6];

            const action_t *spec_a = action_get(id);
            if (!spec_a) return PSVM_ERR_LIMITS;
            /* The safety bound: a wrapper (M4 lets an AI write its source)
             * may tighten the firmware's hard param_max and may never
             * loosen it. */
            if (param_max > spec_a->param_max) return PSVM_ERR_LIMITS;
            if (wlen < 1 || wlen > PSVM_PLAN_WRITE_MAX) return PSVM_ERR_LIMITS;
            ao += 7;

            if (ao + (size_t)wlen > len) return PSVM_ERR_TRUNCATED;
            ao += wlen;

            if (ao + 2 > len) return PSVM_ERR_TRUNCATED;   /* param_offset, param_encoding */
            ao += 2;

            if (aflags & 0x02) {   /* has confirm */
                if (ao + 8 > len) return PSVM_ERR_TRUNCATED;
                uint8_t cmin = blob[ao + 2];
                if (cmin < 1 || cmin > PSVM_PLAN_SLOT) return PSVM_ERR_LIMITS;
                ao += 8;
            }
        }

        act_ptr = blob + act_off;
        act_section_len = (uint16_t)(ao - act_off);
    }

    if (out) {
        out->blob = blob;
        out->len = len;
        out->const_count = const_count;
        out->ref_count = ref_count;
        out->code_len = code_len;
        out->builtins = builtins;
        out->consts = blob + consts_off;
        out->refs = blob + refs_off;
        out->code = blob + code_off;
        out->plan = plan_ptr;
        out->plan_len = plan_section_len;
        out->actions = act_ptr;
        out->actions_len = act_section_len;
    }
    return PSVM_OK;
}

psvm_ref_t psvm_get_ref(const psvm_prog_t *p, uint16_t idx) {
    psvm_ref_t r = {0, 0, 0, 0};
    if (!p || idx >= p->ref_count) return r;
    const uint8_t *o = p->refs + (size_t)idx * 5u;
    r.kind = o[0];
    r.name_const = rd_u16(o + 1);
    r.capability = o[3];
    r.field = o[4];
    return r;
}

const char *psvm_get_str(const psvm_prog_t *p, uint16_t idx, uint16_t *len_out) {
    if (!p || idx >= p->const_count) return NULL;
    uint8_t tag;
    const char *s = NULL;
    uint16_t slen = 0;
    const_entry(p, idx, &tag, NULL, &s, &slen);
    if (tag != 1) return NULL;
    if (len_out) *len_out = slen;
    return s;
}

/* ---- interpreter ---- */

typedef enum { V_NUM, V_STR, V_BOOL } vtag_t;
typedef struct {
    vtag_t tag;
    float f;
    const char *s;
    uint16_t slen;
    bool b;
} value_t;

/* One buffered EMIT (spec section 3) -- see psvm_run()'s doc comment in
 * psvm.h for why buffering (rather than calling wio->emit immediately) is
 * what makes "a failed require emits nothing" work cleanly. */
typedef struct { uint8_t cap; float value; } emit_item_t;

psvm_result_t psvm_run(const psvm_prog_t *p, const psvm_ref_val_t *resolved,
                       const psvm_wrapper_io_t *wio,
                       psvm_sink_t sink, void *sink_ctx, bool run_actions) {
    psvm_result_t res = {false, PSVM_OK, 0};
    value_t stack[PSVM_STACK];
    int sp = 0;
    char strbuf[PSVM_STRBUF];
    uint32_t steps = 0;
    uint16_t pc = 0;
    bool cond = false;

    /* Wrapper-dialect state -- harmless/unused for a rules-dialect run
     * (wio==NULL there, and rules bytecode never contains a payload
     * accessor, EMIT or AES_CCM opcode). payload_buf is a working COPY (not
     * a pointer into wio->payload.data) specifically so AES_CCM can decrypt
     * in place without requiring the caller to hand over mutable storage. */
    uint8_t payload_buf[PSVM_PAYLOAD_MAX];
    uint8_t payload_len = 0;
    if (wio && wio->payload.data && wio->payload.len > 0) {
        payload_len = (wio->payload.len > PSVM_PAYLOAD_MAX) ? PSVM_PAYLOAD_MAX : wio->payload.len;
        memcpy(payload_buf, wio->payload.data, payload_len);
    }
    emit_item_t emit_buf[PSVM_MAX_EMITS];
    uint8_t emit_count = 0;

    for (;;) {
        if (steps >= PSVM_MAX_STEPS) { res.err = PSVM_ERR_STEPS; goto done; }
        steps++;
        if (pc >= p->code_len) { res.err = PSVM_ERR_JUMP; goto done; }
        uint8_t op = p->code[pc];

        switch (op) {
        case 0x01: { /* PUSH_CONST u16 */
            if ((size_t)pc + 3 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            uint16_t idx = rd_u16(p->code + pc + 1);
            if (idx >= p->const_count) { res.err = PSVM_ERR_BADOP; goto done; }
            uint8_t tag; float f = 0; const char *s = NULL; uint16_t slen = 0;
            const_entry(p, idx, &tag, &f, &s, &slen);
            value_t v;
            if (tag == 0) { v.tag = V_NUM; v.f = f; v.s = NULL; v.slen = 0; v.b = false; }
            else { v.tag = V_STR; v.s = s; v.slen = slen; v.f = 0; v.b = false; }
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            stack[sp++] = v;
            pc = (uint16_t)(pc + 3);
            break;
        }
        case 0x02: { /* LOAD_REF u16 */
            if ((size_t)pc + 3 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            uint16_t idx = rd_u16(p->code + pc + 1);
            if (idx >= p->ref_count) { res.err = PSVM_ERR_BADOP; goto done; }
            if (!resolved) { res.err = PSVM_ERR_REF; goto done; }
            psvm_ref_t r = psvm_get_ref(p, idx);
            const psvm_ref_val_t *rv = &resolved[idx];
            if (!rv->ready) { res.err = PSVM_ERR_REF; goto done; }
            value_t v;
            v.tag = V_NUM; v.s = NULL; v.slen = 0; v.b = false;
            v.f = (r.field == 1) ? (float)rv->age_s : rv->value;
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            stack[sp++] = v;
            pc = (uint16_t)(pc + 3);
            break;
        }
        case 0x10: case 0x11: case 0x12: case 0x13: { /* ADD SUB MUL DIV */
            if (sp < 2) { res.err = PSVM_ERR_STACK; goto done; }
            value_t b = stack[--sp], a = stack[--sp];
            if (a.tag != V_NUM || b.tag != V_NUM) { res.err = PSVM_ERR_TYPE; goto done; }
            float r;
            if (op == 0x10) r = a.f + b.f;
            else if (op == 0x11) r = a.f - b.f;
            else if (op == 0x12) r = a.f * b.f;
            else {
                if (b.f == 0.0f) { res.err = PSVM_ERR_DIV0; goto done; }
                r = a.f / b.f;
            }
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            value_t out = { .tag = V_NUM, .f = r, .s = NULL, .slen = 0, .b = false };
            stack[sp++] = out;
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: { /* LT LE GT GE EQ NE */
            if (sp < 2) { res.err = PSVM_ERR_STACK; goto done; }
            value_t b = stack[--sp], a = stack[--sp];
            if (a.tag != V_NUM || b.tag != V_NUM) { res.err = PSVM_ERR_TYPE; goto done; }
            bool r;
            switch (op) {
            case 0x20: r = a.f < b.f; break;
            case 0x21: r = a.f <= b.f; break;
            case 0x22: r = a.f > b.f; break;
            case 0x23: r = a.f >= b.f; break;
            case 0x24: r = a.f == b.f; break;
            default:   r = a.f != b.f; break;
            }
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            value_t out = { .tag = V_BOOL, .f = 0, .s = NULL, .slen = 0, .b = r };
            stack[sp++] = out;
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0x30: case 0x31: { /* AND OR */
            if (sp < 2) { res.err = PSVM_ERR_STACK; goto done; }
            value_t b = stack[--sp], a = stack[--sp];
            if (a.tag != V_BOOL || b.tag != V_BOOL) { res.err = PSVM_ERR_TYPE; goto done; }
            bool r = (op == 0x30) ? (a.b && b.b) : (a.b || b.b);
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            value_t out = { .tag = V_BOOL, .f = 0, .s = NULL, .slen = 0, .b = r };
            stack[sp++] = out;
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0x32: { /* NOT */
            if (sp < 1) { res.err = PSVM_ERR_STACK; goto done; }
            value_t a = stack[--sp];
            if (a.tag != V_BOOL) { res.err = PSVM_ERR_TYPE; goto done; }
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            value_t out = { .tag = V_BOOL, .f = 0, .s = NULL, .slen = 0, .b = !a.b };
            stack[sp++] = out;
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0x40: { /* JZ i16 (relative) */
            if ((size_t)pc + 3 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            if (sp < 1) { res.err = PSVM_ERR_STACK; goto done; }
            value_t c = stack[--sp];
            if (c.tag != V_BOOL) { res.err = PSVM_ERR_TYPE; goto done; }
            int16_t off = rd_i16(p->code + pc + 1);
            int32_t next = (int32_t)pc + 3;
            if (!c.b) {
                int32_t target = next + off;
                if (target < 0 || target >= (int32_t)p->code_len) { res.err = PSVM_ERR_JUMP; goto done; }
                pc = (uint16_t)target;
            } else {
                pc = (uint16_t)next;
            }
            break;
        }
        case 0x41: { /* JMP i16 (relative) */
            if ((size_t)pc + 3 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            int16_t off = rd_i16(p->code + pc + 1);
            int32_t next = (int32_t)pc + 3;
            int32_t target = next + off;
            if (target < 0 || target >= (int32_t)p->code_len) { res.err = PSVM_ERR_JUMP; goto done; }
            pc = (uint16_t)target;
            break;
        }
        case 0x50: { /* BUILD_STR u8 */
            if ((size_t)pc + 2 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            uint8_t n = p->code[pc + 1];
            if (sp < (int)n) { res.err = PSVM_ERR_STACK; goto done; }
            value_t tmp[PSVM_STACK];
            for (int i = (int)n - 1; i >= 0; i--) tmp[i] = stack[--sp];
            size_t o = 0;
            strbuf[0] = '\0';
            for (int i = 0; i < (int)n; i++) {
                value_t *v = &tmp[i];
                int written;
                size_t remain = (o < PSVM_STRBUF) ? (PSVM_STRBUF - o) : 0;
                if (v->tag == V_NUM)
                    written = snprintf(strbuf + o, remain, "%.1f", (double)v->f);
                else if (v->tag == V_BOOL)
                    written = snprintf(strbuf + o, remain, "%s", v->b ? "true" : "false");
                else
                    written = snprintf(strbuf + o, remain, "%.*s", (int)v->slen, v->s);
                if (written < 0) written = 0;
                o += (size_t)written;
                if (o >= PSVM_STRBUF) { o = PSVM_STRBUF - 1; break; }
            }
            value_t out;
            out.tag = V_STR; out.s = strbuf; out.slen = (uint16_t)strlen(strbuf);
            out.f = 0; out.b = false;
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            stack[sp++] = out;
            pc = (uint16_t)(pc + 2);
            break;
        }
        case 0x51: { /* CALL_BUILTIN u8 */
            if ((size_t)pc + 2 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            uint8_t b = p->code[pc + 1];
            if (sp < 1) { res.err = PSVM_ERR_STACK; goto done; }
            value_t s = stack[--sp];
            if (s.tag != V_STR) { res.err = PSVM_ERR_TYPE; goto done; }
            char msgbuf[PSVM_STRBUF];
            uint16_t l = s.slen;
            if (l >= PSVM_STRBUF) l = PSVM_STRBUF - 1;
            memcpy(msgbuf, s.s, l);
            msgbuf[l] = '\0';
            if (sink && !sink(sink_ctx, b, msgbuf)) { res.err = PSVM_ERR_TYPE; goto done; }
            pc = (uint16_t)(pc + 2);
            break;
        }
        case 0x00: { /* HALT_BOOL */
            if (sp < 1) { res.err = PSVM_ERR_STACK; goto done; }
            value_t c = stack[--sp];
            if (c.tag != V_BOOL) { res.err = PSVM_ERR_TYPE; goto done; }
            cond = c.b;
            pc = (uint16_t)(pc + 1);
            if (!run_actions || !cond) { res.err = PSVM_OK; goto done; }
            break;
        }
        /* ---- wrapper dialect (M3 spec section 3): payload accessors ---- */
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: { /* LOAD_U8/U16LE/U16BE/I16LE/I16BE/U24LE/U32LE u16 offset */
            if ((size_t)pc + 3 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            uint16_t off = rd_u16(p->code + pc + 1);
            uint8_t size = (op == 0x60) ? 1u
                         : (op == 0x65) ? 3u
                         : (op == 0x66) ? 4u
                         : 2u;
            if ((uint32_t)off + size > payload_len) { res.err = PSVM_ERR_REF; goto done; }
            const uint8_t *b = payload_buf + off;
            float f;
            switch (op) {
            case 0x60: f = (float)b[0]; break;
            case 0x61: f = (float)((uint16_t)b[0] | ((uint16_t)b[1] << 8)); break;
            case 0x62: f = (float)(((uint16_t)b[0] << 8) | (uint16_t)b[1]); break;
            case 0x63: f = (float)(int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8)); break;
            case 0x64: f = (float)(int16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]); break;
            case 0x65: f = (float)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16)); break;
            default:   f = (float)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24)); break;
            }
            value_t v = { .tag = V_NUM, .f = f, .s = NULL, .slen = 0, .b = false };
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            stack[sp++] = v;
            pc = (uint16_t)(pc + 3);
            break;
        }
        case 0x67: { /* LOAD_BITS u16 offset, u8 lsb, u8 width */
            if ((size_t)pc + 5 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            uint16_t off = rd_u16(p->code + pc + 1);
            uint8_t lsb = p->code[pc + 3];
            uint8_t width = p->code[pc + 4];
            if (width == 0 || width > 32 || (uint32_t)lsb + width > 32) { res.err = PSVM_ERR_BADOP; goto done; }
            uint8_t need = (uint8_t)((lsb + width + 7) / 8);
            if ((uint32_t)off + need > payload_len) { res.err = PSVM_ERR_REF; goto done; }
            uint32_t word = 0;
            for (uint8_t i = 0; i < need; i++) word |= ((uint32_t)payload_buf[off + i]) << (8u * i);
            uint32_t mask = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1u);
            uint32_t val = (word >> lsb) & mask;
            value_t v = { .tag = V_NUM, .f = (float)val, .s = NULL, .slen = 0, .b = false };
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            stack[sp++] = v;
            pc = (uint16_t)(pc + 5);
            break;
        }
        case 0x68: { /* PAYLOAD_LEN */
            value_t v = { .tag = V_NUM, .f = (float)payload_len, .s = NULL, .slen = 0, .b = false };
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            stack[sp++] = v;
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0x69: { /* EMIT u8 capability; pops value, buffers (see HALT) */
            if ((size_t)pc + 2 > p->code_len) { res.err = PSVM_ERR_BADOP; goto done; }
            uint8_t cap = p->code[pc + 1];
            if (sp < 1) { res.err = PSVM_ERR_STACK; goto done; }
            value_t v = stack[--sp];
            if (v.tag != V_NUM) { res.err = PSVM_ERR_TYPE; goto done; }
            if (emit_count >= PSVM_MAX_EMITS) { res.err = PSVM_ERR_LIMITS; goto done; }
            emit_buf[emit_count].cap = cap;
            emit_buf[emit_count].value = v.f;
            emit_count++;
            pc = (uint16_t)(pc + 2);
            break;
        }
        case 0x6A: { /* REQUIRE: pops bool; false ends the run at PSVM_OK
                      * WITHOUT reaching HALT, so the emit buffer (whatever
                      * it holds so far) is simply never flushed. */
            if (sp < 1) { res.err = PSVM_ERR_STACK; goto done; }
            value_t c = stack[--sp];
            if (c.tag != V_BOOL) { res.err = PSVM_ERR_TYPE; goto done; }
            if (!c.b) { res.err = PSVM_OK; goto done; }
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0x6B: { /* AES_CCM: pops len, offset (see psvm_aes_ccm_t's contract) */
            if (sp < 2) { res.err = PSVM_ERR_STACK; goto done; }
            value_t vlen = stack[--sp], voff = stack[--sp];
            if (vlen.tag != V_NUM || voff.tag != V_NUM) { res.err = PSVM_ERR_TYPE; goto done; }
            if (voff.f < 0 || voff.f > 255 || vlen.f < 1 || vlen.f > 255) { res.err = PSVM_ERR_REF; goto done; }
            uint16_t off = (uint16_t)voff.f, rlen = (uint16_t)vlen.f;
            /* The ciphertext+tag region must run exactly to the end of the
             * current working payload -- every real BLE encryption scheme
             * this VM targets (BTHome-style and otherwise) puts the tag
             * last, and requiring this means shrinking payload_len to
             * off+out_len below needs no byte-shifting. */
            if ((uint32_t)off + rlen != payload_len) { res.err = PSVM_ERR_REF; goto done; }
            if (!wio || !wio->aes_ccm) { res.err = PSVM_ERR_REF; goto done; }
            uint8_t out_len = 0;
            if (!wio->aes_ccm(wio->aes_ccm_ctx, (uint8_t)off, (uint8_t)rlen,
                              payload_buf, payload_len, &out_len) || out_len > rlen) {
                res.err = PSVM_ERR_REF; goto done;
            }
            payload_len = (uint8_t)(off + out_len);
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0x6C: { /* FLOOR: pops num; negative is PSVM_ERR_TYPE (a shape
                      * violation, not a value out of range -- FLOOR's whole
                      * contract assumes a non-negative bit-derived integer,
                      * same family of error as comparing two strings or
                      * using a bool where a number is expected). Used by
                      * `>>`'s codegen (x / 2^n then FLOOR) to make the
                      * right-shift idiom bit-exact instead of leaving a
                      * fractional remainder from the division; bits() is
                      * still the recommended, always-bit-exact idiom for
                      * extracting a sub-byte field directly. */
            if (sp < 1) { res.err = PSVM_ERR_STACK; goto done; }
            value_t v = stack[--sp];
            if (v.tag != V_NUM) { res.err = PSVM_ERR_TYPE; goto done; }
            if (v.f < 0.0f) { res.err = PSVM_ERR_TYPE; goto done; }
            value_t out = { .tag = V_NUM, .f = floorf(v.f), .s = NULL, .slen = 0, .b = false };
            if (sp >= PSVM_STACK) { res.err = PSVM_ERR_STACK; goto done; }
            stack[sp++] = out;
            pc = (uint16_t)(pc + 1);
            break;
        }
        case 0xFF: { /* HALT -- only place a wrapper's buffered emits are flushed */
            for (uint8_t i = 0; i < emit_count; i++) {
                if (wio && wio->emit) wio->emit(wio->emit_ctx, emit_buf[i].cap, emit_buf[i].value);
            }
            res.err = PSVM_OK;
            goto done;
        }
        default:
            res.err = PSVM_ERR_BADOP;
            goto done;
        }
    }

done:
    res.cond = cond;
    res.steps_used = steps;
    return res;
}
