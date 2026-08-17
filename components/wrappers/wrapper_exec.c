/* wrapper_exec.c -- see wrapper_exec.h for the full contract. */
#include "wrapper_exec.h"
#include "wrapper_arena.h"
#include "wrapper_index.h"
#include "psvm.h"
#include "capability.h"
#include "data_core.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wrapper_exec";

/* Threaded through psvm_run() as wio->emit_ctx (see psvm_emit_t's own
 * contract, psvm.h): the mac EMIT needs to reach data_core_submit_cap(),
 * plus whether anything was actually written (for the caller's
 * rules_notify_value_update() decision). */
typedef struct {
    const uint8_t *mac;
    bool wrote_any;
} emit_ctx_t;

static void emit_sink(void *ctx, uint8_t capability, float value)
{
    emit_ctx_t *e = (emit_ctx_t *)ctx;
    /* data_core_submit_cap() already owns capability_encode() +
     * skip-on-CAP_VALUE_NONE + a (throttled) WARN log on that skip (see
     * data_core.h's own doc comment on it) -- exactly the discipline the M3
     * Task 5 brief asks EMIT's sink to have, satisfied here by reusing the
     * same, already-tested path M3 Task 3's BTHome dispatch uses, rather
     * than a second copy of it. */
    if (data_core_submit_cap(e->mac, capability, value)) {
        e->wrote_any = true;
    }
}

/* Task 5 review ruling on AES_CCM (endorsing the "stays unwired" call, but
 * requiring the failure not be invisible): today wio.aes_ccm is always NULL
 * (see wrapper_exec_run()'s wio setup below and wrapper_exec.h's full
 * reasoning), so a wrapper that calls aes_ccm_decrypt(...) deterministically
 * fails every single run at PSVM_ERR_REF -- but PSVM_ERR_REF is also the
 * error for an ordinary out-of-range payload accessor (expected background
 * noise on a truncated/malformed advert, decode_bthome_item()'s own
 * precedent, DEBUG-only), so the two can't be told apart from psvm_run()'s
 * result alone. This walks the VALIDATED (already bounds-checked by
 * psvm_validate()) wrapper-dialect instruction stream looking for the
 * AES_CCM opcode (0x6B) itself, using the same per-opcode operand widths
 * psvm.c's own interpreter switch uses for dialect=2 (opcodes
 * 0x00/0x10-0x13/0x20-0x25/0x30-0x32/0x40-0x41/0x50-0x51/0x60-0x6C/0xFF --
 * M3 spec section 3's fixed opcode set). This DUPLICATES that one piece of
 * knowledge (never opcode semantics) outside psvm.c specifically for this
 * diagnostic; if psvm.c's opcode set ever changes this table needs the same
 * update (same kind of small, contained, documented duplication as
 * bindkey_core.h/bindkey.c's NVS_KEY_NAME_MAX_SIZE static_assert). An
 * opcode this table doesn't recognise just stops the scan (psvm_run() is
 * still the sole authority on whether the bytecode is valid; this function
 * only ever answers "did I find 0x6B before I ran out of table", never
 * "is this bytecode well-formed"). */
static bool code_uses_aes_ccm(const psvm_prog_t *p)
{
    uint16_t pc = 0;
    while (pc < p->code_len) {
        uint8_t op = p->code[pc];
        if (op == 0x6B) return true;
        uint16_t width;
        switch (op) {
        case 0x01: case 0x02: case 0x40: case 0x41:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66:
            width = 3; break;
        case 0x67:
            width = 5; break;
        case 0x50: case 0x51: case 0x69:
            width = 2; break;
        case 0x00: case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x30: case 0x31: case 0x32:
        case 0x68: case 0x6A: case 0x6C: case 0xFF:
            width = 1; break;
        default:
            return false;   /* unrecognised opcode -- stop; not this function's job to judge validity */
        }
        pc = (uint16_t)(pc + width);
    }
    return false;
}

/* One bit per wrapper id -- "already logged the elevated
 * AES_CCM-unsupported warning for this id since boot". Without this, a
 * genuinely encryption-using wrapper would re-log its WARN on every single
 * advertisement it matches (the exact firehose shape review FINDING 3
 * flagged for data_core.c's own skip warning) -- loud once, silent after,
 * same policy. */
static uint16_t s_aes_ccm_warned;
_Static_assert(WRAPPERS_MAX <= 16, "s_aes_ccm_warned's per-wrapper bitmask needs one bit per wrapper id, all within a uint16_t");

