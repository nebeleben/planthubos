#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PSVM_FMT_VER      1
#define PSVM_DIALECT_RULES 1
#define PSVM_DIALECT_WRAPPERS 2
#define PSVM_MAX_REFS     32
#define PSVM_STACK        32
#define PSVM_MAX_STEPS    10000
#define PSVM_STRBUF       512
/* Raised from 31 to 64 for M5a. 31 was the largest legacy advertisement
 * payload, which is still the only thing an advert-only wrapper decodes.
 * A connect wrapper decodes a CONCATENATION of its GATT reads instead:
 * PSVM_PLAN_MAX_READS (4) fixed slots of 16 bytes each. Fixed slots are
 * what let named buffers be compile-time offsets, which is why M5a needs
 * no new opcodes and no VM surgery -- the cost is these 33 extra bytes in
 * the interpreter's working buffer. */
#define PSVM_PAYLOAD_MAX  64
#define PSVM_PLAN_SLOT    16
/* Emitted (capability, value) pairs are buffered (never flushed to the
 * caller's sink) until a wrapper program reaches HALT successfully; a
 * failed REQUIRE (spec section 3) discards the buffer instead, so "require
 * after emit" cleanly emits nothing. 16 is generous headroom over
 * CAPABILITY_COUNT (8 in M2) for a wrapper that emits some capability more
 * than once across a run. Fixed array, no allocation. */
#define PSVM_MAX_EMITS    16

/* M5a: a wrapper may carry a declarative GATT connect plan as a trailing
 * section after the code. Gated by this flag so every blob compiled before
 * M5a -- which sets flags to 0 -- is byte-identical and parses exactly as
 * it did. psvm_validate already tolerated trailing bytes, so an older hub
 * reading a newer blob ignores the plan rather than rejecting it.
 *
 * On-blob layout (present only when this flag is set), all multi-byte
 * fields little-endian like the rest of this format:
 *   u8  read_count            (1..PSVM_PLAN_MAX_READS)
 *   u8  write_count           (0..PSVM_PLAN_MAX_WRITES)
 *   u32 interval_s            (60..86400)
 *   read_count  x { u16 uuid16; u8 min_len (1..PSVM_PLAN_SLOT) }
 *   write_count x { u16 uuid16; u8 len (1..PSVM_PLAN_WRITE_MAX); u8 data[len] }
 *
 * min_len is the fewest bytes that read must return for the decode block to
 * mean anything: the compiler's max(offset + accessor width) over every
 * accessor addressing that buffer. A read shorter than it fails the attempt
 * instead of zero-padding its slot, which would emit a plausible wrong value
 * with no failure recorded -- the shape spec section 4 exists to prevent.
 *
 * interval_s is u32, not u16: the cap's upper bound (86400 s, a full day)
 * does not fit in a u16 (max 65535), and a narrower field would silently
 * truncate a wrapper author's "every 24h" (86400) down to 20864 -- a
 * silently wrong value produced from valid input, the exact failure shape
 * this project has already spent three milestones eliminating elsewhere
 * (the `>>` FLOOR fix, the payload-offset bug, the first-vs-last AD
 * structure fix). Two extra bytes in a roughly twenty-byte section is not
 * worth trading away correctness for, so both the lower AND upper bound are
 * enforced at validate time (psvm.c) now that both are representable. */
#define PSVM_FLAG_CONNECT_PLAN 0x0001u
#define PSVM_PLAN_MAX_READS    4
#define PSVM_PLAN_MAX_WRITES   2
#define PSVM_PLAN_WRITE_MAX    8
/* Enforce the concatenated buffer invariant: PSVM_PLAN_MAX_READS (4) slots of
 * PSVM_PLAN_SLOT (16) bytes each must exactly fill PSVM_PAYLOAD_MAX (64). Task 3
 * compiles slot offsets as compile-time constants in the browser's JavaScript
 * version of these numbers; a silent mismatch between two independent copies
 * would produce an out-of-bounds access in the last slot. This assertion makes
 * it a compile failure instead. */
_Static_assert(PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT == PSVM_PAYLOAD_MAX,
               "concatenated GATT read buffer: PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT must equal PSVM_PAYLOAD_MAX");

