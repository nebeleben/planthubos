// Best-effort vendor labels for unknown BLE devices on the Devices tab.
//
// The full IEEE OUI / BLE-SIG company-id registries are megabytes and can't
// ship on a cloudless hub, so these are small CURATED maps of makers common
// to this project's world (BLE sensors + the phones that flood the scan).
// Extend them freely -- an unmapped id/OUI is not an error, it just falls
// back to showing the raw value so it stays useful.

// BLE SIG 16-bit "Company Identifier" (manufacturer-data first two bytes,
// little-endian) -> vendor. The hub reports this as unknown-device company_id.
export const COMPANY_IDS = {
  0x0499: 'Ruuvi Innovations',
  0x038f: 'Xiaomi',
  0x0157: 'Anhui Huami (Xiaomi)',
  0x004c: 'Apple',
  0x0006: 'Microsoft',
  0x0075: 'Samsung',
  0x00e0: 'Google',
  0x0059: 'Nordic Semiconductor',
  0x02e5: 'Espressif',
}

// MAC OUI (first three bytes, uppercase hex, no separators) -> vendor.
// Only entries we're confident about; used as a fallback when a device
// advertises no manufacturer company id.
export const OUIS = {
  '2CCF67': 'Espressif',
  '84F703': 'Espressif',
}

function hex2(n) {
  return '0x' + Number(n).toString(16).padStart(4, '0')
}

// Resolve a display vendor for an unknown device object from /api/v1/unknown
// ({ id: "ble:AABBCC...", company_id }). Prefers the company id (a device
// property) over the OUI (a whole-vendor prefix). Returns a human string, or
// a raw-value hint when the id/OUI isn't in the curated maps, or null when
// there's nothing to say at all.
export function resolveVendor(dev) {
  const cid = dev && dev.company_id
  if (cid) {
    return COMPANY_IDS[cid] || `Company ${hex2(cid)}`
  }
  const id = (dev && dev.id) || ''
  const m = /^ble:([0-9A-Fa-f]{6})/.exec(id)
  if (m) {
    const oui = m[1].toUpperCase()
    return OUIS[oui] || `OUI ${oui}`
  }
  return null
}
