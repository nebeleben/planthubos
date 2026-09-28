import { test } from 'node:test'
import assert from 'node:assert/strict'
import { buildProfilePrompt, parseAiProfile } from '../profileFromAi.js'

const capNames = new Set(['soil.moisture', 'air.humidity', 'air.temperature', 'light.illuminance'])

const inputs = {
  fingerprint: { manufacturer: '_TZE284_o9ofysmo', model: 'TS0601' },
  observed_dps: [{ dp_id: 3, type: 2, value: 55, age_s: 4 }, { dp_id: 5, type: 2, value: 231, age_s: 4 }],
  caps: [{ name: 'soil.moisture', unit: '%' }, { name: 'air.temperature', unit: '°C' }],
  active_caps: [],
}

test('buildProfilePrompt: mentions the fingerprint, the DPs and the allowed cap names', () => {
  const { system, user } = buildProfilePrompt(inputs)
  assert.match(system + user, /_TZE284_o9ofysmo/)
  assert.match(user, /dp_id/i)
  assert.match(user, /soil\.moisture/)
})

test('parseAiProfile: extracts a fenced JSON profile and keeps valid entries', () => {
  const text = 'Sure!\n```json\n{"entries":[{"kind":"dp","dp_id":3,"cap":"soil.moisture","scale":1,"offset":0}]}\n```'
  const r = parseAiProfile(text, capNames)
  assert.equal(r.ok, true)
  assert.equal(r.entries.length, 1)
  assert.equal(r.entries[0].cap, 'soil.moisture')
})

test('parseAiProfile: rejects a hallucinated cap name', () => {
  const text = '```json\n{"entries":[{"kind":"dp","dp_id":9,"cap":"made.up","scale":1,"offset":0}]}\n```'
  const r = parseAiProfile(text, capNames)
  assert.equal(r.ok, false)
  assert.match(r.error, /made\.up/)
})

test('parseAiProfile: malformed JSON fails cleanly', () => {
  const r = parseAiProfile('no json here at all', capNames)
  assert.equal(r.ok, false)
  assert.match(r.error, /json/i)
})

test('parseAiProfile: rejects a non-numeric scale', () => {
  const text = '```json\n{"entries":[{"kind":"dp","dp_id":3,"cap":"soil.moisture","scale":"lots","offset":0}]}\n```'
  const r = parseAiProfile(text, capNames)
  assert.equal(r.ok, false)
})
