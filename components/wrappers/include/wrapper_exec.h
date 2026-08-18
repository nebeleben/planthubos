/* wrapper_exec.h -- M3 Task 5: runs one matched wrapper's bytecode against
 * one advertisement (spec section 3). Its own header for the same reason
 * wrapper_arena.h is its own header (controller ruling on task-5-brief.md):
 * this is a fourth, independent responsibility from the match index, the
 * flash-backed store and the arena, and ble_collector.c is happy to add one
 * more #include for it rather than see them all merged into one file.
 *
 * wrapper_exec.c is ESP-IDF-side (links psvm, data_core, capability -- see
 * components/wrappers/CMakeLists.txt), NOT host-tested: its whole job is
 * gluing already-tested pure pieces together (psvm.c's interpreter,
 * wrapper_arena.c's cache, data_core.c's capability-write discipline), and
 * that gluing has no interesting pure logic of its own to test in isolation
 * -- same reasoning wrapper_store.c and bindkey.c are exempted from
 * tests/host/run.sh for.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Runs wrapper `id`'s bytecode (M3 spec section 3) against one BLE
 * advertisement's decoded payload slice, for the device at mac[6]
 * (DISPLAY/human byte order -- device_id_from_mac()'s and bindkey_get()'s
 * own contract; NOT NimBLE's raw wire-order GAP address, see bthome.h's
 * mac[] doc comment, which this mirrors exactly).
 *
 * Resolves the wrapper's bytecode blob through the shared arena
 * (wrapper_arena_get(), loading it from flash on a cache miss), validates
 * it (dialect 2 = PSVM_DIALECT_WRAPPERS), and runs it (psvm_run()) with:
 *   - EMIT wired straight to data_core_submit_cap(mac, capability, value),
 *     which already owns the capability_encode()/skip-on-CAP_VALUE_NONE/log
 *     discipline (M2's set_cap_or_warn() precedent, already reused by M3
 *     Task 3's BTHome path) -- this file adds no second copy of it.
 *   - The VM's own fixed step budget (PSVM_MAX_STEPS, psvm.h) bounds every
 *     run; nothing here adds a second one.
 *   - AES_CCM is NOT wired up (wio.aes_ccm is NULL, a well-defined "not
 *     available" state per psvm.h -- any wrapper program calling
 *     aes_ccm_decrypt(...) ends its run at PSVM_ERR_REF and emits nothing).
 *     Decrypting through a wrapper needs a concrete, spec-verified
 *     per-vendor AES-CCM nonce construction; BTHome's own (mac || uuid ||
 *     info || counter, bthome.c) is validated against BTHome's PUBLISHED
 *     spec, but nothing published or already in this codebase specifies one
 *     for a generic wrapper, and Xiaomi's own scheme -- the specific case
 *     M3 spec section 4 names as the motivating one -- is neither BTHome-
 *     shaped nor documented here. Guessing one would be exactly the
 *     "plausible-looking wrong reading" class of bug spec section 3 calls
 *     out for `>>`, except worse (silently-wrong decrypted bytes, not just
 *     a fractional leak) and with zero test coverage to catch it. See the
 *     Task 5 report's Deviations section. Native BTHome decryption
 *     (bthome.c) is completely unaffected by this and already works.
 *     Review ruling: this "stays unwired" call is endorsed, but the failure
 *     it causes must not be invisible -- a wrapper whose bytecode actually
 *     calls aes_ccm_decrypt(...) gets an elevated, once-per-boot WARN
 *     naming it and stating encryption is unsupported in this build
 *     (wrapper_exec.c's warn_aes_ccm_unsupported_once()/code_uses_aes_ccm()),
 *     distinguished from an ordinary out-of-range payload accessor (also
 *     PSVM_ERR_REF, but expected background noise on a truncated/malformed
 *     advert -- decode_bthome_item()'s own DEBUG-level precedent) by
 *     scanning the validated instruction stream for the AES_CCM opcode
 *     itself, since psvm_run()'s result alone can't tell the two apart.
 *
 * A wrapper that fails to load (arena refusal/miss) or fails validation (a
 * corrupt/tampered blob -- installed wrappers are validated at install time
 * too, Task 7, so this should be rare) logs a WARN and simply emits
 * nothing; never a fatal error for the decoder task.
 *
 * Returns true iff at least one capability value was actually written (i.e.
 * data_core_submit_cap() returned true for at least one EMIT) -- callers
 * use this to decide whether to wake the rules engine, same convention as
 * ble_collector.c's decode_bthome_item()/wrote_any.
 *
 * M5a gate fix round 2 (2026-08-17): callable ONLY for a wrapper that does
 * NOT carry a connect plan. ble_collector.c's decode_adv_item() checks
 * wrapper_exec_plan_get() before ever reaching a call to this function, and
 * routes a plan-bearing wrapper to wrapper_exec_run_buffer() below instead,
 * once a GATT read actually lands -- never here, never against an
 * advertisement. The hardware gate is why: a connect wrapper's decode
 * addresses named GATT buffers that an advertisement's payload slice
 * doesn't contain, so running it here failed every single time at
 * PSVM_ERR_REF ("bad reference"), reading past the end of an empty/
 * unrelated slice. */
