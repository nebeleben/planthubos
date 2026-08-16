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
 *
 * A wrapper that fails to load (arena refusal/miss) or fails validation (a
 * corrupt/tampered blob -- installed wrappers are validated at install time
 * too, Task 7, so this should be rare) logs a WARN and simply emits
 * nothing; never a fatal error for the decoder task.
 *
 * Returns true iff at least one capability value was actually written (i.e.
 * data_core_submit_cap() returned true for at least one EMIT) -- callers
 * use this to decide whether to wake the rules engine, same convention as
 * ble_collector.c's decode_bthome_item()/wrote_any. */
bool wrapper_exec_run(uint16_t id, const uint8_t mac[6],
                      const uint8_t *payload, uint8_t payload_len);