/* Cheap pre-check, deliberately separate from the function that actually
 * warns+marks below -- checked BEFORE code_uses_aes_ccm()'s scan runs (Task
 * 5 review round 2 nit), so a wrapper that's already been warned once
 * doesn't pay for re-scanning its bytecode on every subsequent failed
 * advert for the rest of the boot. Out-of-range ids (defensive; never
 * happens in practice, ids are always < WRAPPERS_MAX) read as "already
 * warned" -- nothing to warn about for an id with no bit to track it. */
static bool aes_ccm_already_warned(uint16_t id)
{
    if (id >= WRAPPERS_MAX) return true;
    return (s_aes_ccm_warned & (uint16_t)(1u << id)) != 0;
}

static void warn_aes_ccm_unsupported(uint16_t id)
{
    if (id < WRAPPERS_MAX) s_aes_ccm_warned |= (uint16_t)(1u << id);
    ESP_LOGW(TAG, "wrapper %u uses aes_ccm_decrypt(), which this firmware build does not "
             "support (no AES-CCM callback wired) -- every run of this wrapper will fail "
             "and emit nothing; native BTHome decryption is unaffected", (unsigned)id);
}

/* Short human text for a psvm_err_t, for wrapper_store_note_error()'s
 * last_error field (Task 7, spec §6's GET /api/v1/wrappers "last_error").
 * Deliberately its own small copy rather than exporting wrapper_store.c's
 * private psvm_err_str() -- same "psvm_err_t's value set is a fixed,
 * spec'd contract" reasoning api_v1.c's own psvm_err_short() already gives
 * for keeping a second, independent copy instead of a shared header. */
static const char *wrapper_err_short(psvm_err_t e)
{
    switch (e) {
    case PSVM_OK:            return "";
    case PSVM_ERR_HEADER:    return "bad bytecode header";
    case PSVM_ERR_LIMITS:    return "bytecode exceeds a hub limit";
    case PSVM_ERR_TRUNCATED: return "bytecode truncated";
    case PSVM_ERR_BADOP:     return "bad opcode";
    case PSVM_ERR_STACK:     return "stack error";
    case PSVM_ERR_STEPS:     return "step budget exceeded";
    case PSVM_ERR_DIV0:      return "division by zero";
    case PSVM_ERR_JUMP:      return "bad jump target";
    case PSVM_ERR_TYPE:      return "type error";
    case PSVM_ERR_REF:       return "bad reference";
    default:                 return "vm error";
    }
}

/* The whole of a wrapper run except the match counter: resolve the blob,
 * validate it, run it over `payload`, and record/clear last_error. Shared
 * verbatim by the advertisement entry point below and by M5a's GATT buffer
 * entry point (wrapper_exec_run_buffer()) -- the two differ ONLY in which
 * bytes they hand in and in whether the match counter is bumped (see
 * wrapper_exec.h for why the GATT path must not bump it), so this is one
 * implementation rather than two copies that could drift in their EMIT
 * sink, their step budget or their error reporting. */
