import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  previewEntry, validateEntry, shapeMapping, provenanceLabel, clusterHex, proposalKey, proposalDraft, toWireEntry,
} from '../mapping.js'

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

// Fix round 1 (devices.jsx proposals-list bug): a "dp" and a "suppress"
// proposal must never collide on the same identity even if their own
// numeric field happens to match -- proposalKey() is what a proposals
// list is now keyed by (instead of array index) so a reject/confirm that
// removes an earlier row can never hand the row that shifts up into its
// slot a stale draft.
test('proposalKey: distinct identities per kind, stable regardless of numeric overlap', () => {
  assert.equal(proposalKey({ kind: 'dp', dp_id: 5, cap: 'soil.moisture', scale: 1, offset: 0 }), 'dp:5')
  assert.equal(proposalKey({ kind: 'suppress', cluster: 5 }), 'suppress:5')
  assert.notEqual(
    proposalKey({ kind: 'dp', dp_id: 5, cap: 'soil.moisture', scale: 1, offset: 0 }),
    proposalKey({ kind: 'suppress', cluster: 5 }),
  )
})

// Pins the actual regression at the pure-helper layer: proposalDraft (what
// a proposal row's local `draft` state is seeded from) and toWireEntry
// (what Confirm actually POSTs) are pure functions of the ONE entry passed
// in -- never of its position in an array -- so a row that keys off
// proposalKey() and re-derives from these can't end up showing/sending
// another entry's data after an earlier row is removed and the rest shift
// up. (Component-level coverage gap: devices.jsx has no jsx test harness
// in this repo, so the actual Preact reconciliation --  confirming a
// mis-keyed row would reuse component state across the shift -- is not
// exercised by node:test; this test only pins that the shape-layer
// functions a correctly-keyed row depends on stay entry-identity-correct.)
test('proposalDraft/toWireEntry: correspond to the entry itself, not array position, across a list shift', () => {
  const proposals = [
    { kind: 'dp', dp_id: 1, cap: 'soil.moisture', scale: 1, offset: 0 },
    { kind: 'dp', dp_id: 5, cap: 'air.temperature', scale: 0.1, offset: 0 },
    { kind: 'suppress', cluster: 1029 },
  ]

  // Reject the FIRST (non-last) proposal by identity -- the same
  // identity-filter devices.jsx's rejectProposal() now uses -- so the
  // remaining entries shift up one slot, same as a real reject/confirm.
  const remaining = proposals.filter((p) => proposalKey(p) !== proposalKey(proposals[0]))
  assert.equal(remaining.length, 2)

  // The row now at index 0 must still be proposal #2 (dp_id 5), not a
  // leftover of the rejected dp_id-1 row.
  const row0 = remaining[0]
  assert.equal(proposalKey(row0), 'dp:5')
  const draft0 = proposalDraft(row0)
  assert.equal(draft0.dp_id, 5)
  assert.equal(draft0.cap, 'air.temperature')
  assert.equal(toWireEntry(draft0, 'profile').dp_id, 5)
  assert.equal(toWireEntry(draft0, 'profile').cap, 'air.temperature')

  // The row now at index 1 must still be the suppress proposal, with its
  // cluster surviving the source_cluster/cluster field-name round trip.
  const row1 = remaining[1]
  assert.equal(proposalKey(row1), 'suppress:1029')
  const draft1 = proposalDraft(row1)
  assert.equal(draft1.source_cluster, clusterHex(1029))
  assert.equal(toWireEntry(draft1, 'profile').cluster, clusterHex(1029))
})
