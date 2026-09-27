import { test } from 'node:test'
import assert from 'node:assert/strict'
import { lookupCap, CAPS_BY_ID } from '../caps.js'

test('electric.* caps are known to the compiler', () => {
  assert.equal(lookupCap('electric.power').id, 11)
  assert.equal(lookupCap('electric.voltage').unit, 'V')
  assert.equal(CAPS_BY_ID[13].name, 'electric.current')
})
