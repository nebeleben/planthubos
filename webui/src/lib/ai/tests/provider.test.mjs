import { test } from 'node:test'
import assert from 'node:assert/strict'
import { aiComplete, AiError } from '../provider.js'

const ANTHROPIC = { kind: 'anthropic', endpoint: 'https://api.anthropic.com',
                    model: 'claude-sonnet-5', key: 'sk-test' }
const OPENAI = { kind: 'openai', endpoint: 'http://localhost:11434',
                 model: 'llama3', key: 'k' }

function stubFetch(handler) {
  globalThis.fetch = async (url, init) => handler(url, init)
}

test('anthropic request shape carries the browser-access header', async () => {
  let seen = null
  stubFetch((url, init) => {
    seen = { url, init }
    return { ok: true, status: 200,
             json: async () => ({ content: [{ type: 'text', text: 'hello' }] }) }
  })
  const out = await aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC })
  assert.equal(out, 'hello')
  assert.equal(seen.url, 'https://api.anthropic.com/v1/messages')
  assert.equal(seen.init.headers['x-api-key'], 'sk-test')
  assert.equal(seen.init.headers['anthropic-dangerous-direct-browser-access'], 'true')
  assert.ok(seen.init.headers['anthropic-version'])
  const body = JSON.parse(seen.init.body)
  assert.equal(body.model, 'claude-sonnet-5')
  assert.equal(body.system, 'S')
  assert.deepEqual(body.messages, [{ role: 'user', content: 'U' }])
  assert.ok(!('key' in body), 'the key must travel as a header, never in the body')
})

test('openai request shape', async () => {
  let seen = null
  stubFetch((url, init) => {
    seen = { url, init }
    return { ok: true, status: 200,
             json: async () => ({ choices: [{ message: { content: 'hi' } }] }) }
  })
  const out = await aiComplete({ system: 'S', user: 'U', settings: OPENAI })
  assert.equal(out, 'hi')
  assert.equal(seen.url, 'http://localhost:11434/v1/chat/completions')
  assert.equal(seen.init.headers['Authorization'], 'Bearer k')
  const body = JSON.parse(seen.init.body)
  assert.deepEqual(body.messages, [{ role: 'system', content: 'S' },
                                   { role: 'user', content: 'U' }])
})

test('missing key fails before any request is made', async () => {
  let called = false
  stubFetch(() => { called = true; return { ok: true, status: 200, json: async () => ({}) } })
  await assert.rejects(
    () => aiComplete({ system: 'S', user: 'U', settings: { ...ANTHROPIC, key: '' } }),
    (e) => e instanceof AiError && e.kind === 'no-key')
  assert.equal(called, false, 'must not call the provider without a key')
})

test('a thrown fetch is reported as a cors/network refusal, not a generic failure', async () => {
  stubFetch(() => { throw new TypeError('Failed to fetch') })
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: OPENAI }),
                       (e) => e instanceof AiError && e.kind === 'cors')
})

test('401 and 429 are distinguished', async () => {
  stubFetch(() => ({ ok: false, status: 401, text: async () => 'nope' }))
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC }),
                       (e) => e.kind === 'auth')
  stubFetch(() => ({ ok: false, status: 429, text: async () => 'slow down' }))
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC }),
                       (e) => e.kind === 'rate-limit')
})

test('an unrecognised response shape is its own error kind', async () => {
  stubFetch(() => ({ ok: true, status: 200, json: async () => ({ unexpected: true }) }))
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC }),
                       (e) => e.kind === 'shape')
})

test('abort is reported as aborted, not as a network failure', async () => {
  stubFetch(() => { const e = new Error('aborted'); e.name = 'AbortError'; throw e })
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC }),
                       (e) => e.kind === 'aborted')
})

test('anthropic response truncated at max_tokens is reported as truncated', async () => {
  stubFetch(() => ({ ok: true, status: 200,
             json: async () => ({ stop_reason: 'max_tokens',
                                  content: [{ type: 'text', text: 'partial' }] }) }))
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC }),
                       (e) => e instanceof AiError && e.kind === 'truncated' && e.detail === 'partial')
})

test('openai response truncated at length is reported as truncated', async () => {
  stubFetch(() => ({ ok: true, status: 200,
             json: async () => ({ choices: [{ finish_reason: 'length',
                                              message: { content: 'partial' } }] }) }))
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: OPENAI }),
                       (e) => e instanceof AiError && e.kind === 'truncated' && e.detail === 'partial')
})

// M4 fix wave finding 5: a response cut off before emitting any text
// (stop_reason says max_tokens, but there's no text part at all -- e.g.
// the model was still inside a tool-call-shaped or empty turn when the
// limit hit) must report as 'truncated', not 'shape'. Shape is for a
// response this app genuinely doesn't recognise; this is exactly the
// truncation case spec section 6 exists to name correctly.
test('a truncated response with no text at all is reported as truncated, not shape', async () => {
  stubFetch(() => ({ ok: true, status: 200,
             json: async () => ({ stop_reason: 'max_tokens', content: [] }) }))
  await assert.rejects(() => aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC }),
                       (e) => e instanceof AiError && e.kind === 'truncated' && e.detail === '')
})

test('anthropic response with normal stop_reason resolves to text', async () => {
  stubFetch(() => ({ ok: true, status: 200,
             json: async () => ({ stop_reason: 'end_turn',
                                  content: [{ type: 'text', text: 'complete' }] }) }))
  const out = await aiComplete({ system: 'S', user: 'U', settings: ANTHROPIC })
  assert.equal(out, 'complete')
})

test('openai response with normal finish_reason resolves to text', async () => {
  stubFetch(() => ({ ok: true, status: 200,
             json: async () => ({ choices: [{ finish_reason: 'stop',
                                              message: { content: 'complete' } }] }) }))
  const out = await aiComplete({ system: 'S', user: 'U', settings: OPENAI })
  assert.equal(out, 'complete')
})
