#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PSVM_FMT_VER      1
#define PSVM_DIALECT_RULES 1
#define PSVM_MAX_REFS     32
#define PSVM_STACK        32
#define PSVM_MAX_STEPS    10000
#define PSVM_STRBUF       512

typedef enum { PSVM_OK = 0, PSVM_ERR_HEADER, PSVM_ERR_LIMITS, PSVM_ERR_TRUNCATED,
               PSVM_ERR_BADOP, PSVM_ERR_STACK, PSVM_ERR_STEPS, PSVM_ERR_DIV0,
               PSVM_ERR_JUMP, PSVM_ERR_TYPE, PSVM_ERR_REF } psvm_err_t;

typedef struct { float value; uint32_t age_s; bool ready; } psvm_ref_val_t;

/* kind: 0 plant, 1 device; field: 0 value, 1 age (spec section 2) */
typedef struct { uint8_t kind; uint16_t name_const; uint8_t capability; uint8_t field; } psvm_ref_t;

/* Builtin sink: builtin 0=log 1=notify, msg NUL-terminated (may be truncated
 * to PSVM_STRBUF-1). Return false to abort the run (treated as PSVM_ERR_TYPE). */
typedef bool (*psvm_sink_t)(void *ctx, uint8_t builtin, const char *msg);

typedef struct {
    const uint8_t *blob; size_t len;          /* validated PSBC */
    uint16_t const_count, ref_count, code_len;
    uint32_t builtins;                         /* header bitmap */
    const uint8_t *consts, *refs, *code;       /* section pointers */
} psvm_prog_t;

/* Parses+bounds-checks the blob (spec section 2). caps_max = highest
 * capability id this firmware knows (4 in M1); builtins_impl = bitmap of
 * implemented builtins (0b11 in M1). */
psvm_err_t psvm_validate(const uint8_t *blob, size_t len, uint8_t caps_max,
                         uint32_t builtins_impl, psvm_prog_t *out);

/* Ref metadata accessors for the engine's resolver (index < ref_count). */
psvm_ref_t psvm_get_ref(const psvm_prog_t *p, uint16_t idx);
/* Returns const-pool string (tag 1) or NULL; len_out optional. */
const char *psvm_get_str(const psvm_prog_t *p, uint16_t idx, uint16_t *len_out);

typedef struct { bool cond; psvm_err_t err; uint32_t steps_used; } psvm_result_t;

/* Runs the program (spec section 3): executes to HALT_BOOL for cond; if
 * run_actions && cond, continues to HALT calling sink for builtins.
 * resolved[] must have ref_count entries. A not-ready ref touched by
 * execution aborts with PSVM_ERR_REF (engine reports "not ready"). */
psvm_result_t psvm_run(const psvm_prog_t *p, const psvm_ref_val_t *resolved,
                       psvm_sink_t sink, void *sink_ctx, bool run_actions);
