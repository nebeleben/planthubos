// The one place that speaks to an AI provider (M4 spec section 3). Two
// request dialects, one return type: a string of model text. Callers deal
// in prompts and get back text; nobody else in the codebase knows what an
// Anthropic message looks like. AiError.kind is one of: 'no-key', 'cors',
// 'auth', 'rate-limit', 'http', 'shape', 'truncated', 'aborted'.
import { getAiSettings } from './settings.js'

const ANTHROPIC_VERSION = '2023-06-01'

// Raised from 2048 after the M4 hardware proof, where the very first real
// generation came back `truncated`. The armchair reasoning for 2048 was
// that a wrapper is well under 200 tokens, so it looked like ~10x headroom
// -- but that counts only the code. What a current model actually returns
// is reasoning and prose around the code block, and the cap applies to the
// whole response, not to the part we keep. Measured, not guessed: 2048 was
// not enough for a three-line wrapper from a six-byte payload.
//
// The `truncated` error kind is what made this legible instead of a
// mysterious parse failure on half a program, so an overrun at this larger
// figure still surfaces as itself rather than as bad model output.
const MAX_TOKENS = 8192

// `kind` is what the UI switches on to say something useful (spec section 6).
// A generic "request failed" would send a user to check their WiFi when the
// actual cause is an endpoint that does not allow browser calls.
export class AiError extends Error {
  constructor(kind, message, detail) {
    super(message)
    this.name = 'AiError'
    this.kind = kind
    this.detail = detail || ''
  }
}

function anthropicRequest(s, system, user) {
  return {
    url: `${s.endpoint}/v1/messages`,
    init: {
      method: 'POST',
      headers: {
        'content-type': 'application/json',
        'x-api-key': s.key,
        'anthropic-version': ANTHROPIC_VERSION,
        // Without this header the browser call is refused outright. It is
        // named "dangerous" because it puts a key in a page; that is the
        // deliberate trade this product makes to keep the hub offline.
        'anthropic-dangerous-direct-browser-access': 'true',
      },
      body: JSON.stringify({
        model: s.model,
        max_tokens: MAX_TOKENS,
        system,
        messages: [{ role: 'user', content: user }],
      }),
    },
  }
}

function openaiRequest(s, system, user) {
  return {
    url: `${s.endpoint}/v1/chat/completions`,
    init: {
      method: 'POST',
      headers: { 'content-type': 'application/json', Authorization: `Bearer ${s.key}` },
      body: JSON.stringify({
        model: s.model,
        max_tokens: MAX_TOKENS,
        messages: [
          { role: 'system', content: system },
          { role: 'user', content: user },
        ],
      }),
    },
  }
}

function extractText(s, data) {
  if (s.kind === 'anthropic') {
    const parts = Array.isArray(data?.content) ? data.content : null
    const text = parts?.filter((p) => p?.type === 'text').map((p) => p.text).join('')
    if (text) return text
  } else {
    const text = data?.choices?.[0]?.message?.content
    if (typeof text === 'string' && text) return text
  }
  return null
}

function isTruncated(s, data) {
  if (s.kind === 'anthropic') {
    return data?.stop_reason === 'max_tokens'
  } else {
    return data?.choices?.[0]?.finish_reason === 'length'
  }
}

export async function aiComplete({ system, user, signal, settings }) {
  const s = settings || getAiSettings()
  if (!s.key || !s.key.trim()) {
    throw new AiError('no-key', 'No API key configured.')
  }
  const { url, init } = s.kind === 'openai'
    ? openaiRequest(s, system, user)
    : anthropicRequest(s, system, user)

  let res
  try {
    res = await fetch(url, { ...init, signal })
  } catch (err) {
    if (err && err.name === 'AbortError') throw new AiError('aborted', 'Generation cancelled.')
    // A refused CORS preflight and a dead network are the same TypeError
    // here -- the browser deliberately withholds the difference. For a
    // user-supplied endpoint the preflight is overwhelmingly the likelier
    // cause, so name it and let the message carry the hedge.
    throw new AiError('cors',
      `Could not reach ${s.endpoint}. The endpoint may not allow requests from a browser (CORS), or may be unreachable.`,
      String(err && err.message ? err.message : err))
  }

  if (!res.ok) {
    const detail = await res.text().catch(() => '')
    if (res.status === 401 || res.status === 403) {
      throw new AiError('auth', 'The provider rejected the API key.', detail)
    }
    if (res.status === 429 || res.status === 529) {
      throw new AiError('rate-limit', 'The provider is rate-limiting or overloaded. Try again shortly.', detail)
    }
    throw new AiError('http', `The provider returned HTTP ${res.status}.`, detail)
  }

  const data = await res.json().catch(() => null)
  const text = extractText(s, data)
  // Checked before the shape test: a response cut off before emitting any
  // text (stop_reason/finish_reason says so, but there is no text part at
  // all) is truncation, not an unrecognised shape -- spec section 6's
  // truncation row exists precisely so this case reads as "hit the token
  // limit", not as "this app doesn't understand the provider".
  if (isTruncated(s, data)) {
    throw new AiError('truncated',
      'The response was cut off at the token limit. Narrow the prompt or raise max_tokens.',
      text || '')
  }
  if (!text) {
    throw new AiError('shape', 'The provider returned a response this app does not understand.',
                      JSON.stringify(data).slice(0, 500))
  }
  return text
}
