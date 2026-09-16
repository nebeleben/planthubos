import { test } from 'node:test'
import assert from 'node:assert/strict'
import { resolveVendor } from '../vendors.js'

test('resolveVendor: known company id wins', () => {
  assert.equal(resolveVendor({ id: 'ble:AABBCC001122', company_id: 0x0499 }), 'Ruuvi Innovations')
  assert.equal(resolveVendor({ id: 'ble:AABBCC001122', company_id: 0x038f }), 'Xiaomi')
})

test('resolveVendor: unknown company id shows the raw value, not blank', () => {
  assert.equal(resolveVendor({ id: 'ble:AABBCC001122', company_id: 0x1234 }), 'Company 0x1234')
})

test('resolveVendor: falls back to OUI when no company id', () => {
  assert.equal(resolveVendor({ id: 'ble:2CCF67ABCDEF', company_id: 0 }), 'Espressif')
  assert.equal(resolveVendor({ id: 'ble:2CCF67ABCDEF' }), 'Espressif')          // company_id absent
})

test('resolveVendor: unknown OUI shows the raw prefix', () => {
  assert.equal(resolveVendor({ id: 'ble:A1B2C3D4E5F6', company_id: 0 }), 'OUI A1B2C3')
})

test('resolveVendor: company id preferred over OUI', () => {
  // known company id + a would-be OUI: company id must win
  assert.equal(resolveVendor({ id: 'ble:2CCF67ABCDEF', company_id: 0x0499 }), 'Ruuvi Innovations')
})

test('resolveVendor: nothing usable -> null', () => {
  assert.equal(resolveVendor({ id: 'zb:545962A66C38C1A4', company_id: 0 }), null)
  assert.equal(resolveVendor({}), null)
  assert.equal(resolveVendor(null), null)
})