/* M5b: a wrapper may carry a declarative ACTION table as a second trailing
 * section, after the connect plan when both are present. Gated by its own
 * flag bit so a blob with only one of the two parses unambiguously.
 *
 * On-blob layout (spec section 2), little-endian throughout:
 *   u8  action_count            (1..PSVM_ACTION_MAX)
 *   action_count x {
 *     u8  action_id             (< ACTION_COUNT, and unique within this table)
 *     u16 param_max             (<= the firmware action table's param_max)
 *     u8  flags                 bit 0: device-local timed-off
 *                               bit 1: has confirm
 *                               bits 2-7 reserved, must be 0
 *     u16 write_uuid16
 *     u8  write_len             (1..PSVM_PLAN_WRITE_MAX)
 *     u8  write_bytes[write_len]
 *     u8  param_offset          (0xFF = no parameter)
 *     u8  param_encoding        0 u8, 1 u16le, 2 u16be, 0xFF none
 *     -- present only when flags bit 1 is set:
 *     u16 confirm_uuid16
 *     u8  confirm_min_len       (1..PSVM_PLAN_SLOT)
 *     u8  confirm_offset
 *     u8  confirm_encoding      0 u8, 1 u16le, 2 u16be
 *     u8  confirm_op            0 ==, 1 !=
 *     u16 confirm_value
 *   }
 *
 * param_max is checked against action_get(action_id)->param_max here, and
 * may only be LOWER. The hard bound is the firmware's; a wrapper -- which
 * M4 lets an AI write -- may tighten it and may never loosen it.
 *
 * param_offset/param_encoding are a matched pair, validated together: either
 * both are the "no parameter" sentinel (0xFF/0xFF), or param_encoding is
 * 0/1/2 and param_offset + width(param_encoding) <= write_len (width 1 for
 * u8, 2 for u16le/u16be) -- Task 8's executor splices the parameter into a
 * write_len-byte buffer at param_offset, and this is the one function
 * standing between an HTTP client and that splice, so the invariant is
 * established here rather than left for code that has not been written
 * yet. Any other combination (a real encoding with offset==0xFF or vice
 * versa, an offset that would run past write_len, an encoding outside
 * 0/1/2/0xFF) is PSVM_ERR_LIMITS.
 *
 * Two whole-table rules, both PSVM_ERR_LIMITS and both added by the M5b
 * whole-branch review:
 *
 *   - a duration action must declare param_max >= 1. Zero is refused by
 *     action_param_ok() and by actor_table_check()'s effective bound alike,
 *     so a pair declared with it can never fire -- a silent permanent
 *     refusal where an install-time rejection belongs.
 *   - a duration action WITHOUT flags bit 0 is a close the HUB owes (spec
 *     section 4.3), and the hub's only close is ACT_SWITCH_OFF. Such a
 *     table must therefore ALSO declare switch.off, or the obligation
 *     pending_close arms can never be discharged and the actuator stays
 *     open. Checked across the whole table once every entry is parsed. */
#define PSVM_FLAG_ACTION_TABLE 0x0002u
#define PSVM_ACTION_MAX        4

typedef enum { PSVM_OK = 0, PSVM_ERR_HEADER, PSVM_ERR_LIMITS, PSVM_ERR_TRUNCATED,
               PSVM_ERR_BADOP, PSVM_ERR_STACK, PSVM_ERR_STEPS, PSVM_ERR_DIV0,
               PSVM_ERR_JUMP, PSVM_ERR_TYPE, PSVM_ERR_REF } psvm_err_t;

typedef struct { float value; uint32_t age_s; bool ready; } psvm_ref_val_t;

/* kind: 0 plant, 1 device; field: 0 value, 1 age (spec section 2) */
typedef struct { uint8_t kind; uint16_t name_const; uint8_t capability; uint8_t field; } psvm_ref_t;

/* Builtin sink: builtin 0=log 1=notify, msg NUL-terminated (may be truncated
 * to PSVM_STRBUF-1). Return false to abort the run (treated as PSVM_ERR_TYPE).
 * Rules dialect only -- a wrapper program never emits CALL_BUILTIN. */
typedef bool (*psvm_sink_t)(void *ctx, uint8_t builtin, const char *msg);

/* CALL_ACTION (0x52, M5b Task 10) sink: rules dialect only, exactly like
 * psvm_sink_t above -- a wrapper program never emits CALL_ACTION either.
 * kind is the ref-table convention (0 plant, 1 device); name is the
 * resolved const-pool string (NUL-terminated, may be truncated), action_id
 * is action.h's id, and param is the value CALL_ACTION popped off the
 * stack, already range-checked into 0..65535 by psvm_run() itself (see its
 * own doc comment below) -- the sink never sees a negative or truncated
 * value. Return false to abort the run (treated as PSVM_ERR_TYPE), the
 * same convention as psvm_sink_t; the real engine sink (rules_engine.c)
 * never does this -- actor_request() already reports its own refusal via
 * alert_post(), so a guard refusal must not also unwind the VM and skip
 * whatever `then` actions follow it. */
