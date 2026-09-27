import { test } from 'node:test'
import assert from 'node:assert/strict'
import { multiEndpointNames, epSuffixed } from '../endpoints.js'

// Task 11 (multi-endpoint Zigbee): two switch.state caps on endpoints {1,2}
// -- the dual-valve/multi-gang case the brief calls out -- must both be
// flagged as repeating.
test('multiEndpointNames: a name on two distinct endpoints is flagged', () => {
  const caps = [
    { name: 'switch.state', endpoint: 1 },
    { name: 'switch.state', endpoint: 2 },
  ]
  assert.deepEqual([...multiEndpointNames(caps)], ['switch.state'])
})

// A single-endpoint device's card must render with NO endpoint label at all
// (byte-identical to the pre-Task-9 UI) -- so a name that appears only once,
// even mixed in with a repeating one, is never flagged.
test('multiEndpointNames: a name on a single endpoint is not flagged, even alongside a repeating name', () => {
  const items = [
    { name: 'switch.state', endpoint: 1 },
    { name: 'switch.state', endpoint: 2 },
    { name: 'battery.level', endpoint: 1 },
  ]
  const repeated = multiEndpointNames(items)
  assert.equal(repeated.has('battery.level'), false)
  assert.equal(repeated.has('switch.state'), true)
})

// The same (name, endpoint) pair repeated -- not two DIFFERENT endpoints --
// is one endpoint, not two, so it must not be flagged either.
test('multiEndpointNames: the same endpoint repeated for a name is not flagged', () => {
  const items = [
    { name: 'battery.level', endpoint: 1 },
    { name: 'battery.level', endpoint: 1 },
  ]
  assert.equal(multiEndpointNames(items).size, 0)
})

test('multiEndpointNames: empty input flags nothing', () => {
  assert.equal(multiEndpointNames([]).size, 0)
})

test('epSuffixed: showEp true appends "· ep N"', () => {
  assert.equal(epSuffixed('Switch', 2, true), 'Switch · ep 2')
})

test('epSuffixed: showEp false returns the label unchanged (single-endpoint device)', () => {
  assert.equal(epSuffixed('Switch', 1, false), 'Switch')
})
