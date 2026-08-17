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

// M5b action table (spec section 2; on-blob layout owned by psvm.h's
// PSVM_FLAG_ACTION_TABLE doc comment -- psvm.h is GROUND TRUTH). Same
// hand-kept-copy situation as the connect-plan constants above: nothing on
// this side of the language boundary enforces that these agree with
// psvm.h or action.h. tests/plan-limits.test.mjs pins them by hand.
export const PSVM_FLAG_ACTION_TABLE = 0x0002
export const PSVM_ACTION_MAX = 4
// Mirrors action.h's ACTION_COUNT and the table's hard bounds. The compiler
// checks `max` against these so an author sees the error at compile time
// rather than at install; the firmware checks again and is authoritative.
export const ACTIONS = {
  'switch.on':       { id: 0, param: null,         paramMax: 0 },
  'switch.off':      { id: 1, param: null,         paramMax: 0 },
  'irrigation.open': { id: 2, param: 'duration_s', paramMax: 300 },
  'pump.run':        { id: 3, param: 'duration_s', paramMax: 120 },
}

// Wire-format encodings for an action's spliced write parameter and its
// confirm value (psvm.h's PSVM_FLAG_ACTION_TABLE doc comment: "0 u8, 1
// u16le, 2 u16be"). Shared by parser.js (bounds-checking, e.g. confirm_min_len
// and write_len) and codegen.js/disasm.js (encoding the param_encoding /
// confirm_encoding byte and rendering it back), so the id<->name<->width
// mapping exists in exactly one place.
export const ACTION_ENCODING = {
  u8:    { id: 0, width: 1 },
  u16le: { id: 1, width: 2 },
  u16be: { id: 2, width: 2 },
}
