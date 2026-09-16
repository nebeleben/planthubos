import { test } from 'node:test'
import assert from 'node:assert/strict'
import { fmtCap, fmtCapParts } from '../caps.js'

// dim.rotate (id 10, zigbee-command-controller amendment): a signed integer
// from a knob remote, positive = clockwise, negative = counter-clockwise.
// caps.js decodes it via a name lookup in the live capabilities Map, not a
// static table entry, so the test builds that Map by hand rather than going
// through loadCaps() (which would hit the network).
const caps = new Map([[10, { id: 10, name: 'dim.rotate' }]])

test('fmtCap: dim.rotate positive value renders a CW label with a signed count', () => {
  const text = fmtCap(caps, 10, 6)
  assert.match(text, /CW/)
  assert.match(text, /\+6/)
})

test('fmtCap: dim.rotate negative value renders a CCW label with a signed count', () => {
  const text = fmtCap(caps, 10, -3)
  assert.match(text, /CCW/)
  assert.match(text, /-3/)
})

test('fmtCap: dim.rotate null value (never reported) renders the waiting label', () => {
  assert.equal(fmtCap(caps, 10, null), 'waiting for a turn')
})

test('fmtCap: dim.rotate zero value renders "0"', () => {
  assert.equal(fmtCap(caps, 10, 0), '0')
})

test('fmtCapParts: dim.rotate carries no unit', () => {
  assert.equal(fmtCapParts(caps, 10, 6).unit, '')
})
