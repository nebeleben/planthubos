#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "psvm.h"
#include "action.h"

/* --- minimal PSBC builder: header + consts + refs + code --- */
static size_t emit_header_d(uint8_t *b, uint8_t dialect, uint32_t builtins, uint16_t nconst,
                            uint16_t nref, uint16_t codelen) {
    memcpy(b, "PSBC", 4); b[4] = 1; b[5] = dialect; b[6] = b[7] = 0;
    memcpy(b + 8, &builtins, 4);
    memcpy(b + 12, &nconst, 2); memcpy(b + 14, &nref, 2); memcpy(b + 16, &codelen, 2);
    return 18;
}
static size_t emit_header(uint8_t *b, uint32_t builtins, uint16_t nconst,
                          uint16_t nref, uint16_t codelen) {
    return emit_header_d(b, PSVM_DIALECT_RULES, builtins, nconst, nref, codelen);
}
static size_t emit_f32(uint8_t *b, size_t o, float v) { b[o] = 0; memcpy(b+o+1, &v, 4); return o+5; }
static size_t emit_str(uint8_t *b, size_t o, const char *s) {
    uint16_t l = (uint16_t)strlen(s); b[o] = 1; memcpy(b+o+1, &l, 2); memcpy(b+o+3, s, l); return o+3+l;
}
static size_t emit_ref(uint8_t *b, size_t o, uint8_t kind, uint16_t name_const,
                       uint8_t cap, uint8_t field) {
    b[o]=kind; memcpy(b+o+1,&name_const,2); b[o+3]=cap; b[o+4]=field; return o+5;
}
static size_t emit_push_const(uint8_t *b, size_t o, uint16_t idx) {
    b[o] = 0x01; memcpy(b+o+1, &idx, 2); return o+3;
}
static size_t emit_i16(uint8_t *b, size_t o, uint8_t op, int16_t rel) {
    b[o] = op; memcpy(b+o+1, &rel, 2); return o+3;
}
/* --- wrapper-dialect (0x60-0x6B) opcode helpers --- */
static size_t emit_op(uint8_t *b, size_t o, uint8_t op) { b[o] = op; return o + 1; }
static size_t emit_op_u8(uint8_t *b, size_t o, uint8_t op, uint8_t v) { b[o] = op; b[o+1] = v; return o + 2; }
static size_t emit_op_u16(uint8_t *b, size_t o, uint8_t op, uint16_t v) {
    b[o] = op; memcpy(b+o+1, &v, 2); return o + 3;
}
static size_t emit_load_bits(uint8_t *b, size_t o, uint16_t off, uint8_t lsb, uint8_t width) {
    b[o] = 0x67; memcpy(b+o+1, &off, 2); b[o+3] = lsb; b[o+4] = width; return o + 5;
}

typedef struct { char last[600]; uint8_t last_builtin; int calls; } sink_cap_t;
static bool cap_sink(void *ctx, uint8_t builtin, const char *msg) {
    sink_cap_t *c = ctx; c->last_builtin = builtin;
    snprintf(c->last, sizeof c->last, "%s", msg); c->calls++; return true;
}

/* --- wrapper-dialect emit-sink capture --- */
typedef struct { struct { uint8_t cap; float value; } items[PSVM_MAX_EMITS]; int count; } emit_cap_t;
static void emit_capture(void *ctx, uint8_t capability, float value) {
    emit_cap_t *c = ctx;
    if (c->count < PSVM_MAX_EMITS) {
        c->items[c->count].cap = capability;
        c->items[c->count].value = value;
        c->count++;
    }
}

/* One accessor opcode (offset baked in at compile time, spec §3) reading
 * the runtime payload and EMITting it under capability 0: LOAD_<op> off;
 * EMIT 0; HALT. */
static size_t build_w_load(uint8_t *b, uint8_t op, uint16_t offset) {
    uint8_t code[16]; size_t co = 0;
    co = emit_op_u16(code, co, op, offset);
    co = emit_op_u8(code, co, 0x69, 0);
    co = emit_op(code, co, 0xFF);
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)co);
    memcpy(b + o, code, co);
    return o + co;
}

/* LOAD_BITS off,lsb,width; EMIT 0; HALT. */
static size_t build_w_load_bits(uint8_t *b, uint16_t off, uint8_t lsb, uint8_t width) {
    uint8_t code[16]; size_t co = 0;
    co = emit_load_bits(code, co, off, lsb, width);
    co = emit_op_u8(code, co, 0x69, 0);
    co = emit_op(code, co, 0xFF);
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)co);
    memcpy(b + o, code, co);
    return o + co;
}

/* PAYLOAD_LEN; EMIT 0; HALT. */
static size_t build_w_payload_len(uint8_t *b) {
    uint8_t code[8]; size_t co = 0;
    co = emit_op(code, co, 0x68);
    co = emit_op_u8(code, co, 0x69, 0);
    co = emit_op(code, co, 0xFF);
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)co);
    memcpy(b + o, code, co);
    return o + co;
}

/* LOAD_<op> offset; PUSH_CONST divisor; DIV; FLOOR; EMIT 0; HALT -- the
 * exact codegen shape codegen.js's 'shr' case emits for `x >> n` (spec §3
 * as amended: bit-exact, DIV by 2^n then FLOOR). */
static size_t build_w_shift(uint8_t *b, uint8_t load_op, uint16_t offset, float divisor) {
    uint8_t code[24]; size_t co = 0;
    co = emit_op_u16(code, co, load_op, offset);
    co = emit_op_u16(code, co, 0x01, 0);   /* PUSH_CONST 0 (divisor) */
    co = emit_op(code, co, 0x13);          /* DIV */
    co = emit_op(code, co, 0x6C);          /* FLOOR */
    co = emit_op_u8(code, co, 0x69, 0);    /* EMIT 0 */
    co = emit_op(code, co, 0xFF);
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 1, 0, (uint16_t)co);
    o = emit_f32(b, o, divisor);
    memcpy(b + o, code, co);
    return o + co;
}

/* PUSH_CONST(-5.0); FLOOR; EMIT 0; HALT -- FLOOR on a negative operand. */
static size_t build_w_floor_neg(uint8_t *b) {
    uint8_t code[16]; size_t co = 0;
    co = emit_op_u16(code, co, 0x01, 0);   /* PUSH_CONST 0 (-5.0) */
    co = emit_op(code, co, 0x6C);          /* FLOOR */
    co = emit_op_u8(code, co, 0x69, 0);
    co = emit_op(code, co, 0xFF);
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 1, 0, (uint16_t)co);
    o = emit_f32(b, o, -5.0f);
    memcpy(b + o, code, co);
    return o + co;
}

/* Program exercising "require after emit emits nothing" (spec §3):
 *   PUSH_CONST 1.0 ; EMIT 7            -- an emit BEFORE the require
 *   PUSH_CONST 0.0 ; PUSH_CONST 0.0 ; NE   -- false
 *   REQUIRE                             -- pops false: ends the run here
 *   PUSH_CONST 1.0 ; EMIT 6 ; HALT      -- unreachable
 * cond_true selects EQ instead of NE for the require expr's second operand
 * relationship, producing a require that PASSES (both the early emit and
 * the later one reach the sink) -- the positive control. Second EMIT's
 * capability is 6 (not 8, as an earlier version of this test used) -- every
 * caller below validates with caps_max=7 (0..7 valid), and EMIT's operand
 * is now range-checked at validate time too (Task 5 review round 2); 6 and
 * 7 are still two distinct, in-range ids, which is all this test needs. */
static size_t build_w_require(uint8_t *b, bool cond_true) {
    uint8_t code[32]; size_t co = 0;
    co = emit_op_u16(code, co, 0x01, 0);      /* PUSH_CONST 0 (1.0) */
    co = emit_op_u8(code, co, 0x69, 7);       /* EMIT cap=7 */
    co = emit_op_u16(code, co, 0x01, 1);      /* PUSH_CONST 1 (0.0) */
    co = emit_op_u16(code, co, 0x01, 1);      /* PUSH_CONST 1 (0.0) */
    co = emit_op(code, co, cond_true ? 0x24 : 0x25); /* EQ (true) or NE (false) */
    co = emit_op(code, co, 0x6A);             /* REQUIRE */
    co = emit_op_u16(code, co, 0x01, 0);      /* PUSH_CONST 0 (1.0) */
    co = emit_op_u8(code, co, 0x69, 6);       /* EMIT cap=6 */
    co = emit_op(code, co, 0xFF);             /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 2, 0, (uint16_t)co);
    o = emit_f32(b, o, 1.0f); o = emit_f32(b, o, 0.0f);
    memcpy(b + o, code, co);
    return o + co;
}

/* AES_CCM offset,len (both pushed) then a LOAD_U8 at `readOffset` and EMIT
 * 0, then a second LOAD_U8 at `readOffset2` (used to probe the shrunk
 * payload length post-decrypt) EMIT 1, then HALT -- lets one run assert
 * both "subsequent accessors read plaintext" and "payload_len actually
 * shrunk by whatever the callback stripped". */
static size_t build_w_aes_ccm(uint8_t *b, uint16_t offset, uint16_t len) {
    uint8_t code[32]; size_t co = 0;
    co = emit_op_u16(code, co, 0x01, 0);   /* PUSH_CONST 0 (offset) */
    co = emit_op_u16(code, co, 0x01, 1);   /* PUSH_CONST 1 (len) */
    co = emit_op(code, co, 0x6B);          /* AES_CCM */
    co = emit_op_u16(code, co, 0x60, offset);       /* LOAD_U8 offset */
    co = emit_op_u8(code, co, 0x69, 0);             /* EMIT 0 */
    co = emit_op(code, co, 0x68);                   /* PAYLOAD_LEN */
    co = emit_op_u8(code, co, 0x69, 1);             /* EMIT 1 */
    co = emit_op(code, co, 0xFF);
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 2, 0, (uint16_t)co);
    o = emit_f32(b, o, (float)offset); o = emit_f32(b, o, (float)len);
    memcpy(b + o, code, co);
    return o + co;
}

