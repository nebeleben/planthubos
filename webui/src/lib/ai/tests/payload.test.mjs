import { test } from 'node:test'
import assert from 'node:assert/strict'
import { sliceWrapperPayload } from '../payload.js'

// Small AD-structure builder so each advert below reads as "what it
// contains", not as a hand-counted hex string. `type`/`content` are hex
// strings; the length byte is computed, not typed by hand, so a mistake in
// a fixture can't accidentally hide a bug in the code under test.
function adStruct(type, content) {
  const contentBytes = content.length / 2
  const len = (1 + contentBytes).toString(16).padStart(2, '0')
  return len + type + content
}

const FLAGS = adStruct('01', '06')               // AD type 0x01, generic flags -- filler, matches nothing
const LOCAL_NAME = adStruct('09', '506c616e74')   // AD type 0x09 "complete local name" -- filler, matches nothing

test('manufacturer match: structure is not first in the advert', () => {
  const mfg = adStruct('ff', '9904' + '020e04ff01')   // company 0x0499, data 020e04ff01
  const advert = FLAGS + mfg
  assert.equal(sliceWrapperPayload(advert, 1), '020e04ff01')
})

test('service match: several preceding structures', () => {
  const svc = adStruct('16', '4afc' + 'aabbccdd')    // uuid 0xfc4a, data aabbccdd
  const advert = FLAGS + LOCAL_NAME + FLAGS + svc
  assert.equal(sliceWrapperPayload(advert, 0), 'aabbccdd')
})

test('mac_prefix returns the whole captured advert, unsliced', () => {
  const mfg = adStruct('ff', '9904' + 'aabb')
  const advert = FLAGS + mfg
  assert.equal(sliceWrapperPayload(advert, 2), advert)
})

test('matched structure absent from the advert: empty string, never the raw advert', () => {
  const advert = FLAGS + LOCAL_NAME   // no manufacturer, no service data anywhere
  assert.equal(sliceWrapperPayload(advert, 1), '')
  assert.equal(sliceWrapperPayload(advert, 0), '')
})

test('a truncated AD structure (length byte overruns the buffer) is not read past', () => {
  // A real manufacturer structure the slicer SHOULD find, followed by a
  // trailing length byte claiming 0x10 (16) more bytes when only 2 remain
  // -- ble_hs_adv_parse_fields() stops there; this must too, not throw and
  // not read off the end of the array.
  const mfg = adStruct('ff', '9904' + 'aabb')
  const advert = FLAGS + mfg + '10' + 'ff00'
  assert.equal(sliceWrapperPayload(advert, 1), 'aabb')
})

test('a truncated structure with nothing valid before it yields empty, not a throw', () => {
  const advert = FLAGS + '10' + 'ff00'
  assert.doesNotThrow(() => sliceWrapperPayload(advert, 1))
  assert.equal(sliceWrapperPayload(advert, 1), '')
})

test('two manufacturer structures: the LAST one wins, matching ble_hs_adv_parse_fields()\'s overwrite', () => {
  const first = adStruct('ff', '9904' + '1111')   // company 0x0499, data 1111
  const second = adStruct('ff', '0001' + '2222')  // company 0x0100, data 2222
  const advert = FLAGS + first + second
  assert.equal(sliceWrapperPayload(advert, 1), '2222')
})

test('two service-data structures: the LAST one wins', () => {
  const first = adStruct('16', '4afc' + '1111')
  const second = adStruct('16', '0918' + '2222')
  const advert = FLAGS + first + second
  assert.equal(sliceWrapperPayload(advert, 0), '2222')
})
