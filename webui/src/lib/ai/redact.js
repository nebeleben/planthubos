// The only module that decides what leaves the LAN (M4 spec section 4).
//
// PlantHub's premise is a hub that never talks to the internet. The AI
// flow deliberately breaks that at config time, from the browser -- so
// what it sends is a product decision, not an implementation detail, and
// it is enforced in one file with a property test rather than spread
// across call sites and trusted to review.
//
// Sent for a wrapper: payload hex, the vendor OUI, how many samples and
// how far apart. Sent for a rule: plant names with their bound capability
// names and units. Never sent: the full MAC, RSSI, device ids, current
// readings, hub name, WiFi details, claim state.
import { CAPS } from '../psc/caps.js'

// "ble:D0CF13E5BCCA" -> "D0CF13". The OUI earns its place: it often names
// the manufacturer outright, which is what lets a model recognise a Ruuvi
// frame instead of inferring a vendor from byte structure and confidently
// decoding the wrong device. The device-unique half stays home.
function ouiOf(deviceId) {
  const m = /^[a-z]+:([0-9A-Fa-f]{12})$/.exec(String(deviceId || ''))
  return m ? m[1].slice(0, 6).toUpperCase() : null
}

export function redactUnknownDevice(device) {
  const samples = Array.isArray(device?.samples) ? device.samples : []
  let spacingSeconds = null
  if (samples.length >= 2) {
    const a = Number(samples[0].ts), b = Number(samples[samples.length - 1].ts)
    if (Number.isFinite(a) && Number.isFinite(b)) spacingSeconds = Math.abs(b - a)
  }
  return {
    ouiHex: ouiOf(device?.id),
    sampleCount: samples.length,
    spacingSeconds,
    // Rebuilt field by field rather than spread: a spread would silently
    // carry any new field the endpoint grows into the prompt.
    samples: samples.map((s) => ({ hex: String(s.hex), len: Number(s.len) })),
  }
}

export function redactPlants(plants) {
  return (Array.isArray(plants) ? plants : []).map((p) => ({
    id: Number(p.id),
    name: String(p.name || ''),
    capabilities: (Array.isArray(p.bindings) ? p.bindings : []).map((b) => ({
      name: String(b.name),
      unit: CAPS[b.name] ? CAPS[b.name].unit : '',
    })),
  }))
}
