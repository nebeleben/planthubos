// AI provider configuration (M4 spec section 3). Lives in localStorage and
// NOWHERE else: the key is never sent to the hub, never written to flash,
// never logged and never put in a URL. The hub is a cloudless device -- the
// browser is what talks to the provider, so the browser is what holds the
// credential.
//
// This module is the only one that touches storage; every other module
// takes settings as an argument, which keeps the storage shape one file's
// problem and makes provider.js testable without a DOM.

const K = {
  kind: 'ph.ai.kind',
  endpoint: 'ph.ai.endpoint',
  model: 'ph.ai.model',
  key: 'ph.ai.key',
}

export const AI_DEFAULTS = {
  kind: 'anthropic',
  endpoint: 'https://api.anthropic.com',
  model: 'claude-sonnet-5',
  key: '',
}

// Trailing slashes are stripped here rather than at every call site: the
// request builders concatenate a path onto this, and "…:11434/" + "/v1/…"
// is a 404 that looks like a wrong-endpoint error. Empty string and only-slashes
// endpoints silently revert to the default on read (via the g() fallback).
function normEndpoint(v) {
  return String(v || '').trim().replace(/\/+$/, '')
}

export function getAiSettings() {
  const g = (k, d) => {
    const v = localStorage.getItem(k)
    return v === null || v === '' ? d : v
  }
  return {
    kind: g(K.kind, AI_DEFAULTS.kind),
    endpoint: normEndpoint(g(K.endpoint, AI_DEFAULTS.endpoint)),
    model: g(K.model, AI_DEFAULTS.model),
    key: g(K.key, AI_DEFAULTS.key),
  }
}

export function setAiSettings(partial) {
  for (const [field, storageKey] of Object.entries(K)) {
    if (!(field in partial)) continue
    const rawValue = partial[field]

    // Treat null/undefined as "remove this setting" to prevent silent corruption
    // from stringifying them into literal "null"/"undefined" strings (which would
    // then appear present and block further generation).
    if (rawValue === null || rawValue === undefined) {
      localStorage.removeItem(storageKey)
      continue
    }

    const v = field === 'endpoint' ? normEndpoint(rawValue) : String(rawValue)
    localStorage.setItem(storageKey, v)
  }
}

// A key of whitespace is a key the user has cleared, not a key they have.
export function hasAiKey() {
  return getAiSettings().key.trim().length > 0
}
