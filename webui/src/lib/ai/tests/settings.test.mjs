import { test, beforeEach } from 'node:test'
import assert from 'node:assert/strict'
import { getAiSettings, setAiSettings, hasAiKey, AI_DEFAULTS } from '../settings.js'

// node has no localStorage; settings.js must work against this shim.
beforeEach(() => {
  const store = new Map()
  globalThis.localStorage = {
    getItem: (k) => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => store.set(k, String(v)),
    removeItem: (k) => store.delete(k),
  }
})

test('defaults when nothing is stored', () => {
  const s = getAiSettings()
  assert.equal(s.kind, 'anthropic')
  assert.equal(s.endpoint, 'https://api.anthropic.com')
  assert.equal(s.model, 'claude-sonnet-5')
  assert.equal(s.key, '')
  assert.equal(hasAiKey(), false)
})

test('round-trips a partial update without clobbering the rest', () => {
  setAiSettings({ key: 'sk-test' })
  assert.equal(getAiSettings().key, 'sk-test')
  assert.equal(getAiSettings().model, AI_DEFAULTS.model)
  setAiSettings({ model: 'claude-opus-5' })
  assert.equal(getAiSettings().model, 'claude-opus-5')
  assert.equal(getAiSettings().key, 'sk-test', 'updating model must not drop the key')
  assert.equal(hasAiKey(), true)
})

test('an endpoint with a trailing slash is normalised', () => {
  setAiSettings({ endpoint: 'http://localhost:11434/' })
  assert.equal(getAiSettings().endpoint, 'http://localhost:11434')
})

test('a blank key reads as absent', () => {
  setAiSettings({ key: '   ' })
  assert.equal(hasAiKey(), false)
})

test('null key clears the key and reverts to default', () => {
  setAiSettings({ key: 'sk-test' })
  assert.equal(hasAiKey(), true)
  setAiSettings({ key: null })
  assert.equal(hasAiKey(), false)
  assert.equal(getAiSettings().key, '')
})

test('undefined key clears the key and reverts to default', () => {
  setAiSettings({ key: 'sk-test' })
  assert.equal(hasAiKey(), true)
  setAiSettings({ key: undefined })
  assert.equal(hasAiKey(), false)
  assert.equal(getAiSettings().key, '')
})

test('empty string key is stored and does not clear (regression)', () => {
  setAiSettings({ key: 'sk-test' })
  setAiSettings({ key: '' })
  assert.equal(getAiSettings().key, '')
  assert.equal(hasAiKey(), false)
})

test('clearing one field does not disturb another', () => {
  setAiSettings({ key: 'sk-test', model: 'claude-opus-5' })
  setAiSettings({ key: null })
  assert.equal(hasAiKey(), false)
  assert.equal(getAiSettings().model, 'claude-opus-5', 'model must survive key removal')
})
