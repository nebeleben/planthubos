// Assemble the profile prompt from the hub's prompt-inputs and validate the
// model's returned profile JSON before it is ever previewed (spec §5). All
// failure modes fall back to manual mapping; nothing auto-applies.
import { profilePrompt } from './prompts/profile.js'
import { extractSource } from './extract.js'
import { validateEntry } from '../mapping.js'

export function buildProfilePrompt(inputs) {
  return profilePrompt(inputs)
}

export function parseAiProfile(text, capNames) {
  // Prefer a fenced block; fall back to the first {...} in the text.
  let jsonText = extractSource(text)
  if (!jsonText) {
    const m = /\{[\s\S]*\}/.exec(String(text || ''))
    jsonText = m ? m[0] : null
  }
  if (!jsonText) return { ok: false, entries: [], error: 'no JSON found in the AI response' }
  let obj
  try { obj = JSON.parse(jsonText) } catch (e) { return { ok: false, entries: [], error: `invalid JSON: ${e.message}` } }
  const raw = Array.isArray(obj) ? obj : (Array.isArray(obj.entries) ? obj.entries : null)
  if (!raw) return { ok: false, entries: [], error: 'JSON has no entries array' }
  const entries = []
  for (const e of raw) {
    const v = validateEntry(e, capNames)
    if (!v.ok) return { ok: false, entries: [], error: v.error }
    entries.push(e)
  }
  if (entries.length === 0) return { ok: false, entries: [], error: 'no valid entries' }
  return { ok: true, entries, error: '' }
}