/* Fake AES-CCM decrypt used by the AES_CCM opcode test: XORs each
 * ciphertext byte with 0xFF ("plaintext"), always keeps only the first
 * two bytes of the region as plaintext (simulating a 4-byte trailing tag
 * on a 6-byte region) so the test can assert payload_len actually shrinks. */
static bool fake_aes_ccm(void *ctx, uint8_t offset, uint8_t len,
                         uint8_t *payload, uint8_t payload_len, uint8_t *out_len) {
    (void)ctx; (void)payload_len;
    if (len < 2) return false;
    payload[offset]   = (uint8_t)(payload[offset] ^ 0xFF);
    payload[offset+1] = (uint8_t)(payload[offset+1] ^ 0xFF);
    *out_len = 2;
    return true;
}
static bool failing_aes_ccm(void *ctx, uint8_t offset, uint8_t len,
                            uint8_t *payload, uint8_t payload_len, uint8_t *out_len) {
    (void)ctx; (void)offset; (void)len; (void)payload; (void)payload_len; (void)out_len;
    return false;
}

/* Program: when ref0 < 22 then notify("dry: {ref0}%")  (spec section 2 shape) */
static size_t build_demo(uint8_t *b) {
    /* consts: 0=22.0f, 1="dry: ", 2="%" ; refs: 0 = plant name-const 1?? -- name
       unused by the VM itself, use const 1 arbitrarily */
    uint8_t code[] = {
        0x02, 0,0,          /* LOAD_REF 0 */
        0x01, 0,0,          /* PUSH_CONST 0 (22.0) */
        0x20,               /* LT */
        0x00,               /* HALT_BOOL */
        0x01, 1,0,          /* PUSH_CONST 1 "dry: " */
        0x02, 0,0,          /* LOAD_REF 0 */
        0x01, 2,0,          /* PUSH_CONST 2 "%" */
        0x50, 3,            /* BUILD_STR 3 */
        0x51, 1,            /* CALL_BUILTIN notify */
        0xFF                /* HALT */
    };
    size_t o = emit_header(b, 0x2, 3, 1, sizeof code);
    o = emit_f32(b, o, 22.0f); o = emit_str(b, o, "dry: "); o = emit_str(b, o, "%");
    o = emit_ref(b, o, 0, 1, 0, 0);
    memcpy(b + o, code, sizeof code);
    return o + sizeof code;
}

/* Program: ref0.age > 60 */
static size_t build_age(uint8_t *b) {
    uint8_t code[] = {
        0x02, 0,0,   /* LOAD_REF 0 (field=age) */
        0x01, 0,0,   /* PUSH_CONST 0 (60.0) */
        0x22,        /* GT */
        0x00,        /* HALT_BOOL */
        0xFF,        /* HALT */
    };
    size_t o = emit_header(b, 0, 2, 1, sizeof code);
    o = emit_f32(b, o, 60.0f);
    o = emit_str(b, o, "x");
    o = emit_ref(b, o, 0, 1, 0, 1); /* field=1 -> age */
    memcpy(b + o, code, sizeof code);
    return o + sizeof code;
}

/* Program: (ref0 > 0) <AND|OR> (ref1 > 0), or NOT(ref0 > 0) when ref1 unused (nrefs=1). */
static size_t build_bool2(uint8_t *b, uint8_t combine_op /* 0x30 AND, 0x31 OR, 0 for NOT */) {
    uint8_t code[64];
    size_t co = 0;
    code[co++] = 0x02; code[co++] = 0; code[co++] = 0; /* LOAD_REF 0 */
    code[co++] = 0x01; code[co++] = 0; code[co++] = 0; /* PUSH_CONST 0 (0.0) */
    code[co++] = 0x22;                                  /* GT */
    if (combine_op == 0x30 || combine_op == 0x31) {
        code[co++] = 0x02; code[co++] = 1; code[co++] = 0; /* LOAD_REF 1 */
        code[co++] = 0x01; code[co++] = 0; code[co++] = 0; /* PUSH_CONST 0 (0.0) */
        code[co++] = 0x22;                                  /* GT */
        code[co++] = combine_op;                            /* AND/OR */
    } else {
        code[co++] = 0x32; /* NOT */
    }
    code[co++] = 0x00; /* HALT_BOOL */
    code[co++] = 0xFF; /* HALT */

    uint16_t nref = (combine_op == 0x30 || combine_op == 0x31) ? 2 : 1;
    size_t o = emit_header(b, 0, 2, nref, (uint16_t)co);
    o = emit_f32(b, o, 0.0f);
    o = emit_str(b, o, "x");
    o = emit_ref(b, o, 0, 1, 0, 0);
    if (nref == 2) o = emit_ref(b, o, 0, 1, 0, 0);
    memcpy(b + o, code, co);
    return o + co;
}

/* Program: two strings compared with EQ -> PSVM_ERR_TYPE at runtime. */
static size_t build_eq_str(uint8_t *b) {
    uint8_t code[] = {
        0x01, 0,0,  /* PUSH_CONST 0 "a" */
        0x01, 1,0,  /* PUSH_CONST 1 "b" */
        0x24,       /* EQ */
        0x00,       /* HALT_BOOL */
        0xFF,
    };
    size_t o = emit_header(b, 0, 2, 0, sizeof code);
    o = emit_str(b, o, "a"); o = emit_str(b, o, "b");
    memcpy(b + o, code, sizeof code);
    return o + sizeof code;
}

/* Program: 1.0 / 0.0 -> PSVM_ERR_DIV0. */
static size_t build_div0(uint8_t *b) {
    uint8_t code[] = {
        0x01, 0,0,  /* PUSH_CONST 0 (1.0) */
        0x01, 1,0,  /* PUSH_CONST 1 (0.0) */
        0x13,       /* DIV */
        0x00,       /* HALT_BOOL (unreachable) */
        0xFF,
    };
    size_t o = emit_header(b, 0, 2, 0, sizeof code);
    o = emit_f32(b, o, 1.0f); o = emit_f32(b, o, 0.0f);
    memcpy(b + o, code, sizeof code);
    return o + sizeof code;
}

/* Program: single JMP whose target lands past code_len -> PSVM_ERR_JUMP. */
static size_t build_jmp_oob(uint8_t *b) {
    uint8_t code[3];
    size_t co = emit_i16(code, 0, 0x41, 1000); /* JMP +1000, way past code_len=3 */
    size_t o = emit_header(b, 0, 0, 0, (uint16_t)co);
    memcpy(b + o, code, co);
    return o + co;
}

/* Program: single JMP -3 jumping to itself -> infinite loop -> PSVM_ERR_STEPS. */
static size_t build_jmp_loop(uint8_t *b) {
    uint8_t code[3];
    size_t co = emit_i16(code, 0, 0x41, -3);
    size_t o = emit_header(b, 0, 0, 0, (uint16_t)co);
    memcpy(b + o, code, co);
    return o + co;
}

/* Program: 33 PUSH_CONST in a row -> stack overflow (PSVM_STACK==32). */
static size_t build_stack_overflow(uint8_t *b) {
    uint8_t code[33 * 3];
    size_t co = 0;
    for (int i = 0; i < 33; i++) co = emit_push_const(code, co, 0);
    size_t o = emit_header(b, 0, 1, 0, (uint16_t)co);
    o = emit_f32(b, o, 1.0f);
    memcpy(b + o, code, co);
    return o + co;
}

/* Program: LOAD_REF0 <op> PUSH_CONST(threshold) -> HALT_BOOL (used for LE/GE/NE truth checks). */
static size_t build_cmp1(uint8_t *b, uint8_t cmp_op, float threshold) {
    uint8_t code[] = {
        0x02, 0,0,      /* LOAD_REF 0 */
        0x01, 0,0,      /* PUSH_CONST 0 (threshold) */
        cmp_op,
        0x00,           /* HALT_BOOL */
        0xFF,
    };
    size_t o = emit_header(b, 0, 2, 1, sizeof code);
    o = emit_f32(b, o, threshold);
    o = emit_str(b, o, "x");
    o = emit_ref(b, o, 0, 1, 0, 0);
    memcpy(b + o, code, sizeof code);
    return o + sizeof code;
}

/* Program: ((ref0 <op> const) == expected) -> HALT_BOOL; verifies ADD/SUB/MUL/DIV results. */
static size_t build_arith(uint8_t *b, uint8_t arith_op, float refval, float constval, float expected) {
    uint8_t code[] = {
        0x02, 0,0,      /* LOAD_REF 0 */
        0x01, 1,0,      /* PUSH_CONST 1 (constval) */
        arith_op,
        0x01, 2,0,      /* PUSH_CONST 2 (expected) */
        0x24,           /* EQ */
        0x00,           /* HALT_BOOL */
        0xFF,
    };
    size_t o = emit_header(b, 0, 3, 1, sizeof code);
    o = emit_str(b, o, "x");
    o = emit_f32(b, o, constval);
    o = emit_f32(b, o, expected);
    o = emit_ref(b, o, 0, 0, 0, 0);
    memcpy(b + o, code, sizeof code);
    (void)refval;
    return o + sizeof code;
}

/* Program exercising JZ/JMP control flow: if ref0>0 push GT(1.0,0.0)=true branch,
 * else push GT(0.0,1.0)=false branch, then HALT_BOOL. */
