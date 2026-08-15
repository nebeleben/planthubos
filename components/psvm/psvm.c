/* psvm.c -- PSBC v1 bytecode validator + interpreter.
 * Pure C99 + libc, no ESP-IDF includes (host-testable, spec section 2/3). */
#include "psvm.h"
#include <string.h>
#include <stdio.h>

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

psvm_err_t psvm_validate(const uint8_t *blob, size_t len, uint8_t caps_max,
                         uint32_t builtins_impl, psvm_prog_t *out) {
    if (len < PSVM_HEADER_LEN) return PSVM_ERR_TRUNCATED;
    if (memcmp(blob, "PSBC", 4) != 0) return PSVM_ERR_HEADER;
    if (blob[4] != PSVM_FMT_VER || blob[5] != PSVM_DIALECT_RULES) return PSVM_ERR_HEADER;
    uint16_t flags = rd_u16(blob + 6);
    if (flags != 0) return PSVM_ERR_HEADER;

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

psvm_result_t psvm_run(const psvm_prog_t *p, const psvm_ref_val_t *resolved,
                       psvm_sink_t sink, void *sink_ctx, bool run_actions) {
    psvm_result_t res = {false, PSVM_OK, 0};
    value_t stack[PSVM_STACK];
    int sp = 0;
    char strbuf[PSVM_STRBUF];
    uint32_t steps = 0;
    uint16_t pc = 0;
    bool cond = false;

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
        case 0xFF: /* HALT */
            res.err = PSVM_OK;
            goto done;
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
