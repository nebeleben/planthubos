// M5a connect-plan constants (spec section 2; on-blob layout and caps owned
// by components/psvm/include/psvm.h -- psvm.h is GROUND TRUTH).
//
// A browser compiler cannot #include a C header, so this file is a
// hand-kept JavaScript copy of five numbers psvm.h also defines:
// PSVM_FLAG_CONNECT_PLAN, PSVM_PLAN_MAX_READS, PSVM_PLAN_MAX_WRITES,
// PSVM_PLAN_WRITE_MAX, PSVM_PLAN_SLOT and PSVM_PAYLOAD_MAX. Nothing on this
// side of the language boundary enforces that the two copies agree. If they
// ever drift -- someone raises PSVM_PLAN_MAX_READS in psvm.h without
// touching this file, say -- the compiler keeps producing well-formed-
// LOOKING bytecode, but the last read slot it assigns runs past
// PSVM_PAYLOAD_MAX's 64-byte working buffer on real hardware, silently.
// That is the exact shape of the worst defects in the last three
// milestones of this project.
//
// The only thing standing between a drifted copy and that silent
// out-of-bounds read is tests/plan-limits.test.mjs, which pins every value
// below against psvm.h by hand. If you change a number here, change it in
// psvm.h in the SAME commit, and watch that test fail-then-pass before
// trusting it.
export const PSVM_FLAG_CONNECT_PLAN = 0x0001
export const PSVM_PLAN_MAX_READS = 4
export const PSVM_PLAN_MAX_WRITES = 2
export const PSVM_PLAN_WRITE_MAX = 8
export const PSVM_PLAN_SLOT = 16
export const PSVM_PAYLOAD_MAX = 64

// Mirrors psvm.h's own `_Static_assert(PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT
// == PSVM_PAYLOAD_MAX, ...)`. JS has no compile-time assertions, so this
// runs at import time instead: a drifted copy fails the moment anything
// imports this module, rather than quietly compiling a wrapper whose last
// slot reads out of bounds on-device.
if (PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT !== PSVM_PAYLOAD_MAX) {
  throw new Error(
    'plan-limits.js: PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT must equal ' +
    'PSVM_PAYLOAD_MAX -- check components/psvm/include/psvm.h and fix ' +
    'this file to match'
  )
}
