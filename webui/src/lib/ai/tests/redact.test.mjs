import { test } from 'node:test'
import assert from 'node:assert/strict'
import { redactUnknownDevice, redactPlants } from '../redact.js'

// Shaped exactly like GET /api/v1/unknown returns (M3 spec section 5).
const DEVICE = {
  id: 'ble:D0CF13E5BCCA',
  rssi: -46,
  last_seen_s: 0,
  samples: [
    { hex: '0513885780C87A0004FFFC040CAC364200CD', len: 18, ts: 40 },
    { hex: '0513905780C87A0004FFFC040CAC364200CE', len: 18, ts: 45 },
  ],
}

const PLANTS = [{
  id: 1,
  name: 'TestPlant',
  bindings: [
    { cap: 0, name: 'soil.moisture', device: 'ble:80EACA892A0A', value: 12, age_s: 46 },
    { cap: 1, name: 'air.temperature', device: 'ble:80EACA892A0A', value: 29.7, age_s: 110 },
  ],
}]

test('keeps the payload hex and the vendor OUI', () => {
  const r = redactUnknownDevice(DEVICE)
  assert.equal(r.ouiHex, 'D0CF13')
  assert.equal(r.sampleCount, 2)
  assert.equal(r.spacingSeconds, 5)
  assert.deepEqual(r.samples.map((s) => s.hex), DEVICE.samples.map((s) => s.hex))
})

// The property that matters: whatever shape the context grows into, these
// strings must not appear in what gets serialised into a prompt.
test('never carries a full MAC, an rssi or a device id', () => {
  const serialised = JSON.stringify(redactUnknownDevice(DEVICE))
  assert.ok(!serialised.includes('D0CF13E5BCCA'), 'full MAC leaked')
  assert.ok(!serialised.includes('E5BCCA'), 'device-unique MAC half leaked')
  assert.ok(!serialised.includes('ble:'), 'device id leaked')
  assert.ok(!serialised.includes('-46'), 'rssi leaked')
  assert.ok(!/rssi/i.test(serialised), 'rssi field leaked')
})

test('plant context keeps names and capabilities, drops devices and readings', () => {
  const r = redactPlants(PLANTS)
  assert.deepEqual(r, [{
    id: 1,
    name: 'TestPlant',
    capabilities: [
      { name: 'soil.moisture', unit: '%' },
      { name: 'air.temperature', unit: '°C' },
    ],
  }])
  const serialised = JSON.stringify(r)
  assert.ok(!serialised.includes('80EACA892A0A'), 'device MAC leaked')
  assert.ok(!serialised.includes('ble:'), 'device id leaked')
  assert.ok(!serialised.includes('29.7'), 'current reading leaked')
  assert.ok(!serialised.includes('12'), 'current reading leaked')
})

test('a single sample reports no spacing rather than a fake one', () => {
  const r = redactUnknownDevice({ ...DEVICE, samples: [DEVICE.samples[0]] })
  assert.equal(r.sampleCount, 1)
  assert.equal(r.spacingSeconds, null)
})

test('a malformed device id yields no OUI rather than a wrong one', () => {
  const r = redactUnknownDevice({ ...DEVICE, id: 'ble:XY' })
  assert.equal(r.ouiHex, null)
})
