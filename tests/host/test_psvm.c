#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "psvm.h"

/* --- minimal PSBC builder: header + consts + refs + code --- */
static size_t emit_header(uint8_t *b, uint32_t builtins, uint16_t nconst,
                          uint16_t nref, uint16_t codelen) {
    memcpy(b, "PSBC", 4); b[4] = 1; b[5] = 1; b[6] = b[7] = 0;
    memcpy(b + 8, &builtins, 4);
    memcpy(b + 12, &nconst, 2); memcpy(b + 14, &nref, 2); memcpy(b + 16, &codelen, 2);
    return 18;
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

typedef struct { char last[600]; uint8_t last_builtin; int calls; } sink_cap_t;
static bool cap_sink(void *ctx, uint8_t builtin, const char *msg) {
    sink_cap_t *c = ctx; c->last_builtin = builtin;
    snprintf(c->last, sizeof c->last, "%s", msg); c->calls++; return true;
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

int main(void) {
    uint8_t blob[512]; psvm_prog_t p;
    size_t len = build_demo(blob);

    /* validate happy path */
    assert(psvm_validate(blob, len, 4, 0x3, &p) == PSVM_OK);
    /* corrupt magic / short blob / bad capability id / unknown builtin bit */
    { uint8_t bad[512]; memcpy(bad, blob, len); bad[0] = 'X';
      assert(psvm_validate(bad, len, 4, 0x3, &p) == PSVM_ERR_HEADER); }
    assert(psvm_validate(blob, 10, 4, 0x3, &p) == PSVM_ERR_TRUNCATED);
    { uint8_t bad[512]; memcpy(bad, blob, len); bad[18+5+3+7+3+3] = 9; /* ref cap */
      assert(psvm_validate(bad, len, 4, 0x3, &p) != PSVM_OK); }
    { uint8_t bad[512]; memcpy(bad, blob, len); bad[8] = 0xFF;
      assert(psvm_validate(bad, len, 4, 0x3, &p) == PSVM_ERR_LIMITS); }

    assert(psvm_validate(blob, len, 4, 0x3, &p) == PSVM_OK);
    sink_cap_t cap = {{0}, 0, 0};
    psvm_ref_val_t vals[1] = {{ .value = 15.5f, .age_s = 10, .ready = true }};

    /* cond true + actions: sink sees interpolated string, %.1f formatting */
    psvm_result_t r = psvm_run(&p, vals, cap_sink, &cap, true);
    assert(r.err == PSVM_OK && r.cond && cap.calls == 1 && cap.last_builtin == 1);
    assert(strcmp(cap.last, "dry: 15.5%") == 0);

    /* run_actions=false: no sink call */
    cap.calls = 0; r = psvm_run(&p, vals, cap_sink, &cap, false);
    assert(r.err == PSVM_OK && r.cond && cap.calls == 0);

    /* cond false: actions skipped even with run_actions */
    vals[0].value = 30.0f; cap.calls = 0;
    r = psvm_run(&p, vals, cap_sink, &cap, true);
    assert(r.err == PSVM_OK && !r.cond && cap.calls == 0);

    /* not-ready ref aborts with PSVM_ERR_REF */
    vals[0].ready = false;
    r = psvm_run(&p, vals, cap_sink, &cap, true);
    assert(r.err == PSVM_ERR_REF);

    /* DIV by zero */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_div0(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_DIV0);
    }

    /* JMP past code end */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_jmp_oob(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_JUMP);
    }

    /* infinite JMP -3 loop -> step exhaustion */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_jmp_loop(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_STEPS);
        assert(r2.steps_used == PSVM_MAX_STEPS);
    }

    /* stack overflow via 33 PUSH_CONST */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_stack_overflow(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_STACK);
    }

    /* .age field ref */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_age(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t v_old[1] = {{ .value = 0, .age_s = 61, .ready = true }};
        psvm_result_t r2 = psvm_run(&p2, v_old, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
        psvm_ref_val_t v_fresh[1] = {{ .value = 0, .age_s = 30, .ready = true }};
        r2 = psvm_run(&p2, v_fresh, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond);
    }

    /* AND / OR / NOT truth table on two refs */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_bool2(b2, 0x30); /* AND */
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        struct { float a, b; bool expect; } cases[] = {
            { -1, -1, false }, { -1, 1, false }, { 1, -1, false }, { 1, 1, true },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            psvm_ref_val_t rv[2] = {
                { .value = cases[i].a, .age_s = 0, .ready = true },
                { .value = cases[i].b, .age_s = 0, .ready = true },
            };
            psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
            assert(r2.err == PSVM_OK && r2.cond == cases[i].expect);
        }
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_bool2(b2, 0x31); /* OR */
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        struct { float a, b; bool expect; } cases[] = {
            { -1, -1, false }, { -1, 1, true }, { 1, -1, true }, { 1, 1, true },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            psvm_ref_val_t rv[2] = {
                { .value = cases[i].a, .age_s = 0, .ready = true },
                { .value = cases[i].b, .age_s = 0, .ready = true },
            };
            psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
            assert(r2.err == PSVM_OK && r2.cond == cases[i].expect);
        }
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_bool2(b2, 0); /* NOT */
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv_true[1] = { { .value = 1, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv_true, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* NOT(true) */
        psvm_ref_val_t rv_false[1] = { { .value = -1, .age_s = 0, .ready = true } };
        r2 = psvm_run(&p2, rv_false, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* NOT(false) */
    }

    /* EQ on two strings -> PSVM_ERR_TYPE (comparisons are numeric-only) */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_eq_str(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_result_t r2 = psvm_run(&p2, NULL, NULL, NULL, true);
        assert(r2.err == PSVM_ERR_TYPE);
    }

    /* LE / GE / NE truth checks */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_cmp1(b2, 0x21 /* LE */, 5.0f);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* 5 <= 5 */
        rv[0].value = 6.0f;
        r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* 6 <= 5 false */
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_cmp1(b2, 0x23 /* GE */, 5.0f);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* 5 >= 5 */
        rv[0].value = 4.0f;
        r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_cmp1(b2, 0x25 /* NE */, 5.0f);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* 5 != 5 false */
        rv[0].value = 4.0f;
        r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }

    /* ADD / SUB / MUL / DIV correctness */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x10, 3.0f, 4.0f, 7.0f); /* ADD */
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 3.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x11, 10.0f, 4.0f, 6.0f); /* SUB */
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 10.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x12, 3.0f, 4.0f, 12.0f); /* MUL */
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 3.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_arith(b2, 0x13, 12.0f, 4.0f, 3.0f); /* DIV */
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv[1] = { { .value = 12.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond);
    }

    /* JZ/JMP control flow */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_branch2(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        psvm_ref_val_t rv_pos[1] = { { .value = 5.0f, .age_s = 0, .ready = true } };
        psvm_result_t r2 = psvm_run(&p2, rv_pos, NULL, NULL, false);
        assert(r2.err == PSVM_OK && r2.cond); /* true branch: GT(1,0)=true */
        psvm_ref_val_t rv_neg[1] = { { .value = -5.0f, .age_s = 0, .ready = true } };
        r2 = psvm_run(&p2, rv_neg, NULL, NULL, false);
        assert(r2.err == PSVM_OK && !r2.cond); /* else branch: GT(0,1)=false */
    }

    /* CALL_BUILTIN 0 (log) */
    {
        uint8_t b2[512]; psvm_prog_t p2;
        size_t l2 = build_log(b2);
        assert(psvm_validate(b2, l2, 4, 0x3, &p2) == PSVM_OK);
        sink_cap_t cap2 = {{0}, 0, 0};
        psvm_result_t r2 = psvm_run(&p2, NULL, cap_sink, &cap2, true);
        assert(r2.err == PSVM_OK && r2.cond && cap2.calls == 1 && cap2.last_builtin == 0);
        assert(strcmp(cap2.last, "hello") == 0);
    }

    /* validate-time limits: ref_count and const_count over the caps */
    {
        uint8_t b2[64];
        size_t l2 = build_limits_header(b2, 0, PSVM_MAX_REFS + 1);
        assert(psvm_validate(b2, l2, 4, 0x3, &p) == PSVM_ERR_LIMITS);
    }
    {
        uint8_t b2[64];
        size_t l2 = build_limits_header(b2, 257, 0);
        assert(psvm_validate(b2, l2, 4, 0x3, &p) == PSVM_ERR_LIMITS);
    }

    printf("test_psvm: all passed\n");
    return 0;
}