static bool run_payload(uint16_t id, const uint8_t mac[6],
                        const uint8_t *payload, uint8_t payload_len)
{
    size_t blob_len = 0;
    const uint8_t *blob = wrapper_arena_get(id, &blob_len);
    if (!blob) {
        ESP_LOGW(TAG, "wrapper %u: bytecode unavailable (arena miss/refusal), skipping", (unsigned)id);
        wrapper_store_note_error(id, "bytecode unavailable (arena miss/refusal)");
        return false;
    }

    psvm_prog_t prog;
    psvm_err_t verr = psvm_validate(blob, blob_len, PSVM_DIALECT_WRAPPERS,
                                    CAPABILITY_COUNT - 1, 0, &prog);
    if (verr != PSVM_OK) {
        ESP_LOGW(TAG, "wrapper %u: bytecode failed validation (err=%d), skipping", (unsigned)id, (int)verr);
        wrapper_store_note_error(id, wrapper_err_short(verr));
        return false;
    }

    emit_ctx_t ectx = { .mac = mac, .wrote_any = false };
    psvm_wrapper_io_t wio = {
        .payload = { .data = payload, .len = payload_len },
        .emit = emit_sink,
        .emit_ctx = &ectx,
        /* AES_CCM deliberately not wired -- see wrapper_exec.h's doc
         * comment for the full reasoning. NULL is a well-defined "not
         * available" state (psvm.h): a wrapper using aes_ccm_decrypt(...)
         * ends its run at PSVM_ERR_REF without ever calling this. */
        .aes_ccm = NULL,
        .aes_ccm_ctx = NULL,
    };
    /* resolved=NULL, sink=NULL/NULL, run_actions=false: none of these apply
     * to the wrapper dialect -- see psvm_run()'s own doc comment (psvm.h).
     * The VM's own PSVM_MAX_STEPS budget bounds this call; nothing here
     * adds a second one (M3 Task 5 brief's "enforce M1's step budget" is
     * satisfied by simply calling psvm_run() normally -- that budget is
     * unconditional inside psvm_run() itself, not opt-in). */
    psvm_result_t res = psvm_run(&prog, NULL, &wio, NULL, NULL, false);
    if (res.err == PSVM_ERR_REF && !aes_ccm_already_warned(id) && code_uses_aes_ccm(&prog)) {
        /* Elevated per the Task 5 review ruling: this specific, structural,
         * every-run-fails case must not be invisible at DEBUG. Checked
         * already-warned FIRST (cheap) so the scan only ever runs once per
         * wrapper id, not on every single failed advert. */
        warn_aes_ccm_unsupported(id);
        wrapper_store_note_error(id, "aes_ccm_decrypt unsupported in this build");
    } else if (res.err != PSVM_OK) {
        ESP_LOGD(TAG, "wrapper %u: run ended err=%d after %u step(s)",
                 (unsigned)id, (int)res.err, (unsigned)res.steps_used);
        wrapper_store_note_error(id, wrapper_err_short(res.err));
    } else {
        /* PSVM_OK covers BOTH a clean HALT and a cleanly-rejected REQUIRE
         * (psvm.h: "REQUIRE popping false ends the run at PSVM_OK... a
         * failed require emits nothing") -- neither is an error, so clear
         * any earlier last_error rather than leaving a stale one visible
         * after the wrapper starts working again (e.g. a bind key gets set,
         * or traffic shape changes). */
        wrapper_store_note_error(id, NULL);
    }
    return ectx.wrote_any;
}

bool wrapper_exec_run(uint16_t id, const uint8_t mac[6],
                      const uint8_t *payload, uint8_t payload_len)
{
    /* Task 7 (controller RULING-3): "the only way a user can tell whether a
     * hand-written wrapper is matching anything" -- bumped once per actual
     * invocation, regardless of whether the run below ends up emitting
     * anything, since "matching" is decided by the match-index lookup that
     * already happened before this function was ever called. */
    wrapper_store_note_match(id);
    return run_payload(id, mac, payload, payload_len);
}

bool wrapper_exec_run_buffer(uint16_t id, const uint8_t mac[6],
                             const uint8_t *buf, uint8_t len)
{
    /* No wrapper_store_note_match() here -- see wrapper_exec.h. */
    return run_payload(id, mac, buf, len);
}

uint16_t wrapper_exec_plan_get(uint16_t id, uint8_t *out, uint16_t cap,
                               uint32_t *interval_s_out)
{
    if (interval_s_out) *interval_s_out = 0;

    size_t blob_len = 0;
    const uint8_t *blob = wrapper_arena_get(id, &blob_len);
    if (!blob) return 0;

    psvm_prog_t prog;
    if (psvm_validate(blob, blob_len, PSVM_DIALECT_WRAPPERS,
                      CAPABILITY_COUNT - 1, 0, &prog) != PSVM_OK) {
        /* Deliberately silent: this is a query, not a run. The very next
         * advertisement from this device takes wrapper_exec_run() above,
         * which logs and records the same failure through
         * wrapper_store_note_error() -- doing it twice per advertisement
         * would just double the noise. */
        return 0;
    }
    /* prog.plan is NULL unless PSVM_FLAG_CONNECT_PLAN was set, and
     * psvm_validate() has already bounds-checked the whole section (its
     * header alone is 6 bytes: read_count, write_count, interval_s). */
    if (prog.plan == NULL || prog.plan_len < 6) return 0;

    if (out) {
        /* Checked before interval_s_out is written, so a caller that gets 0
         * back never also gets an interval it might act on. */
        if (prog.plan_len > cap) {
            ESP_LOGW(TAG, "wrapper %u: connect plan is %u B, does not fit the caller's %u B",
                     (unsigned)id, (unsigned)prog.plan_len, (unsigned)cap);
            return 0;
        }
        memcpy(out, prog.plan, prog.plan_len);
    }
    if (interval_s_out) {
        *interval_s_out = (uint32_t)prog.plan[2] | ((uint32_t)prog.plan[3] << 8) |
                          ((uint32_t)prog.plan[4] << 16) | ((uint32_t)prog.plan[5] << 24);
    }
    return prog.plan_len;
}
