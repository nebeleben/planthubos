// Capability table shared with the UI (spec section 2 "Capability ids (M1)").
// name -> { id, unit, aliases }. `aliases` lists every unit suffix the
// compiler accepts in source for this capability (spec section 1: "C-like
// precedence... literals ... must match the referenced capability's unit").
export const CAPS = {
  'soil.moisture': { id: 0, unit: '%', aliases: ['%'] },
  'air.temperature': { id: 1, unit: '°C', aliases: ['°C', 'C'] },
  'light.illuminance': { id: 2, unit: 'lux', aliases: ['lux'] },
  'soil.conductivity': { id: 3, unit: 'uS', aliases: ['uS'] },
  'battery.level': { id: 4, unit: '%', aliases: ['%'] },
}

export const CAPS_BY_ID = Object.fromEntries(
  Object.entries(CAPS).map(([name, c]) => [c.id, { name, ...c }])
)

export function lookupCap(name) {
  return CAPS[name] || null
}