typedef bool (*psvm_action_sink_t)(void *ctx, uint8_t kind, const char *name,
                                   uint8_t action_id, uint16_t param);

/* Wrapper dialect (dialect=2) EMIT sink: capability id (EMIT's own u8
 * operand) and the popped numeric value. Called once per buffered emit,
 * only when the run reaches HALT successfully (spec section 3: a failed
 * REQUIRE emits nothing, including anything emitted earlier in the same
 * run -- see psvm_run()'s own doc comment). No return value: unlike
 * psvm_sink_t, an individual emit can never abort the wrapper -- Task 5's
 * capability_encode()/registry_set_cap() path skips+logs an out-of-range
 * value exactly as M2's submit path does, it never unwinds the run. */
typedef void (*psvm_emit_t)(void *ctx, uint8_t capability, float value);

/* AES_CCM (0x6B) decrypt callback. psvm.c stays pure C99+libc with no
 * crypto dependency (same reason engine-specific string handling is kept
 * out of the interpreter via psvm_sink_t) -- the real AES-CCM decrypt
 * (mbedtls, the device's stored bind key, nonce construction) is Task 5's
 * wrapper_exec.c, injected here as a callback.
 *
 * Contract: `payload[offset .. offset+len)` is the ciphertext-plus-tag
 * region (psvm_run() has already checked offset+len fits the CURRENT
 * working payload length and that it runs exactly to the end of it -- see
 * AES_CCM's opcode doc in psvm.c for why "runs to the end" is required).
 * The callback decrypts IN PLACE, overwriting `payload[offset ..]` with
 * plaintext, and reports the plaintext length (<= len) via *out_len; the
 * working payload's length is then set to `offset + *out_len`, so a
 * scheme that strips a trailing MIC/tag naturally shrinks the visible
 * payload. Returns false on ANY failure -- no bind key stored for this
 * device, MIC/tag mismatch, or any other decrypt error -- and
 * psvm_run() then aborts the wrapper with PSVM_ERR_REF, exactly like an
 * unready ref: "no usable plaintext" is the same shape of failure as "no
 * usable value" from the wrapper's point of view. A NULL callback (no key
 * plumbing wired up at all) is handled identically by psvm_run() without
 * ever being called. */
typedef bool (*psvm_aes_ccm_t)(void *ctx, uint8_t offset, uint8_t len,
                               uint8_t *payload, uint8_t payload_len,
                               uint8_t *out_len);

/* Raw advertisement payload for a wrapper-dialect run. psvm_run() copies
 * `len` bytes (clamped to PSVM_PAYLOAD_MAX) into its own working buffer, so
 * `data` need not outlive the call and AES_CCM can decrypt in place without
 * the caller handing over mutable storage. NULL data / len==0 is valid: it
 * just means every payload accessor's bounds check fails immediately with
 * PSVM_ERR_REF. */
typedef struct {
    const uint8_t *data;
    uint8_t        len;
} psvm_payload_t;

/* Wrapper-dialect (dialect=2) execution I/O, bundled into one optional
 * pointer so a rules-dialect call site only ever adds one `NULL` argument.
 * NULL wio (or a NULL member within it) is always a well-defined "not
 * available" state -- see psvm_payload_t, psvm_emit_t and psvm_aes_ccm_t's
 * own doc comments -- never undefined behaviour. A rules-dialect run
 * (dialect=1) always passes wio=NULL; it has no payload, never executes
 * EMIT/AES_CCM, so nothing here is ever touched. */
typedef struct {
    psvm_payload_t payload;
    psvm_emit_t    emit;
    void          *emit_ctx;
    psvm_aes_ccm_t aes_ccm;
    void          *aes_ccm_ctx;
} psvm_wrapper_io_t;

typedef struct {
    const uint8_t *blob; size_t len;          /* validated PSBC */
    uint16_t const_count, ref_count, code_len;
    uint32_t builtins;                         /* header bitmap */
    const uint8_t *consts, *refs, *code;       /* section pointers */
    /* M5a connect plan (PSVM_FLAG_CONNECT_PLAN): NULL/0 when the flag is
     * clear, otherwise points at the validated trailing plan section (see
     * PSVM_FLAG_CONNECT_PLAN's own doc comment above for the on-blob
     * layout). Tasks 3 and 6 read these. */
    const uint8_t *plan; uint16_t plan_len;
    /* M5b action table (PSVM_FLAG_ACTION_TABLE): NULL/0 when the flag is
     * clear, otherwise points at the validated trailing action section
     * (see PSVM_FLAG_ACTION_TABLE's own doc comment above for the on-blob
     * layout). Follows the connect plan when both are present. */
    const uint8_t *actions; uint16_t actions_len;
} psvm_prog_t;