static size_t build_branch2(uint8_t *b) {
    uint8_t code[28];
    size_t co = 0;
    code[co++] = 0x02; code[co++] = 0; code[co++] = 0; /* 0: LOAD_REF 0 */
    code[co++] = 0x01; code[co++] = 0; code[co++] = 0; /* 3: PUSH_CONST 0 (0.0) */
    code[co++] = 0x22;                                  /* 6: GT */
    size_t jz_at = co;
    co = emit_i16(code, co, 0x40, 0);                   /* 7: JZ */
    code[co++] = 0x01; code[co++] = 1; code[co++] = 0; /* 10: PUSH_CONST 1 (1.0) */
    code[co++] = 0x01; code[co++] = 0; code[co++] = 0; /* 13: PUSH_CONST 0 (0.0) */
    code[co++] = 0x22;                                  /* 16: GT */
    size_t jmp_at = co;
    co = emit_i16(code, co, 0x41, 0);                   /* 17: JMP */
    size_t else_pc = co;
    code[co++] = 0x01; code[co++] = 0; code[co++] = 0; /* 20: PUSH_CONST 0 (0.0) */
    code[co++] = 0x01; code[co++] = 1; code[co++] = 0; /* 23: PUSH_CONST 1 (1.0) */
    code[co++] = 0x22;                                  /* 26: GT */
    size_t end_pc = co;
    code[co++] = 0x00; /* HALT_BOOL */

    int16_t jz_off = (int16_t)((int32_t)else_pc - (int32_t)(jz_at + 3));
    memcpy(code + jz_at + 1, &jz_off, 2);
    int16_t jmp_off = (int16_t)((int32_t)end_pc - (int32_t)(jmp_at + 3));
    memcpy(code + jmp_at + 1, &jmp_off, 2);

    size_t o = emit_header(b, 0, 3, 1, (uint16_t)co);
    o = emit_f32(b, o, 0.0f);   /* const0 */
    o = emit_f32(b, o, 1.0f);   /* const1 */
    o = emit_str(b, o, "x");    /* const2 -- ref name */
    o = emit_ref(b, o, 0, 2, 0, 0);
    memcpy(b + o, code, co);
    return o + co;
}

/* Program: log("hello") unconditionally (cond always true). */
static size_t build_log(uint8_t *b) {
    uint8_t code[] = {
        0x01, 1,0,  /* PUSH_CONST 1 (1.0) */
        0x01, 0,0,  /* PUSH_CONST 0 (0.0) */
        0x22,       /* GT -> true */
        0x00,       /* HALT_BOOL */
        0x01, 2,0,  /* PUSH_CONST 2 "hello" */
        0x51, 0,    /* CALL_BUILTIN 0 (log) */
        0xFF,
    };
    size_t o = emit_header(b, 0x1, 3, 0, sizeof code);
    o = emit_f32(b, o, 0.0f); o = emit_f32(b, o, 1.0f); o = emit_str(b, o, "hello");
    memcpy(b + o, code, sizeof code);
    return o + sizeof code;
}

/* Header-only blob asserting a limits violation without a well-formed body. */
static size_t build_limits_header(uint8_t *b, uint16_t nconst, uint16_t nref) {
    return emit_header(b, 0, nconst, nref, 0);
}

/* --- M5a: PSBC trailing connect-plan section builders --- */

/* Dialect=2 (wrapper) blob with PSVM_FLAG_CONNECT_PLAN set and a trailing
 * connect-plan section (psvm.h's PSVM_FLAG_CONNECT_PLAN doc comment has the
 * on-blob layout, including why interval_s is u32 rather than u16). Code
 * body is a bare HALT -- this builder's whole point is the plan section
 * that follows it, not the code that precedes it. write_data/write_lens
 * are parallel write_count-length arrays: write i's declared payload is
 * write_lens[i] bytes, each byte equal to write_data[i] (every M5a test
 * case here uses write_lens[i]==1, where this reduces to exactly "the one
 * byte you asked for"). bufsz is accepted to match the brief's call shape;
 * every case below fits comfortably so it is unused. */
static size_t build_wrapper_with_plan(uint8_t *b, size_t bufsz,
                                      uint8_t read_count, uint16_t *reads,
                                      uint8_t write_count, uint16_t *write_uuids,
                                      uint8_t *write_data, uint8_t *write_lens,
                                      uint32_t interval_s) {
    (void)bufsz;
    uint8_t code[1] = { 0xFF };   /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)sizeof code);
    memcpy(b + o, code, sizeof code);
    o += sizeof code;

    uint16_t flags = PSVM_FLAG_CONNECT_PLAN;
    memcpy(b + 6, &flags, 2);

    b[o++] = read_count;
    b[o++] = write_count;
    memcpy(b + o, &interval_s, 4); o += 4;
    for (uint8_t i = 0; i < read_count; i++) {
        memcpy(b + o, &reads[i], 2); o += 2;
        /* min_len: 2 bytes, what an i16_le/u16_le accessor at offset 0
         * needs. Every read here is a 2-byte characteristic. */
        b[o++] = 2;
    }
    for (uint8_t i = 0; i < write_count; i++) {
        memcpy(b + o, &write_uuids[i], 2); o += 2;
        b[o++] = write_lens[i];
        for (uint8_t j = 0; j < write_lens[i]; j++) b[o++] = write_data[i];
    }
    return o;
}

/* Dialect=1 (rules) header-only blob with PSVM_FLAG_CONNECT_PLAN set --
 * used to prove the validator refuses a plan on a rules blob outright, at
 * the header, before parsing anything past it (a rules program has no
 * radio). */
static size_t build_rules_blob_with_plan_flag(uint8_t *b, size_t bufsz) {
    (void)bufsz;
    size_t o = emit_header(b, 0, 0, 0, 0);   /* dialect=RULES, empty body */
    uint16_t flags = PSVM_FLAG_CONNECT_PLAN;
    memcpy(b + 6, &flags, 2);
    return o;
}

/* A dialect-2 blob with no plan flag must validate exactly as before and
 * report no plan -- this is the M3/M4 compatibility guarantee. */
static void test_no_plan_flag_unchanged(void) {
    uint8_t blob[64];
    size_t n = build_w_payload_len(blob);   /* existing helper: flags=0 wrapper blob */
    psvm_prog_t p;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_OK);
    assert(p.plan == NULL && p.plan_len == 0);
}

/* Flag set, well-formed plan: pointer and length exposed. */
static void test_plan_parsed(void) {
    uint8_t blob[128];
    size_t n = build_wrapper_with_plan(blob, sizeof blob,
        /*reads*/  2, (uint16_t[]){0x2A6E, 0x2A6F},
        /*writes*/ 1, (uint16_t[]){0x2A00}, (uint8_t[]){0x01}, (uint8_t[]){1},
        /*interval*/ 600);
    psvm_prog_t p;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_OK);
    assert(p.plan != NULL);
    assert(p.plan[0] == 2 && p.plan[1] == 1);
}

/* Flag set but the section is missing or short. */
static void test_plan_truncated(void) {
    uint8_t blob[128];
    size_t n = build_wrapper_with_plan(blob, sizeof blob, 2,
        (uint16_t[]){0x2A6E, 0x2A6F}, 0, NULL, NULL, NULL, 600);
    psvm_prog_t p;
    assert(psvm_validate(blob, n - 1, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_TRUNCATED);
}

/* Every cap, at the boundary. */
static void test_plan_limits(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_plan(blob, sizeof blob, 5,
        (uint16_t[]){1,2,3,4,5}, 0, NULL, NULL, NULL, 600);          /* 5 reads */
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);
    n = build_wrapper_with_plan(blob, sizeof blob, 1, (uint16_t[]){1},
        3, (uint16_t[]){1,2,3}, (uint8_t[]){1,1,1}, (uint8_t[]){1,1,1}, 600); /* 3 writes */
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);
    n = build_wrapper_with_plan(blob, sizeof blob, 1, (uint16_t[]){1},
        0, NULL, NULL, NULL, 30);                                     /* interval too short */
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);
}

/* interval_s's exact boundary, both ends: 59 and 86401 rejected, 60 and
 * 86400 accepted. The upper end is the case that motivated widening the
 * field to u32 (psvm.h's PSVM_FLAG_CONNECT_PLAN doc comment has the full
 * reasoning) -- without this test, a future narrowing of the field back to
 * u16 would silently truncate 86400 to 20864 and this suite would not
 * notice. */
static void test_plan_interval_bounds(void) {
    uint8_t blob[128]; psvm_prog_t p;

    size_t n = build_wrapper_with_plan(blob, sizeof blob, 1, (uint16_t[]){1},
        0, NULL, NULL, NULL, 59);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);

    n = build_wrapper_with_plan(blob, sizeof blob, 1, (uint16_t[]){1},
        0, NULL, NULL, NULL, 86401);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);

    n = build_wrapper_with_plan(blob, sizeof blob, 1, (uint16_t[]){1},
        0, NULL, NULL, NULL, 60);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_OK);

    n = build_wrapper_with_plan(blob, sizeof blob, 1, (uint16_t[]){1},
        0, NULL, NULL, NULL, 86400);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_OK);
}

/* read_count 0 validates as a plan the engine will never run but the
 * scheduler never gates, so every advertisement retries it forever. */
static void test_plan_zero_reads_rejected(void) {
    uint8_t blob[128]; psvm_prog_t p;
    size_t n = build_wrapper_with_plan(blob, sizeof blob, 0, NULL,
                                       0, NULL, NULL, NULL, 600);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);
}

/* min_len must be 1..PSVM_PLAN_SLOT: 0 would let an empty response count as
 * a successful read, and > a slot could never be satisfied. */
static void test_plan_min_len_bounds(void) {
    uint8_t blob[128]; psvm_prog_t p;
    size_t n = build_wrapper_with_plan(blob, sizeof blob, 1, (uint16_t[]){0x2A6E},
                                       0, NULL, NULL, NULL, 600);
    /* The min_len byte is the last of the single read entry. */
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_OK);
    blob[n - 1] = 0;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);
    blob[n - 1] = PSVM_PLAN_SLOT + 1;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_ERR_LIMITS);
    blob[n - 1] = PSVM_PLAN_SLOT;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 7, 0, &p) == PSVM_OK);
}

