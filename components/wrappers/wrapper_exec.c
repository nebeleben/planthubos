/* wrapper_exec.c -- see wrapper_exec.h for the full contract. */
#include "wrapper_exec.h"
#include "wrapper_arena.h"
#include "psvm.h"
#include "capability.h"
#include "data_core.h"
#include "esp_log.h"

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
     * skip-on-CAP_VALUE_NONE + a WARN log on that skip (see data_core.h's
     * own doc comment on it) -- exactly the discipline the M3 Task 5 brief
     * asks EMIT's sink to have, satisfied here by reusing the same,
     * already-tested path M3 Task 3's BTHome dispatch uses, rather than a
     * second copy of it. */
    if (data_core_submit_cap(e->mac, capability, value)) {
        e->wrote_any = true;
    }
}

bool wrapper_exec_run(uint16_t id, const uint8_t mac[6],
                      const uint8_t *payload, uint8_t payload_len)
{
    size_t blob_len = 0;
    const uint8_t *blob = wrapper_arena_get(id, &blob_len);
    if (!blob) {
        ESP_LOGW(TAG, "wrapper %u: bytecode unavailable (arena miss/refusal), skipping", (unsigned)id);
        return false;
    }

    psvm_prog_t prog;
    psvm_err_t verr = psvm_validate(blob, blob_len, PSVM_DIALECT_WRAPPERS,
                                    CAPABILITY_COUNT - 1, 0, &prog);
    if (verr != PSVM_OK) {
        ESP_LOGW(TAG, "wrapper %u: bytecode failed validation (err=%d), skipping", (unsigned)id, (int)verr);
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
    if (res.err != PSVM_OK) {
        ESP_LOGD(TAG, "wrapper %u: run ended err=%d after %u step(s)",
                 (unsigned)id, (int)res.err, (unsigned)res.steps_used);
    }
    return ectx.wrote_any;
}
