// Capability metadata (GET /api/v1/capabilities), shared by every tab that
// renders a capability value: the build-time table (spec §1, `components/
// capability/capability.c`) is the single source of truth for a
// capability's display name, unit and decimal precision, and every tab
// needs the same {id -> metadata} lookup. Module-level promise cache (task
// brief step 1) so mounting Dashboard, Devices, Plants and History back to
// back -- normal tab-switching -- costs one fetch total, not one per tab.
//
// Deliberately separate from ../lib/psc/caps.js: that file is M1's
// PlantScript compiler's own frozen, hand-written ids-0-4 table (compiler
// literal/unit checking, source-derived) -- a different concern with a
// similar name purely by coincidence. This file is the live, server-served
// M2 capability table (all 8 ids, id 5-7 included) and must never be
// merged with or read by the compiler.
let capsPromise = null

// Returns a Map<id, {id, name, unit, precision, ha_device_class}>, fetched
// once and cached for the lifetime of the page. A failed fetch clears the
// cache so the next caller (e.g. a tab mounted after the hub came back)
// retries instead of being stuck replaying the same rejection forever.
// M5 fixwave: a non-2xx response (`r.ok` false) and a 200 whose body lacks
// a usable `capabilities` array both used to resolve to an empty-but-truthy
// Map -- silently passing every tab's `!caps` guard and getting cached for
// the page's whole lifetime, so e.g. plants.jsx's per-capability bind rows
// would just disappear with no way to recover short of a reload. Both cases
// now reject instead, which (via the catch below) self-nulls the cache so
// the next caller retries rather than replaying the same bad result.
export function loadCaps() {
  if (!capsPromise) {
    capsPromise = fetch('/api/v1/capabilities')
      .then((r) => {
        if (!r.ok) throw new Error(`GET /api/v1/capabilities: ${r.status}`)
        return r.json()
      })
      .then((d) => {
        const map = new Map()
        for (const c of d.capabilities || []) map.set(c.id, c)
        if (map.size === 0) throw new Error('GET /api/v1/capabilities: empty capability table')
        return map
      })
      .catch((err) => {
        capsPromise = null
        throw err
      })
  }
  return capsPromise
}

// "soil.moisture" -> "Soil Moisture", "battery.level" -> "Battery Level"
// (every dot-segment capitalised, not just the first).
// Every id in the frozen table (spec §1) is a dotted `domain.metric` name;
// this is the one humanisation rule every tab needs (a plain unit label
// like "%" is ambiguous -- soil.moisture, battery.level and air.humidity
// all use it, spec §1's table) so it lives here rather than being
// reinvented per tab. Falls back to a bare "cap <id>" for an id the
// currently-loaded table doesn't (yet) know about, rather than throwing.
export function capLabel(caps, id) {
  const c = caps && caps.get(id)
  if (!c || !c.name) return `cap ${id}`
  const words = c.name.split('.')
  return words.map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(' ')
}

// Value/unit split shared by fmtCap() below and dashboard.jsx's Reading()
// (Triage item 5 fixwave). Before this, each had its own copy of this same
// logic, and they'd drifted: fmtCap fell back to the raw `String(value)` for
// a capability id the loaded table has no metadata for, while Reading()'s
// copy fell back to `value.toFixed(0)` with the unit dropped entirely --
// exactly the id-missing-from-caps case M5's caps-loading fix (this file's
// loadCaps()) can still legitimately produce transiently (a tab rendered
// before the fetch resolves) or permanently (an id newer than an older
// cached table). The two fallbacks disagreed on both rounding AND unit, so
// the SAME reading could show "21.4" on one tab and "21" (silently wrong,
// not obviously incomplete) on Dashboard. Both callers now go through this
// one function so they can't diverge again.
// `value` null/undefined/NaN (never reported / not bound) -> '–' text, no
// unit, matching every other tab's existing "no data" convention (fmtAge's
// "never", etc.).
export function fmtCapParts(caps, id, value) {
  if (value == null || Number.isNaN(value)) return { text: '–', unit: '' }
  const c = caps && caps.get(id)
  if (!c) return { text: String(value), unit: '' }
  return { text: value.toFixed(c.precision ?? 0), unit: c.unit }
}

// Display string at the table's own precision/unit, e.g. fmtCap(caps, 1,
// 21.4) -> "21.4 °C". `value` of null/undefined (never reported / not
// bound) renders as an em dash, matching every other tab's existing "no
// data" convention (fmtAge's "never", etc.) rather than "null" or "NaN".
export function fmtCap(caps, id, value) {
  const { text, unit } = fmtCapParts(caps, id, value)
  return unit ? `${text} ${unit}` : text
}