/* A dialect-1 (rules) blob may never carry a plan. */
static void test_plan_rejected_for_rules(void) {
    uint8_t blob[128];
    size_t n = build_rules_blob_with_plan_flag(blob, sizeof blob);
    psvm_prog_t p;
    assert(psvm_validate(blob, n, PSVM_DIALECT_RULES, 7, 0, &p) == PSVM_ERR_HEADER);
}

/* --- M5b: PSBC trailing action-table section builders --- */

/* One action entry (no confirm, no action_count byte): write a single
 * constant byte to 0x2AF0, that same byte doubling as a u8 parameter slot
 * at offset 0 (psvm.h's PSVM_FLAG_ACTION_TABLE doc comment has the full
 * on-blob layout, including the param_offset/param_encoding-must-fit-
 * write_len invariant this shape satisfies: offset 0 + width 1 <=
 * write_len 1). flags here is the action entry's OWN flags byte (bit 0
 * device-local timed-off, bit 1 has confirm) -- not to be confused with
 * the blob header's flags word, which the blob-level builders below set
 * separately. Factored out of append_action_section so
 * build_wrapper_with_duplicate_action_ids can place two of these under one
 * action_count without duplicating the byte layout. */
static size_t append_one_action_entry(uint8_t *b, size_t o, uint8_t action_id,
                                      uint16_t param_max, uint8_t flags) {
    b[o++] = action_id;
    b[o++] = (uint8_t)(param_max & 0xFF);
    b[o++] = (uint8_t)(param_max >> 8);
    b[o++] = flags;
    b[o++] = 0xF0; b[o++] = 0x2A;   /* write uuid16 0x2AF0 */
    b[o++] = 1;                     /* write_len */
    b[o++] = 0x01;                  /* constant byte / u8 param slot */
    b[o++] = 0;                     /* param_offset: byte 0 of write_bytes */
    b[o++] = 0;                     /* param_encoding: 0 = u8 */
    return o;
}

/* action_count=1 wrapping a single append_one_action_entry. */
static size_t append_action_section(uint8_t *b, size_t o, uint8_t action_id,
                                    uint16_t param_max, uint8_t flags) {
    b[o++] = 1;                 /* action_count */
    return append_one_action_entry(b, o, action_id, param_max, flags);
}

/* Dialect=2 (wrapper) blob with PSVM_FLAG_ACTION_TABLE set and a trailing
 * one-action, no-confirm action section appended after a bare HALT code
 * body -- this builder's whole point is the action section, not the code
 * preceding it (same shape as build_wrapper_with_plan). */
static size_t build_wrapper_with_action(uint8_t *b, size_t bufsz,
                                        uint8_t action_id, uint16_t param_max,
                                        uint8_t flags) {
    (void)bufsz;
    uint8_t code[1] = { 0xFF };   /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)sizeof code);
    memcpy(b + o, code, sizeof code);
    o += sizeof code;

    uint16_t hflags = PSVM_FLAG_ACTION_TABLE;
    memcpy(b + 6, &hflags, 2);

    return append_action_section(b, o, action_id, param_max, flags);
}

/* Dialect=1 (rules) header-only blob with PSVM_FLAG_ACTION_TABLE set --
 * proves the validator refuses an action table on a rules blob outright, at
 * the header, before parsing anything past it (same reasoning as
 * build_rules_blob_with_plan_flag: a rules program has no actuators). */
static size_t build_rules_blob_with_action_flag(uint8_t *b, size_t bufsz) {
    (void)bufsz;
    size_t o = emit_header(b, 0, 0, 0, 0);   /* dialect=RULES, empty body */
    uint16_t flags = PSVM_FLAG_ACTION_TABLE;
    memcpy(b + 6, &flags, 2);
    return o;
}

static void test_action_section_parsed(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_action(blob, sizeof blob,
                                         ACT_IRRIGATION_OPEN, 300, 0x01);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);
    assert(p.actions != NULL);
    assert(p.actions[0] == 1);
}

/* The bound may only be LOWERED. This is the single most important check in
 * the validator: M4 lets an AI write wrapper source, so a generated wrapper
 * must not be able to raise a safety bound. */
static void test_action_param_max_cannot_exceed_firmware(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_action(blob, sizeof blob, ACT_IRRIGATION_OPEN, 301, 0x01);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);

    n = build_wrapper_with_action(blob, sizeof blob, ACT_IRRIGATION_OPEN, 60, 0x01);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);
}

static void test_action_unknown_id_rejected(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_action(blob, sizeof blob, ACTION_COUNT, 0, 0x01);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);
}

/* Truncation at EVERY boundary, not just the last one. M5a's gate proved a
 * validator that trusts its counts reads past its buffer. */
static void test_action_section_truncated(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_action(blob, sizeof blob, ACT_IRRIGATION_OPEN, 300, 0x01);
    for (size_t cut = 1; cut < 12; cut++) {
        assert(psvm_validate(blob, n - cut, PSVM_DIALECT_WRAPPERS, 8, 0, &p)
               != PSVM_OK);
    }
}

/* A rules blob may never carry an action section. */
static void test_action_section_rejected_for_rules(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_rules_blob_with_action_flag(blob, sizeof blob);
    assert(psvm_validate(blob, n, PSVM_DIALECT_RULES, 8, 0, &p) == PSVM_ERR_HEADER);
}

/* --- M5b fix round 1: param_offset/param_encoding, confirm block, both
 * section flags together, duplicate action_id, reserved per-action flag
 * bits --- */

/* Wraps a single append_one_action_entry with a caller-chosen write_len and
 * an all-zero write_bytes, and caller-chosen param_offset/param_encoding --
 * exists to probe that pair's validity directly, independent of any other
 * field. */
static size_t append_action_entry_param(uint8_t *b, size_t o, uint8_t action_id,
                                        uint16_t param_max, uint8_t write_len,
                                        uint8_t param_offset, uint8_t param_encoding) {
    b[o++] = action_id;
    b[o++] = (uint8_t)(param_max & 0xFF);
    b[o++] = (uint8_t)(param_max >> 8);
    b[o++] = 0x01;                  /* flags: device-local timed-off, no confirm */
    b[o++] = 0xF0; b[o++] = 0x2A;   /* write uuid16 0x2AF0 */
    b[o++] = write_len;
    for (uint8_t i = 0; i < write_len; i++) b[o++] = 0;
    b[o++] = param_offset;
    b[o++] = param_encoding;
    return o;
}

static size_t build_wrapper_with_action_param(uint8_t *b, size_t bufsz,
                                              uint8_t write_len, uint8_t param_offset,
                                              uint8_t param_encoding) {
    (void)bufsz;
    uint8_t code[1] = { 0xFF };   /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)sizeof code);
    memcpy(b + o, code, sizeof code);
    o += sizeof code;

    uint16_t hflags = PSVM_FLAG_ACTION_TABLE;
    memcpy(b + 6, &hflags, 2);

    b[o++] = 1;   /* action_count */
    return append_action_entry_param(b, o, ACT_IRRIGATION_OPEN, 60,
                                     write_len, param_offset, param_encoding);
}

/* Action entry with a 3-byte write_bytes AND (when has_confirm) a full
 * confirm block -- append_one_action_entry's fixed write_len=1/no-confirm
 * shape can exercise neither the multi-byte write_bytes truncation path nor
 * the confirm branch, so this builder exists specifically for both. */
static size_t append_action_entry_confirm(uint8_t *b, size_t o, uint8_t action_id,
                                          uint16_t param_max, bool has_confirm,
                                          uint8_t confirm_min_len) {
    uint8_t aflags = (uint8_t)(0x01 | (has_confirm ? 0x02 : 0));
    b[o++] = action_id;
    b[o++] = (uint8_t)(param_max & 0xFF);
    b[o++] = (uint8_t)(param_max >> 8);
    b[o++] = aflags;
    b[o++] = 0xF1; b[o++] = 0x2A;      /* write uuid16 0x2AF1 */
    b[o++] = 3;                         /* write_len: 3 bytes */
    b[o++] = 0x01; b[o++] = 0; b[o++] = 0;  /* constant byte + u16le param placeholder */
    b[o++] = 1;                         /* param_offset: byte 1 of write_bytes */
    b[o++] = 1;                         /* param_encoding: u16le, 1+2<=3 */
    if (has_confirm) {
        b[o++] = 0xF2; b[o++] = 0x2A;   /* confirm uuid16 0x2AF2 */
        b[o++] = confirm_min_len;
        b[o++] = 0;                     /* confirm_offset */
        b[o++] = 1;                     /* confirm_encoding: u16le */
        b[o++] = 0;                     /* confirm_op: == */
        b[o++] = 0x2A; b[o++] = 0x00;   /* confirm_value: 42 */
    }
    return o;
}

static size_t build_wrapper_with_confirm_action(uint8_t *b, size_t bufsz) {
    (void)bufsz;
    uint8_t code[1] = { 0xFF };   /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)sizeof code);
    memcpy(b + o, code, sizeof code);
    o += sizeof code;

    uint16_t hflags = PSVM_FLAG_ACTION_TABLE;
    memcpy(b + 6, &hflags, 2);

    b[o++] = 1;   /* action_count */
    return append_action_entry_confirm(b, o, ACT_PUMP_RUN, 60, true, 2);
}

/* Both PSVM_FLAG_CONNECT_PLAN and PSVM_FLAG_ACTION_TABLE set: the one case
 * where the action section's start offset (plan_off + plan_section_len) is
 * non-trivial, exercising that derivation directly instead of trusting it
 * by construction. */
