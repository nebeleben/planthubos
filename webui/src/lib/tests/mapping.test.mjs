import { test } from 'node:test'
import assert from 'node:assert/strict'
import { previewEntry, validateEntry, shapeMapping, provenanceLabel } from '../mapping.js'

const capNames = new Set(['soil.moisture', 'air.humidity', 'air.temperature', 'light.illuminance'])

test('previewEntry: dp value * scale + offset with the cap unit', () => {
  const p = previewEntry({ kind: 'dp', dp_id: 5, cap: 'air.temperature', scale: 0.1, offset: 0 }, { value: 256 }, { 'air.temperature': '°C' })
  assert.match(p.value, /25\.6/)
  assert.match(p.value, /°C/)
})

test('previewEntry: suppress entry describes the dropped cluster', () => {
  const p = previewEntry({ kind: 'suppress', source_cluster: '0x0405' }, null, {})
  assert.match(p.label, /0x0405/)
})

test('validateEntry: rejects an unknown cap name', () => {
  const r = validateEntry({ kind: 'dp', dp_id: 3, cap: 'bogus.cap', scale: 1, offset: 0 }, capNames)
  assert.equal(r.ok, false)
  assert.match(r.error, /cap/i)
})

test('validateEntry: rejects a non-numeric scale', () => {
  const r = validateEntry({ kind: 'dp', dp_id: 3, cap: 'soil.moisture', scale: 'x', offset: 0 }, capNames)
  assert.equal(r.ok, false)
})

test('validateEntry: accepts a good dp entry and a good suppress entry', () => {
  assert.equal(validateEntry({ kind: 'dp', dp_id: 3, cap: 'soil.moisture', scale: 1, offset: 0 }, capNames).ok, true)
  assert.equal(validateEntry({ kind: 'suppress', source_cluster: '0x0405' }, capNames).ok, true)
})

test('shapeMapping: splits the API response into sections and keeps provenance', () => {
  const s = shapeMapping({
    applied: [{ dp_id: 3, cap: 'soil.moisture', scale: 1, offset: 0, provenance: 'profile' }],
    suppress: [{ cluster: 1029, provenance: 'ai' }],
    proposals: [{ kind: 'dp', dp_id: 5, cap: 'air.temperature', scale: 0.1, offset: 0 }],
    observed_unmapped: [{ dp_id: 101, type: 2, value: 44, age_s: 3 }],
  })
  assert.equal(s.applied.length, 1)
  assert.equal(s.applied[0].provenance, 'profile')
  assert.equal(s.proposals.length, 1)
  assert.equal(s.observed.length, 1)
  assert.equal(s.suppress[0].cluster, 1029)
  assert.equal(s.suppress[0].provenance, 'ai')
})

test('provenanceLabel: maps the provenance string to a UI tag', () => {
  assert.equal(provenanceLabel('profile'), 'via profile')
  assert.equal(provenanceLabel('ai'), 'via AI')
  assert.equal(provenanceLabel('manual'), 'manual')
  assert.equal(provenanceLabel(undefined), 'manual')
})