/* Parses+bounds-checks the blob (spec section 2). dialect = the ONE
 * dialect this call site accepts (PSVM_DIALECT_RULES or
 * PSVM_DIALECT_WRAPPERS) -- a blob whose header byte says otherwise is
 * PSVM_ERR_HEADER, so the rules engine can never load a wrapper blob and
 * vice versa (spec section 3's "the hub rejects unknown dialects... exactly
 * as it does for rules"). caps_max = highest capability id this firmware
 * knows; builtins_impl = bitmap of implemented CALL_BUILTIN ids (rules
 * dialect only -- pass 0 for wrappers, which never use CALL_BUILTIN).
 *
 * caps_max also bounds EVERY capability id a validated blob can reference,
 * not just the ref table's: EMIT's (0x69, wrapper dialect) inline
 * capability-id operand is range-checked the same way (added M3 Task 5
 * review round 2) -- a wrapper naming a nonexistent capability is
 * PSVM_ERR_REF at validate time, not a silent, permanent, every-run runtime
 * failure. */
psvm_err_t psvm_validate(const uint8_t *blob, size_t len, uint8_t dialect,
                         uint8_t caps_max, uint32_t builtins_impl, psvm_prog_t *out);

/* Ref metadata accessors for the engine's resolver (index < ref_count). */
psvm_ref_t psvm_get_ref(const psvm_prog_t *p, uint16_t idx);
/* Returns const-pool string (tag 1) or NULL; len_out optional. */
const char *psvm_get_str(const psvm_prog_t *p, uint16_t idx, uint16_t *len_out);

typedef struct { bool cond; psvm_err_t err; uint32_t steps_used; } psvm_result_t;

/* Runs the program.
 *
 * Rules dialect (spec section 3, unchanged by M3): executes to HALT_BOOL
 * for cond; if run_actions && cond, continues to HALT calling sink for
 * builtins and action_sink for CALL_ACTION (M5b Task 10). resolved[] must
 * have ref_count entries. A not-ready ref touched by execution aborts with
 * PSVM_ERR_REF (engine reports "not ready"). wio is unused (pass NULL).
 *
 * CALL_ACTION (0x52) pops its parameter off the stack as a float and
 * converts it to the uint16_t action_sink expects; a negative value, NaN,
 * or anything above 65535 is PSVM_ERR_TYPE rather than being silently
 * truncated (the parameter is a duration in seconds fed straight to an
 * actuator -- turning a compiler or hand-crafted-blob bug into a 65535 s
 * irrigation run instead of a clean rejection is exactly the failure shape
 * this project exists to avoid). A NULL action_sink is PSVM_ERR_REF, the
 * same shape an unready ref uses -- a rules blob can reach psvm_run() in a
 * dry-run context where no real sink is wired at all, and that must fail
 * cleanly rather than dereference NULL.
 *
 * Wrapper dialect (M3 spec section 3): resolved is unused (pass NULL --
 * wrapper bytecode never emits LOAD_REF, ref_count is always 0). wio
 * supplies the advertisement payload and the EMIT/AES_CCM callbacks (NULL
 * wio, or NULL members within it, are well-defined "not available" states,
 * see psvm_wrapper_io_t). A decode program is a flat sequence of
 * REQUIRE/EMIT/AES_CCM/accessor instructions ending in HALT: EMIT buffers
 * (capability, value) pairs rather than calling wio->emit immediately, and
 * that buffer is flushed through wio->emit ONLY on reaching HALT; REQUIRE
 * popping false ends the run at PSVM_OK without ever reaching HALT, so
 * nothing buffered -- including anything EMIT'd earlier in the very same
 * run -- is ever flushed. This is the actual mechanism behind "a failed
 * require emits nothing": it is not a special case, it falls straight out
 * of "only HALT flushes". run_actions is ignored (wrapper code never
 * contains HALT_BOOL). */
psvm_result_t psvm_run(const psvm_prog_t *p, const psvm_ref_val_t *resolved,
                       const psvm_wrapper_io_t *wio,
                       psvm_sink_t sink, void *sink_ctx,
                       psvm_action_sink_t action_sink, void *action_sink_ctx,
                       bool run_actions);