static size_t build_wrapper_with_plan_and_action(uint8_t *b, size_t bufsz) {
    (void)bufsz;
    uint8_t code[1] = { 0xFF };   /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)sizeof code);
    memcpy(b + o, code, sizeof code);
    o += sizeof code;

    uint16_t hflags = PSVM_FLAG_CONNECT_PLAN | PSVM_FLAG_ACTION_TABLE;
    memcpy(b + 6, &hflags, 2);

    /* connect plan: 1 read, 0 writes, interval 600s */
    b[o++] = 1;      /* read_count */
    b[o++] = 0;      /* write_count */
    uint32_t interval_s = 600;
    memcpy(b + o, &interval_s, 4); o += 4;
    uint16_t read_uuid = 0x2A6E;
    memcpy(b + o, &read_uuid, 2); o += 2;
    b[o++] = 2;      /* min_len */

    return append_action_section(b, o, ACT_IRRIGATION_OPEN, 60, 0x01);
}

/* Two action entries under one action_count sharing the same action_id
 * (different param_max) -- a hand-posted blob reaches psvm_validate() with
 * no compiler involved, so this invariant must hold here, not just in
 * Task 4's (untrusted) compiler. */
static size_t build_wrapper_with_duplicate_action_ids(uint8_t *b, size_t bufsz) {
    (void)bufsz;
    uint8_t code[1] = { 0xFF };   /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)sizeof code);
    memcpy(b + o, code, sizeof code);
    o += sizeof code;

    uint16_t hflags = PSVM_FLAG_ACTION_TABLE;
    memcpy(b + 6, &hflags, 2);

    b[o++] = 2;   /* action_count */
    o = append_one_action_entry(b, o, ACT_IRRIGATION_OPEN, 60, 0x01);
    o = append_one_action_entry(b, o, ACT_IRRIGATION_OPEN, 30, 0x01);
    return o;
}

/* param_offset/param_encoding: either both the "no parameter" sentinel
 * (0xFF/0xFF), or a real encoding (0/1/2) whose width fits within
 * write_len. Task 8's executor splices the parameter at this offset into a
 * write_len-byte buffer, so this is the one function standing between an
 * HTTP client and that splice. */
static void test_action_param_offset_encoding_bounds(void) {
    uint8_t blob[160]; psvm_prog_t p;

    /* The exact garbage combo the reviewer found accepted before this fix:
     * write_len=1, param_offset=200, param_encoding=126. */
    size_t n = build_wrapper_with_action_param(blob, sizeof blob, 1, 200, 126);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);

    /* no-parameter sentinel: both 0xFF is legal. */
    n = build_wrapper_with_action_param(blob, sizeof blob, 1, 0xFF, 0xFF);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);

    /* mismatched sentinel, both directions. */
    n = build_wrapper_with_action_param(blob, sizeof blob, 1, 0xFF, 0);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);
    n = build_wrapper_with_action_param(blob, sizeof blob, 1, 0, 0xFF);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);

    /* encoding outside 0/1/2/0xFF. */
    n = build_wrapper_with_action_param(blob, sizeof blob, 1, 0, 3);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);

    /* u16le (width 2) at the exact boundary: offset=0 fits write_len=2;
     * offset=1 would need bytes [1,3) but write_len=2 only has byte 1. */
    n = build_wrapper_with_action_param(blob, sizeof blob, 2, 0, 1);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);
    n = build_wrapper_with_action_param(blob, sizeof blob, 2, 1, 1);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);

    /* u8 (width 1) at the exact boundary: offset=0 fits write_len=1;
     * offset=1 does not. */
    n = build_wrapper_with_action_param(blob, sizeof blob, 1, 0, 0);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);
    n = build_wrapper_with_action_param(blob, sizeof blob, 1, 1, 0);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);
}

static void test_action_confirm_parsed(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_confirm_action(blob, sizeof blob);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);
    assert(p.actions != NULL);
    assert(p.actions_len == 21);
}

/* Truncation at every boundary of a blob whose action section HAS a
 * confirm block and a multi-byte write_bytes -- the shape
 * test_action_section_truncated's 1-byte-write_len/no-confirm blob can
 * never reach. */
static void test_action_confirm_section_truncated(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_confirm_action(blob, sizeof blob);
    for (size_t cut = 1; cut < 22; cut++) {
        assert(psvm_validate(blob, n - cut, PSVM_DIALECT_WRAPPERS, 8, 0, &p)
               != PSVM_OK);
    }
}

static void test_action_confirm_min_len_bounds(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_confirm_action(blob, sizeof blob);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);

    /* confirm_min_len sits 6 bytes from the end: offset(1) + encoding(1) +
     * op(1) + value(2) follow it, i.e. index n-6. */
    size_t idx = n - 6;
    uint8_t saved = blob[idx];
    blob[idx] = 0;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);
    blob[idx] = PSVM_PLAN_SLOT + 1;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);
    blob[idx] = saved;
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);
}

/* The action section's start offset (plan_off + plan_section_len) is only
 * non-trivial when a connect plan actually precedes it -- assert both
 * sections' pointers/lengths land exactly where expected. */
static void test_action_section_after_connect_plan(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_plan_and_action(blob, sizeof blob);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);

    assert(p.plan != NULL);
    assert(p.plan_len == 9);   /* read_count(1)+write_count(1)+interval(4)+one read entry(3) */
    assert(p.plan[0] == 1 && p.plan[1] == 0);

    assert(p.actions != NULL);
    assert(p.actions_len == 11);   /* single no-confirm entry, see append_action_section */
    assert(p.actions[0] == 1);
    assert(p.actions == p.plan + p.plan_len);
}

static void test_action_duplicate_id_rejected(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_duplicate_action_ids(blob, sizeof blob);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);
}

/* Header flags already reject unknown bits (psvm.c); the per-action flags
 * byte gets the same entry-level forward-compat gate -- only bits 0-1 are
 * defined. */
static void test_action_reserved_flag_bits_rejected(void) {
    uint8_t blob[160]; psvm_prog_t p;
    size_t n = build_wrapper_with_action(blob, sizeof blob, ACT_IRRIGATION_OPEN, 60, 0xFC);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);
}

/* --- M5b whole-branch review, findings 3 and 5 --- */

/* A parameterless entry (switch.on/switch.off): param_max 0 and the
 * 0xFF/0xFF "no parameter" sentinel pair, which is exactly what the
 * browser compiler emits for an action block with an empty parameter
 * list (the controller's cross-language harness, ledger Task 4). */
static size_t append_action_entry_noparam(uint8_t *b, size_t o, uint8_t action_id) {
    b[o++] = action_id;
    b[o++] = 0; b[o++] = 0;         /* param_max 0 */
    b[o++] = 0x00;                  /* flags: no device-local, no confirm */
    b[o++] = 0xF0; b[o++] = 0x2A;   /* write uuid16 0x2AF0 */
    b[o++] = 1;                     /* write_len */
    b[o++] = 0x00;                  /* the close opcode byte */
    b[o++] = 0xFF;                  /* param_offset: no parameter */
    b[o++] = 0xFF;                  /* param_encoding: no parameter */
    return o;
}

/* A wrapper whose action table holds one duration action with a
 * caller-chosen flags byte and, optionally, a switch.off entry beside it.
 * This is the exact pair finding 3 is about: whether the hub has anything
 * to discharge the obligation the duration action creates. */
static size_t build_wrapper_actions_duration_and_off(uint8_t *b, size_t bufsz,
                                                     uint16_t param_max, uint8_t aflags,
                                                     bool with_switch_off) {
    (void)bufsz;
    uint8_t code[1] = { 0xFF };   /* HALT */
    size_t o = emit_header_d(b, PSVM_DIALECT_WRAPPERS, 0, 0, 0, (uint16_t)sizeof code);
    memcpy(b + o, code, sizeof code);
    o += sizeof code;

    uint16_t hflags = PSVM_FLAG_ACTION_TABLE;
    memcpy(b + 6, &hflags, 2);

    b[o++] = with_switch_off ? 2 : 1;   /* action_count */
    o = append_one_action_entry(b, o, ACT_IRRIGATION_OPEN, param_max, aflags);
    if (with_switch_off) o = append_action_entry_noparam(b, o, ACT_SWITCH_OFF);
    return o;
}

/* Finding 3: a duration action the DEVICE does not close itself is the
 * hub's obligation, and pending_close can only discharge it through
 * ACT_SWITCH_OFF. A wrapper declaring the open without the close installs
 * an obligation nothing can ever satisfy -- the valve stays open, the
 * retry budget is never spent so EXHAUSTED/CRITICAL never fires, and the
 * one alert that does fire names the wrong cause. Rejected at install. */
static void test_action_hub_closed_duration_requires_switch_off(void) {
    uint8_t blob[160]; psvm_prog_t p;

    /* No closes_itself, no switch.off -> refused. */
    size_t n = build_wrapper_actions_duration_and_off(blob, sizeof blob, 60, 0x00, false);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);

    /* Same action, plus switch.off -> the obligation is dischargeable. */
    n = build_wrapper_actions_duration_and_off(blob, sizeof blob, 60, 0x00, true);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);

    /* closes_itself set -> the hub owes no close, so switch.off is not
     * required (spec section 4.3's preferred path). */
    n = build_wrapper_actions_duration_and_off(blob, sizeof blob, 60, 0x01, false);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);

    /* A confirm block changes nothing about the obligation: bit 1 without
     * bit 0 is still the hub's close. */
    n = build_wrapper_actions_duration_and_off(blob, sizeof blob, 60, 0x02, false);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) != PSVM_OK);
}

/* Finding 5: param_max 0 on a duration action is refused by
 * actor_table_check() for every parameter value forever (action_param_ok()
 * refuses 0, and anything above 0 is over the effective bound), so it is a
 * pair that can never fire. Silent failure at run time becomes a named
 * failure at install time. */
