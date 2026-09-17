// Built-in wrapper library: a curated catalog of ready-made PlantScript
// wrappers for well-documented open BLE sensor formats. Shipped as SOURCE
// (not pre-compiled bytecode) — the Wrappers tab's "Install" compiles each
// with the same psc compiler the editor uses, then POSTs it like any manual
// wrapper, so an installed entry is a normal, editable wrapper.
//
// Only formats whose byte layout is publicly documented and expressible in
// the wrapper dialect (src/lib/ai/prompts/dialect.js — arithmetic + `/`,
// `>>`, `bits()`, but NO modulo) live here. MiFlora/BTHome are decoded
// natively in firmware and their UUIDs are reserved against wrappers, so
// they are intentionally absent. Govee H5075 is absent because its packed
// temp+humidity field needs modulo, which the dialect lacks.

export const WRAPPER_LIBRARY = [
  {
    name: 'Ruuvi RAWv2',
    description:
      'Ruuvi environmental tags broadcasting data format 5 (RAWv2, manufacturer 0x0499): air temperature, humidity, and pressure.',
    source: `wrapper "Ruuvi RAWv2" match manufacturer 0x0499
decode
  require u8(payload, 0) == 5
  emit air.temperature i16_be(payload, 1) * 0.005
  emit air.humidity u16_be(payload, 3) * 0.0025
  emit air.pressure u16_be(payload, 5) / 100 + 500`,
  },
  {
    name: 'Xiaomi Thermometer (ATC/pvvx)',
    description:
      'Xiaomi LYWSD03MMC thermometers running the ATC/pvvx custom firmware in "custom" advertising mode (service 0x181A, 15-byte payload): temperature, humidity, and battery level.',
    source: `wrapper "Xiaomi Thermometer (ATC/pvvx)" match service 0x181A
decode
  require len(payload) == 15
  emit air.temperature i16_le(payload, 6) * 0.01
  emit air.humidity u16_le(payload, 8) * 0.01
  emit battery.level u8(payload, 12)`,
  },
]