bool wrapper_exec_run(uint16_t id, const uint8_t mac[6],
                      const uint8_t *payload, uint8_t payload_len);

/* M5a Task 6: the same run, over the CONCATENATED GATT READ BUFFER instead
 * of an advertisement payload (design spec section 2). Identical machinery
 * -- same arena, same validation, same EMIT sink into
 * data_core_submit_cap(), same last_error bookkeeping -- because a wrapper
 * with a `connect` block differs only in which bytes its accessors address:
 * PSVM_PLAN_MAX_READS fixed 16-byte slots rather than one advert, which is
 * exactly what let M5a add a GATT path without touching the VM.
 *
 * M5a gate fix round 2 (2026-08-17): THIS is the sole invocation point for a
 * connect wrapper's decode, and so bumps the wrapper's match counter itself
 * (wrapper_store_note_match()), exactly as wrapper_exec_run() above does for
 * an advert wrapper. Earlier, decode_adv_item() also ran wrapper_exec_run()
 * against the triggering advertisement for a connect wrapper, so THIS
 * function deliberately did not bump the counter a second time -- but that
 * advertisement-side run is exactly what the hardware gate found broken
 * (see wrapper_exec_run()'s own updated comment) and it is gone.
 * match_count for a connect wrapper therefore now counts completed GATT
 * reads, not advertisement sightings -- the same "is my hand-written
 * wrapper matching anything at all" signal (Task 7, RULING-3) an advert
 * wrapper's counter gives, just keyed to the event that is actually
 * meaningful for this class of wrapper.
 *
 * Returns true iff at least one capability value was actually written, same
 * convention as wrapper_exec_run(). */
bool wrapper_exec_run_buffer(uint16_t id, const uint8_t mac[6],
                             const uint8_t *buf, uint8_t len);

/* M5a Task 6: reports whether wrapper `id` carries a connect plan
 * (psvm.h's PSVM_FLAG_CONNECT_PLAN trailing section) and hands back a COPY
 * of it. Returns the plan's length in bytes, or 0 when the wrapper has no
 * plan, cannot be loaded, fails validation, or its plan does not fit `cap`.
 *
 * Copies rather than returning a pointer on purpose: the plan lives inside
 * a blob owned by the shared wrapper arena, and ANY later
 * wrapper_arena_get() from ANY caller can evict it (wrapper_arena.h's
 * FINDING 2 doc comment). M5a's GATT engine keeps the plan alive across a
 * whole connection attempt, long after the arena pointer it came from could
 * have gone stale.
 *
 * out may be NULL with cap 0 to ask only "is there a plan, and what
 * interval does it declare" without copying -- the cheap form the
 * per-advertisement trigger in ble_collector.c uses.
 * interval_s_out, when non-NULL, receives the plan's declared read interval
 * in seconds (psvm_validate() has already bounded it to 60..86400), or 0
 * when this function returns 0.
 *
 * Loads through the arena, so like wrapper_exec_run() it may read flash and
 * must only be called from a task where that is allowed. */
uint16_t wrapper_exec_plan_get(uint16_t id, uint8_t *out, uint16_t cap,
                               uint32_t *interval_s_out);

/* M5b Task 8: the same two questions about the ACTION table
 * (PSVM_FLAG_ACTION_TABLE) that wrapper_exec_plan_get() answers about the
 * connect plan -- "does this wrapper declare actuator actions" and "give me
 * the bytes of one". Both COPY out of the arena for the reason stated
 * above: an arena pointer can be evicted by any later wrapper_arena_get()
 * from any caller, and a command's bytes are held across a whole connection
 * attempt on another task. Both load through the arena, so like
 * wrapper_exec_run() they may read flash and must only be called from a
 * task where that is allowed. */
typedef struct {
    uint8_t  action_id;   /* < ACTION_COUNT (psvm_validate() enforced it) */
    uint16_t param_max;   /* the wrapper's own bound, never above the firmware's */
    uint8_t  flags;       /* bit 0 device-local timed-off, bit 1 has confirm */
} wrapper_action_t;

/* Lists what wrapper `id` declares, up to `cap` entries, for the boot/
 * discovery wiring that populates the actor table (actor_declare()).
 * Returns the number written, 0 when the wrapper has no action table,
 * cannot be loaded or fails validation. */
uint8_t wrapper_exec_actions_list(uint16_t id, wrapper_action_t *out, uint8_t cap);

/* Copies the ONE entry for `action_id` in the exact shape
 * gatt_fsm_init_command() parses: a u8 action_count of 1 followed by that
 * entry's bytes, verbatim from the blob. Returns the number of bytes
 * written, or 0 when the wrapper declares no such action, has no action
 * table, or the entry does not fit `cap`. */
uint16_t wrapper_exec_action_get(uint16_t id, uint8_t action_id, uint8_t *out, uint16_t cap);