static void test_action_duration_param_max_zero_rejected(void) {
    uint8_t blob[160]; psvm_prog_t p;

    size_t n = build_wrapper_with_action(blob, sizeof blob, ACT_IRRIGATION_OPEN, 0, 0x01);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_ERR_LIMITS);

    /* 1 is the smallest bound that can ever admit a command, and is
     * accepted -- the rejection above is about zero specifically, not
     * about "small". */
    n = build_wrapper_with_action(blob, sizeof blob, ACT_IRRIGATION_OPEN, 1, 0x01);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);

    /* A PARAMETERLESS action still requires param_max 0 (the pre-existing
     * upper-bound check, since action.h gives switch.off param_max 0) --
     * the new rule must not have inverted that. */
    n = build_wrapper_actions_duration_and_off(blob, sizeof blob, 60, 0x01, true);
    assert(psvm_validate(blob, n, PSVM_DIALECT_WRAPPERS, 8, 0, &p) == PSVM_OK);
}

/* --- M5b Task 10: CALL_ACTION (0x52), rules dialect action path --- */

typedef struct {
    uint8_t  kind;
    char     name[40];
    uint8_t  action_id;
    uint16_t param;
    int      calls;
} action_cap_t;

static bool action_capture(void *ctx, uint8_t kind, const char *name,
                           uint8_t action_id, uint16_t param) {
    action_cap_t *c = ctx;
    c->kind = kind;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->action_id = action_id;
    c->param = param;
    c->calls++;
    return true;
}

/* Rules-dialect blob: cond is trivially true (0.0 == 0.0), then code pushes
 * `param` and calls CALL_ACTION kind, name_const=1 ("Ficus"), action_id.
 * Const pool: [0] 0.0 (cond), [1] "Ficus" (action target name), [2] param. */
static size_t build_call_action(uint8_t *b, uint8_t kind, uint8_t action_id, float param) {
    uint8_t code[] = {
        0x01, 0,0,              /* PUSH_CONST 0 (0.0) */
        0x01, 0,0,              /* PUSH_CONST 0 (0.0) */
        0x24,                   /* EQ -> true */
        0x00,                   /* HALT_BOOL */
        0x01, 2,0,              /* PUSH_CONST 2 (param) */
        0x52, kind, 1,0, action_id, /* CALL_ACTION kind, name_const=1, action_id */
        0xFF,
    };
    size_t o = emit_header(b, 0, 3, 0, sizeof code);
    o = emit_f32(b, o, 0.0f);
    o = emit_str(b, o, "Ficus");
    o = emit_f32(b, o, param);
    memcpy(b + o, code, sizeof code);
    return o + sizeof code;
}

/* A CALL_ACTION blob calls the sink with the right operands (Task 10
 * brief, Step 1). */
static void test_call_action_sink(void) {
    uint8_t blob[64]; psvm_prog_t p;
    size_t n = build_call_action(blob, 0, ACT_IRRIGATION_OPEN, 8.0f);
    assert(psvm_validate(blob, n, PSVM_DIALECT_RULES, 4, 0, &p) == PSVM_OK);

    action_cap_t cap = {0, {0}, 0, 0, 0};
    psvm_result_t r = psvm_run(&p, NULL, NULL, NULL, NULL, action_capture, &cap, true);
    assert(r.err == PSVM_OK && r.cond);
    assert(cap.calls == 1);
    assert(cap.kind == 0);
    assert(strcmp(cap.name, "Ficus") == 0);
    assert(cap.action_id == ACT_IRRIGATION_OPEN);
    assert(cap.param == 8);
}

/* A run with a NULL action sink fails cleanly (PSVM_ERR_REF, the same
 * shape an unready ref uses) rather than crashing -- a rules blob can
 * reach psvm_run() in a dry-run context where no sink is wired at all
 * (Task 10 brief, Step 1/3). */
static void test_call_action_null_sink(void) {
    uint8_t blob[64]; psvm_prog_t p;
    size_t n = build_call_action(blob, 0, ACT_IRRIGATION_OPEN, 8.0f);
    assert(psvm_validate(blob, n, PSVM_DIALECT_RULES, 4, 0, &p) == PSVM_OK);

    psvm_result_t r = psvm_run(&p, NULL, NULL, NULL, NULL, NULL, NULL, true);
    assert(r.err == PSVM_ERR_REF);
}

/* The parameter comes off the stack as a float: negative, NaN and anything
 * above 65535 are all PSVM_ERR_TYPE rather than being truncated into a
 * plausible-looking uint16_t (Task 10 brief's design point 3 -- turning a
 * negative duration into 65535 s of irrigation is this project's oldest
 * failure shape aimed at its most dangerous surface). The sink must never
 * be called on any of these three. */
static void test_call_action_param_out_of_range_rejected(void) {
    action_cap_t cap;
    psvm_prog_t p;
    uint8_t blob[64];

    cap = (action_cap_t){0, {0}, 0, 0, 0};
    { size_t n = build_call_action(blob, 0, ACT_IRRIGATION_OPEN, -1.0f);
      assert(psvm_validate(blob, n, PSVM_DIALECT_RULES, 4, 0, &p) == PSVM_OK);
      psvm_result_t r = psvm_run(&p, NULL, NULL, NULL, NULL, action_capture, &cap, true);
      assert(r.err == PSVM_ERR_TYPE && cap.calls == 0); }

    cap = (action_cap_t){0, {0}, 0, 0, 0};
    { size_t n = build_call_action(blob, 0, ACT_IRRIGATION_OPEN, 70000.0f);
      assert(psvm_validate(blob, n, PSVM_DIALECT_RULES, 4, 0, &p) == PSVM_OK);
      psvm_result_t r = psvm_run(&p, NULL, NULL, NULL, NULL, action_capture, &cap, true);
      assert(r.err == PSVM_ERR_TYPE && cap.calls == 0); }

    cap = (action_cap_t){0, {0}, 0, 0, 0};
    { float nanval; { uint32_t bits = 0x7FC00000u; memcpy(&nanval, &bits, 4); }
      size_t n = build_call_action(blob, 0, ACT_IRRIGATION_OPEN, nanval);
      assert(psvm_validate(blob, n, PSVM_DIALECT_RULES, 4, 0, &p) == PSVM_OK);
      psvm_result_t r = psvm_run(&p, NULL, NULL, NULL, NULL, action_capture, &cap, true);
      assert(r.err == PSVM_ERR_TYPE && cap.calls == 0); }
}

