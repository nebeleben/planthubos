import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  PSVM_FLAG_CONNECT_PLAN, PSVM_PLAN_MAX_READS, PSVM_PLAN_MAX_WRITES,
  PSVM_PLAN_WRITE_MAX, PSVM_PLAN_SLOT, PSVM_PAYLOAD_MAX,
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
