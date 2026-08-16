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
export function loadCaps() {
  if (!capsPromise) {
    capsPromise = fetch('/api/v1/capabilities')
      .then((r) => r.json())
      .then((d) => {
        const map = new Map()
        for (const c of d.capabilities || []) map.set(c.id, c)
        return map
      })
      .catch((err) => {
        capsPromise = null
        throw err
      })
  }
  return capsPromise
}

// "soil.moisture" -> "Soil moisture", "battery.level" -> "Battery level".
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

// Display string at the table's own precision/unit, e.g. fmtCap(caps, 1,
// 21.4) -> "21.4 °C". `value` of null/undefined (never reported / not
// bound) renders as an em dash, matching every other tab's existing "no
// data" convention (fmtAge's "never", etc.) rather than "null" or "NaN".
export function fmtCap(caps, id, value) {
  if (value == null || Number.isNaN(value)) return '–'
  const c = caps && caps.get(id)
  if (!c) return String(value)
  return `${value.toFixed(c.precision ?? 0)} ${c.unit}`
}
