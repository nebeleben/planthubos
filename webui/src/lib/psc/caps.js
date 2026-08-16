// Capability table shared with the UI (spec section 2 "Capability ids (M1)").
// name -> { id, unit, aliases }. `aliases` lists every unit suffix the
// compiler accepts in source for this capability (spec section 1: "C-like
// precedence... literals ... must match the referenced capability's unit").
//
// Ids 0-4 are frozen from M1. Ids 5-7 (air.humidity/air.pressure/
// signal.rssi) were added to components/capability/include/capability.h by
// M2 but never mirrored here -- the wrapper dialect (M3) is the first
// PlantScript dialect that can actually reach them (a Ruuvi wrapper emits
// air.humidity, spec §3's own example), so they're added now. Unit aliases
// use ASCII stand-ins for capability.c's UTF-8 unit strings (µS/cm, hPa,
// dBm) the same way soil.conductivity's 'uS' already does -- the lexer's
// UNIT_CHAR class ([A-Za-z%°]) cannot lex 'µ' or '/'.
export const CAPS = {
  'soil.moisture': { id: 0, unit: '%', aliases: ['%'] },
  'air.temperature': { id: 1, unit: '°C', aliases: ['°C', 'C'] },
  'light.illuminance': { id: 2, unit: 'lux', aliases: ['lux'] },
  'soil.conductivity': { id: 3, unit: 'uS', aliases: ['uS'] },
  'battery.level': { id: 4, unit: '%', aliases: ['%'] },
  'air.humidity': { id: 5, unit: '%', aliases: ['%'] },
  'air.pressure': { id: 6, unit: 'hPa', aliases: ['hPa'] },
  'signal.rssi': { id: 7, unit: 'dBm', aliases: ['dBm'] },
}

export const CAPS_BY_ID = Object.fromEntries(
  Object.entries(CAPS).map(([name, c]) => [c.id, { name, ...c }])
)

export function lookupCap(name) {
  return CAPS[name] || null
}
