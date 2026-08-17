import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  PSVM_FLAG_CONNECT_PLAN, PSVM_PLAN_MAX_READS, PSVM_PLAN_MAX_WRITES,
  PSVM_PLAN_WRITE_MAX, PSVM_PLAN_SLOT, PSVM_PAYLOAD_MAX,
  PSVM_FLAG_ACTION_TABLE, PSVM_ACTION_MAX, ACTIONS, ACTION_ENCODING,
} from '../plan-limits.js'

// plan-limits.js is a hand-kept JavaScript copy of constants whose ground
// truth is components/psvm/include/psvm.h -- a browser compiler cannot
// #include a C header. Nothing else enforces that the two copies agree; if
// this test fails, the fix is almost always to edit plan-limits.js to match
// psvm.h, NOT to edit this test. A silent mismatch would let the compiler
// assign a read past PSVM_PAYLOAD_MAX's 64-byte working buffer -- an
// out-of-bounds read that only shows up on real hardware.
test('plan-limits.js constants match components/psvm/include/psvm.h (ground truth) -- check psvm.h if this fails', () => {
  assert.equal(PSVM_FLAG_CONNECT_PLAN, 0x0001, 'PSVM_FLAG_CONNECT_PLAN drifted from psvm.h')
  assert.equal(PSVM_PLAN_MAX_READS, 4, 'PSVM_PLAN_MAX_READS drifted from psvm.h')
  assert.equal(PSVM_PLAN_MAX_WRITES, 2, 'PSVM_PLAN_MAX_WRITES drifted from psvm.h')
  assert.equal(PSVM_PLAN_WRITE_MAX, 8, 'PSVM_PLAN_WRITE_MAX drifted from psvm.h')
  assert.equal(PSVM_PLAN_SLOT, 16, 'PSVM_PLAN_SLOT drifted from psvm.h')
  assert.equal(PSVM_PAYLOAD_MAX, 64, 'PSVM_PAYLOAD_MAX drifted from psvm.h')
})

// Mirrors psvm.h's own
// `_Static_assert(PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT == PSVM_PAYLOAD_MAX, ...)`.
// plan-limits.js also throws this at import time; this test additionally
// pins it so a future edit that silences the throw (or narrows it) still
// gets caught here.
test('concatenated GATT read buffer invariant: PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT === PSVM_PAYLOAD_MAX -- check psvm.h if this fails', () => {
  assert.equal(
    PSVM_PLAN_MAX_READS * PSVM_PLAN_SLOT, PSVM_PAYLOAD_MAX,
    "psvm.h's _Static_assert has no JS-side mirror here -- fix plan-limits.js to match psvm.h"
  )
})

// M5b action table (PSVM_FLAG_ACTION_TABLE). Same "hand-kept copy, pinned by
// this test" situation as the connect-plan constants above -- see
// plan-limits.js's own doc comment for the failure mode a drifted copy
// causes. ACTIONS mirrors action.h's ACTION_COUNT table (ids and hard
// paramMax bounds); ACTION_ENCODING mirrors PSVM_FLAG_ACTION_TABLE's
// param_encoding/confirm_encoding spellings (0 u8, 1 u16le, 2 u16be).
test('plan-limits.js action-table constants match psvm.h/action.h (ground truth) -- check psvm.h if this fails', () => {
  assert.equal(PSVM_FLAG_ACTION_TABLE, 0x0002, 'PSVM_FLAG_ACTION_TABLE drifted from psvm.h')
  assert.equal(PSVM_ACTION_MAX, 4, 'PSVM_ACTION_MAX drifted from psvm.h')
  assert.deepEqual(ACTIONS, {
    'switch.on':       { id: 0, param: null,         paramMax: 0 },
    'switch.off':      { id: 1, param: null,         paramMax: 0 },
    'irrigation.open': { id: 2, param: 'duration_s', paramMax: 300 },
    'pump.run':        { id: 3, param: 'duration_s', paramMax: 120 },
  }, 'ACTIONS drifted from action.h')
  assert.deepEqual(ACTION_ENCODING, {
    u8:    { id: 0, width: 1 },
    u16le: { id: 1, width: 2 },
    u16be: { id: 2, width: 2 },
  }, 'ACTION_ENCODING drifted from psvm.h')
})