int main(void) {
    uint8_t blob[512]; psvm_prog_t p;
    size_t len = build_demo(blob);

    /* validate happy path */
    assert(psvm_validate(blob, len, 1, 4, 0x3, &p) == PSVM_OK);
    /* corrupt magic / short blob / bad capability id / unknown builtin bit */
    { uint8_t bad[512]; memcpy(bad, blob, len); bad[0] = 'X';
      assert(psvm_validate(bad, len, 1, 4, 0x3, &p) == PSVM_ERR_HEADER); }
    assert(psvm_validate(blob, 10, 1, 4, 0x3, &p) == PSVM_ERR_TRUNCATED);
    { uint8_t bad[512]; memcpy(bad, blob, len); bad[18+5+3+7+3+3] = 9; /* ref cap */
      assert(psvm_validate(bad, len, 1, 4, 0x3, &p) != PSVM_OK); }
    { uint8_t bad[512]; memcpy(bad, blob, len); bad[8] = 0xFF;
      assert(psvm_validate(bad, len, 1, 4, 0x3, &p) == PSVM_ERR_LIMITS); }

    assert(psvm_validate(blob, len, 1, 4, 0x3, &p) == PSVM_OK);
    sink_cap_t cap = {{0}, 0, 0};
    psvm_ref_val_t vals[1] = {{ .value = 15.5f, .age_s = 10, .ready = true }};

    /* cond true + actions: sink sees interpolated string, %.1f formatting */
    psvm_result_t r = psvm_run(&p, vals, NULL, cap_sink, &cap, NULL, NULL, true);
    assert(r.err == PSVM_OK && r.cond && cap.calls == 1 && cap.last_builtin == 1);
    assert(strcmp(cap.last, "dry: 15.5%") == 0);

    /* run_actions=false: no sink call */
    cap.calls = 0; r = psvm_run(&p, vals, NULL, cap_sink, &cap, NULL, NULL, false);
    assert(r.err == PSVM_OK && r.cond && cap.calls == 0);

    /* cond false: actions skipped even with run_actions */
    vals[0].value = 30.0f; cap.calls = 0;
    r = psvm_run(&p, vals, NULL, cap_sink, &cap, NULL, NULL, true);
    assert(r.err == PSVM_OK && !r.cond && cap.calls == 0);

    /* not-ready ref aborts with PSVM_ERR_REF */
    vals[0].ready = false;
    r = psvm_run(&p, vals, NULL, cap_sink, &cap, NULL, NULL, true);
    assert(r.err == PSVM_ERR_REF);

    /* DIV by zero */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_div0(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_DIV0);
    }

    /* JMP past code end */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_jmp_oob(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_JUMP);
    }

    /* infinite JMP -3 loop -> step exhaustion */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_jmp_loop(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_STEPS);
        assert(r2.steps_used == PSVM_MAX_STEPS);
    }

    /* stack overflow via 33 PUSH_CONST */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_stack_overflow(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_STACK);
    }

    /* .age field ref */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_age(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t v_old[1] = {{ .value = 0, .age_s = 61, .ready = true }};
        psvm_result_t r2 = psvm_run(&p2, v_old, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
        psvm_ref_val_t v_fresh[1] = {{ .value = 0, .age_s = 30, .ready = true }};
        r2 = psvm_run(&p2, v_fresh, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond);
    }

    /* AND / OR / NOT truth table on two refs */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_bool2(b2, 0x30); /* AND */
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        struct { float a, b; bool expect; } cases[] = {
            { -1, -1, false }, { -1, 1, false }, { 1, -1, false }, { 1, 1, true },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            psvm_ref_val_t rv[2] = {
                { .value = cases[i].a, .age_s = 0, .ready = true },
                { .value = cases[i].b, .age_s = 0, .ready = true },
            };
            psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
            assert(r2.err == PSVM_OK && r2.cond == cases[i].expect);
        }
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_bool2(b2, 0x31); /* OR */
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        struct { float a, b; bool expect; } cases[] = {
            { -1, -1, false }, { -1, 1, true }, { 1, -1, true }, { 1, 1, true },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            psvm_ref_val_t rv[2] = {
                { .value = cases[i].a, .age_s = 0, .ready = true },
                { .value = cases[i].b, .age_s = 0, .ready = true },
            };
            psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
            assert(r2.err == PSVM_OK && r2.cond == cases[i].expect);
        }
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_bool2(b2, 0); /* NOT */
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv_true[1] = { { .value = 1, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv_true, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* NOT(true) */
        psvm_ref_val_t rv_false[1] = { { .value = -1, .age_s = 0, .ready = true } };
        r2 = psvm_run(&p2, rv_false, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* NOT(false) */
    }

    /* EQ on two strings -> PSVM_ERR_TYPE (comparisons are numeric-only) */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_eq_str(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_TYPE);
    }

    /* LE / GE / NE truth checks */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_cmp1(b2, 0x21 /* LE */, 5.0f);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* 5 <= 5 */
        rv[0].value = 6.0f;
        r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* 6 <= 5 false */
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_cmp1(b2, 0x23 /* GE */, 5.0f);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* 5 >= 5 */
        rv[0].value = 4.0f;
        r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_cmp1(b2, 0x25 /* NE */, 5.0f);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* 5 != 5 false */
        rv[0].value = 4.0f;
        r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }

    /* ADD / SUB / MUL / DIV correctness */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x10, 3.0f, 4.0f, 7.0f); /* ADD */
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 3.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x11, 10.0f, 4.0f, 6.0f); /* SUB */
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 10.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x12, 3.0f, 4.0f, 12.0f); /* MUL */
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 3.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x13, 12.0f, 4.0f, 3.0f); /* DIV */
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 12.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }

    /* JZ/JMP control flow */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_branch2(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv_pos[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv_pos, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* true branch: GT(1,0)=true */
        psvm_ref_val_t rv_neg[1] = { { .value = -5.0f, .age_s = 0, .ready = true } };
        r2 = psvm_run(&p2, rv_neg, NULL, NULL, NULL, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* else branch: GT(0,1)=false */
    }

    /* CALL_BUILTIN 0 (log) */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_log(b2);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p2) == PSVM_OK);
        sink_cap_t cap2 = {{0}, 0, 0};
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, cap_sink, &cap2, NULL, NULL, true);
        assert(r2.err == PSVM_OK && r2.cond && cap2.calls == 1 && cap2.last_builtin == 0);
        assert(strcmp(cap2.last, "hello") == 0);
    }

    /* ---- wrapper dialect (M3 spec section 3): payload accessors ---- */
    {
        /* offsets 0/1 vs 4/5 deliberately carry DIFFERENT bytes so an LE/BE
         * mixup gives a visibly wrong number rather than accidentally
         * matching. */
        uint8_t payload[20] = {
            0xCD, 0xAB,             /* [0..1]  u16_le -> 0xABCD, u16_be -> 0xCDAB */
            0x00, 0x00,             /* [2..3]  filler */
            0xAB, 0xCD,             /* [4..5]  u16_be -> 0xABCD, u16_le -> 0xCDAB */
            0x00, 0x80,             /* [6..7]  i16_le -> -32768 */
            0x80, 0x00,             /* [8..9]  i16_be -> -32768 */
            0x01, 0x02, 0x03,       /* [10..12] u24_le -> 0x030201 */
            0x01, 0x02, 0x03, 0x04, /* [13..16] u32_le -> 0x04030201 */
            0x42,                   /* [17]    u8 -> 66 */
            0xB4, 0x0F,             /* [18..19] bits: 0xB4=10110100, 0x0FB4=0000111110110100 */
        };
        psvm_wrapper_io_t wio = {0};
        wio.payload.data = payload;
        wio.payload.len = (uint8_t)sizeof(payload);

        struct { uint8_t op; uint16_t off; float expect; } cases[] = {
            { 0x61 /* LOAD_U16LE */, 0,  (float)0xABCD },
            { 0x62 /* LOAD_U16BE */, 0,  (float)0xCDAB },
            { 0x62 /* LOAD_U16BE */, 4,  (float)0xABCD },
            { 0x61 /* LOAD_U16LE */, 4,  (float)0xCDAB },
            { 0x63 /* LOAD_I16LE */, 6,  -32768.0f },
            { 0x64 /* LOAD_I16BE */, 8,  -32768.0f },
            { 0x65 /* LOAD_U24LE */, 10, (float)0x030201 },
            { 0x66 /* LOAD_U32LE */, 13, (float)0x04030201u },
            { 0x60 /* LOAD_U8 */,    17, 66.0f },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            uint8_t b2[64]; psvm_prog_t p2;
            size_t l2 = build_w_load(b2, cases[i].op, cases[i].off);
            assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
            emit_cap_t ecap = {0};
            wio.emit = emit_capture; wio.emit_ctx = &ecap;
            psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
            assert(r2.err == PSVM_OK);
            assert(ecap.count == 1 && ecap.items[0].cap == 0);
            assert(ecap.items[0].value == cases[i].expect);
        }

        /* LOAD_BITS: single-byte field, then a field spanning two bytes. */
        {
            uint8_t b2[64]; psvm_prog_t p2;
            size_t l2 = build_w_load_bits(b2, 18, 2, 4); /* (0xB4>>2)&0xF = 13 */
            assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
            emit_cap_t ecap = {0};
            wio.emit = emit_capture; wio.emit_ctx = &ecap;
            psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
            assert(r2.err == PSVM_OK && ecap.count == 1 && ecap.items[0].value == 13.0f);
        }
        {
            uint8_t b2[64]; psvm_prog_t p2;
            size_t l2 = build_w_load_bits(b2, 18, 4, 8); /* (0x0FB4>>4)&0xFF = 0xFB = 251 */
            assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
            emit_cap_t ecap = {0};
            wio.emit = emit_capture; wio.emit_ctx = &ecap;
            psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
            assert(r2.err == PSVM_OK && ecap.count == 1 && ecap.items[0].value == 251.0f);
        }

        /* PAYLOAD_LEN reflects the actual advert length. */
        {
            uint8_t b2[64]; psvm_prog_t p2;
            size_t l2 = build_w_payload_len(b2);
            assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
            emit_cap_t ecap = {0};
            wio.emit = emit_capture; wio.emit_ctx = &ecap;
            psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
            assert(r2.err == PSVM_OK && ecap.count == 1 && ecap.items[0].value == 20.0f);
        }
    }

    /* `>>` is bit-exact (spec §3 as amended): DIV by 2^n then FLOOR must
     * match a real integer right-shift exactly, not leave a fractional
     * remainder. 0xAC36 = 44086; 44086 >> 5 = 1377 (44086 / 32 = 1377.6875,
     * so without FLOOR this would wrongly be 1377.6875). Also: FLOOR on a
     * negative operand is a runtime error (PSVM_ERR_TYPE -- the closest
     * existing code: a negative input is a shape violation for an
     * operation whose whole contract assumes a non-negative bit-derived
     * integer, the same family as comparing two strings or using a bool
     * where a number is expected). */
    {
        uint8_t payload[2] = { 0xAC, 0x36 }; /* u16_be(0) = 0xAC36 = 44086 */
        psvm_wrapper_io_t wio = {0};
        wio.payload.data = payload; wio.payload.len = 2;

        uint8_t b2[64]; psvm_prog_t p2;
        size_t l2 = build_w_shift(b2, 0x62 /* LOAD_U16BE */, 0, 32.0f);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        emit_cap_t ecap = {0};
        wio.emit = emit_capture; wio.emit_ctx = &ecap;
        psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_OK && ecap.count == 1);
        assert(ecap.items[0].value == 1377.0f); /* exact, not 1377.6875 */

        uint8_t b3[64]; psvm_prog_t p3;
        size_t l3 = build_w_floor_neg(b3);
        assert(psvm_validate(b3, l3, PSVM_DIALECT_WRAPPERS, 7, 0, &p3) == PSVM_OK);
        r2 = psvm_run(&p3, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_TYPE);
    }

    /* Bounds: an offset at exactly `len` and one past it both PSVM_ERR_REF;
     * the last IN-bounds offset (len-1) still succeeds. */
    {
        uint8_t payload[5] = { 1, 2, 3, 4, 5 };
        psvm_wrapper_io_t wio = {0};
        wio.payload.data = payload; wio.payload.len = 5;

        uint8_t b2[64]; psvm_prog_t p2;
        size_t l2 = build_w_load(b2, 0x60 /* LOAD_U8 */, 4); /* last valid index */
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        emit_cap_t ecap = {0}; wio.emit = emit_capture; wio.emit_ctx = &ecap;
        psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_OK && ecap.count == 1 && ecap.items[0].value == 5.0f);

        l2 = build_w_load(b2, 0x60, 5); /* == len: PSVM_ERR_REF */
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_REF);

        l2 = build_w_load(b2, 0x60, 6); /* one past len: PSVM_ERR_REF */
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_REF);

        /* A multi-byte accessor whose tail runs past len is also PSVM_ERR_REF
         * even though its OFFSET alone is in range (offset=3, u16 needs
         * bytes 3 and 4, but len=5 so byte 4 is the last valid one -- this
         * one should succeed -- and offset=4 needs byte 5, out of range). */
        l2 = build_w_load(b2, 0x61 /* LOAD_U16LE */, 3);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        emit_cap_t ecap2 = {0}; wio.emit = emit_capture; wio.emit_ctx = &ecap2;
        r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_OK && ecap2.count == 1);
        l2 = build_w_load(b2, 0x61, 4);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_REF);
    }

    /* A wrapper reading at offset 48 is in bounds once the buffer is 64 B --
     * that offset is slot 3 of the concatenated GATT read buffer (16 B slots).
     * Before M5a it was out of range and the run failed with a bounds error. */
    {
        assert(PSVM_PAYLOAD_MAX == 64);
        uint8_t payload[64] = {0};
        payload[48] = 0x12; payload[49] = 0x34;
        /* u16_le at 48 -> 0x3412 */
        psvm_wrapper_io_t wio = {0};
        wio.payload.data = payload; wio.payload.len = 64;

        uint8_t b2[64]; psvm_prog_t p2;
        size_t l2 = build_w_load(b2, 0x61 /* LOAD_U16LE */, 48);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        emit_cap_t ecap = {0};
        wio.emit = emit_capture; wio.emit_ctx = &ecap;
        psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_OK && ecap.count == 1 && ecap.items[0].value == 0x3412);
    }

    /* REQUIRE: false discards the whole run's buffered emits (including one
     * emitted BEFORE the require); true lets both through. EMIT reaching
     * the sink with the right capability id is covered by both halves. */
    {
        uint8_t b2[128]; psvm_prog_t p2;
        size_t l2 = build_w_require(b2, false);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        emit_cap_t ecap = {0};
        psvm_wrapper_io_t wio = {0}; wio.emit = emit_capture; wio.emit_ctx = &ecap;
        psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_OK && ecap.count == 0);

        l2 = build_w_require(b2, true);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        emit_cap_t ecap2 = {0};
        wio.emit = emit_capture; wio.emit_ctx = &ecap2;
        r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_OK && ecap2.count == 2);
        assert(ecap2.items[0].cap == 7 && ecap2.items[0].value == 1.0f);
        assert(ecap2.items[1].cap == 6 && ecap2.items[1].value == 1.0f);
    }

    /* EMIT's inline capability operand is range-checked at validate time
     * (Task 5 review round 2, "Bypass A"): PUSH_CONST 1.0; EMIT 99; HALT,
     * validated with caps_max=7 (0..7 valid) -- 99 is out of range and must
     * be rejected with the same error code the ref table's own capability
     * field uses for the identical shape of problem. */
    {
        uint8_t code[16]; size_t co = 0;
        co = emit_op_u16(code, co, 0x01, 0);    /* PUSH_CONST 0 (1.0) */
        co = emit_op_u8(code, co, 0x69, 99);    /* EMIT cap=99 -- invalid */
        co = emit_op(code, co, 0xFF);           /* HALT */
        uint8_t b2[64]; psvm_prog_t p2;
        size_t o = emit_header_d(b2, PSVM_DIALECT_WRAPPERS, 0, 1, 0, (uint16_t)co);
        o = emit_f32(b2, o, 1.0f);
        memcpy(b2 + o, code, co);
        assert(psvm_validate(b2, o + co, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_ERR_REF);
    }

    /* AES_CCM: decrypts in place, subsequent accessors read plaintext, and
     * the working payload length shrinks to whatever the callback reports
     * (simulating a stripped trailing tag). */
    {
        uint8_t payload[10] = { 0,0,0,0, 0x11,0x22,0x33,0x44,0x55,0x66 };
        psvm_wrapper_io_t wio = {0};
        wio.payload.data = payload; wio.payload.len = 10;
        wio.aes_ccm = fake_aes_ccm;

        uint8_t b2[64]; psvm_prog_t p2;
        size_t l2 = build_w_aes_ccm(b2, 4, 6); /* region [4,10) runs to len=10 */
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        emit_cap_t ecap = {0};
        wio.emit = emit_capture; wio.emit_ctx = &ecap;
        psvm_result_t r2 = psvm_run(&p2, NULL, &wio, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_OK && ecap.count == 2);
        assert(ecap.items[0].cap == 0 && ecap.items[0].value == (float)(0x11 ^ 0xFF)); /* plaintext byte */
        assert(ecap.items[1].cap == 1 && ecap.items[1].value == 6.0f); /* new payload_len: 4+2 */

        /* No aes_ccm callback wired up ("no key available"): PSVM_ERR_REF. */
        psvm_wrapper_io_t wio_nokey = {0};
        wio_nokey.payload.data = payload; wio_nokey.payload.len = 10;
        r2 = psvm_run(&p2, NULL, &wio_nokey, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_REF);

        /* Callback present but reports failure (bad MIC, etc.): PSVM_ERR_REF. */
        psvm_wrapper_io_t wio_fail = {0};
        wio_fail.payload.data = payload; wio_fail.payload.len = 10;
        wio_fail.aes_ccm = failing_aes_ccm;
        r2 = psvm_run(&p2, NULL, &wio_fail, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_REF);

        /* Region that does NOT run to the end of the payload: PSVM_ERR_REF
         * (AES_CCM's contract requires the ciphertext+tag region be the
         * payload's tail -- see psvm.c's own doc comment on 0x6B). */
        {
            uint8_t b3[64]; psvm_prog_t p3;
            size_t l3 = build_w_aes_ccm(b3, 4, 5); /* [4,9) leaves byte 9 unconsumed */
            assert(psvm_validate(b3, l3, PSVM_DIALECT_WRAPPERS, 7, 0, &p3) == PSVM_OK);
            r2 = psvm_run(&p3, NULL, &wio, NULL, NULL, NULL, NULL, true);
            assert(r2.err == PSVM_ERR_REF);
        }
    }

    /* dialect gate: a dialect=2 blob rejected when the validator is told to
     * expect rules, and a dialect=1 blob rejected when told to expect
     * wrappers. */
    {
        uint8_t b2[64]; psvm_prog_t p2;
        size_t l2 = build_w_payload_len(b2);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);
        assert(psvm_validate(b2, l2, PSVM_DIALECT_RULES, 7, 0, &p2) == PSVM_ERR_HEADER);

        uint8_t b3[512]; psvm_prog_t p3;
        size_t l3 = build_demo(b3);
        assert(psvm_validate(b3, l3, PSVM_DIALECT_RULES, 4, 0x3, &p3) == PSVM_OK);
        assert(psvm_validate(b3, l3, PSVM_DIALECT_WRAPPERS, 4, 0x3, &p3) == PSVM_ERR_HEADER);
    }

    /* validate-time limits: ref_count and const_count over the caps */
    {
        uint8_t b2[64];
        size_t l2 = build_limits_header(b2, 0, PSVM_MAX_REFS + 1);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p) == PSVM_ERR_LIMITS);
    }
    {
        uint8_t b2[64];
        size_t l2 = build_limits_header(b2, 257, 0);
        assert(psvm_validate(b2, l2, 1, 4, 0x3, &p) == PSVM_ERR_LIMITS);
    }

    /* M3 review fix 1: a dialect=2 (wrapper) blob with ref_count>=1 and a
     * LOAD_REF instruction validates cleanly (psvm_validate() does not
     * opcode-walk for dialect legality, and validate_emit_caps() treats
     * 0x02 as a 3-byte instruction and walks past it). Both wrapper-dialect
     * call sites (wrapper_exec.c, api_v1.c's /test) pass resolved=NULL, so
     * psvm_run() must turn that into PSVM_ERR_REF instead of dereferencing
     * NULL. Without the psvm.c fix this segfaults. */
    {
        uint8_t code[8]; size_t co = 0;
        co = emit_op_u16(code, co, 0x02, 0);   /* LOAD_REF 0 */
        co = emit_op_u8(code, co, 0x69, 0);    /* EMIT 0 */
        co = emit_op(code, co, 0xFF);          /* HALT */

        uint8_t b2[64];
        size_t o = emit_header_d(b2, PSVM_DIALECT_WRAPPERS, 0, 1, 1, (uint16_t)co);
        o = emit_str(b2, o, "x");
        o = emit_ref(b2, o, 0, 0, 0, 0);
        memcpy(b2 + o, code, co);
        size_t l2 = o + co;

        psvm_prog_t p2;
        assert(psvm_validate(b2, l2, PSVM_DIALECT_WRAPPERS, 7, 0, &p2) == PSVM_OK);

        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_REF);
    }

    /* M5a: PSBC trailing connect-plan section */
    test_no_plan_flag_unchanged();
    test_plan_parsed();
    test_plan_truncated();
    test_plan_limits();
    test_plan_interval_bounds();
    test_plan_rejected_for_rules();
    test_plan_zero_reads_rejected();
    test_plan_min_len_bounds();

    /* M5b: PSBC trailing action-table section */
    test_action_section_parsed();
    test_action_param_max_cannot_exceed_firmware();
    test_action_unknown_id_rejected();
    test_action_section_truncated();
    test_action_section_rejected_for_rules();

    /* M5b fix round 1 */
    test_action_param_offset_encoding_bounds();
    test_action_confirm_parsed();
    test_action_confirm_section_truncated();
    test_action_confirm_min_len_bounds();
    test_action_section_after_connect_plan();
    test_action_duplicate_id_rejected();
    test_action_reserved_flag_bits_rejected();
    test_action_hub_closed_duration_requires_switch_off();
    test_action_duration_param_max_zero_rejected();

    /* M5b Task 10: CALL_ACTION (0x52), rules dialect action path */
    test_call_action_sink();
    test_call_action_null_sink();
    test_call_action_param_out_of_range_rejected();

    printf("test_psvm: all passed\n");
    return 0;
}
