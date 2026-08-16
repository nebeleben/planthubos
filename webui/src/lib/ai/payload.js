// Bytes a wrapper actually receives are NOT the raw advert GET
// /api/v1/unknown hands back -- they are sliced first, per
// components/ble_collector/ble_collector.c's decode_adv_item() (the
// comment directly above its `switch (wrapper_index_kind_of(...))`, M3
// spec §3): a `service` match gets the bytes after that AD structure's own
// 2-byte UUID, a `manufacturer` match gets the bytes after its 2-byte
// company id, and `mac_prefix` has no header to skip and gets the whole
// captured advertisement unsliced. When the advert doesn't contain the
// structure a service/manufacturer wrapper matches on, the firmware hands
// the wrapper a NULL/zero-length payload -- NOT the raw advert -- so this
// mirrors that with an empty hex string, never a fallback to the whole
// thing. Get this wrong and a correct wrapper decodes the id bytes as if
// they were sensor data.
//
// LAST matching AD structure wins, not first: decode_adv_item() parses the
// whole advert exactly once via NimBLE's ble_hs_adv_parse_fields(), which
// unconditionally overwrites fields.mfg_data / fields.svc_data_uuid16 on
// every AD structure of that type it walks as it scans -- so on an advert
// carrying two same-type structures, the LAST one is both what
// wrapper_index_lookup() matched the wrapper on (svc_uuid/manu_id are read
// off those same overwritten fields) and what the wrapper's `payload`
// pointer actually decodes. decode_adv_item() + ble_hs_adv_parse_fields()
// remain the ground truth for the slicing rule itself. api_v1.c's
// wrapper_ad_find_slice() (renamed from wrapper_ad_find_u16() in the same
// fix wave) was changed to walk last-wins too, matching that ground truth,
// and is no longer advisory-only bookkeeping for a UI preview -- it is now
// the function that produces the actual dry-run sample GET
// /api/v1/wrappers/test returns.
//
// `matchKind` is always the NUMERIC form (0 service, 1 manufacturer,
// 2 mac_prefix) straight off a fresh compileWrapper() result -- GET
// /api/v1/wrappers's string form (wmatch_kind_str) must never be passed
// here; the two are a real, easy-to-conflate footgun (see wrappers.jsx's
// own MATCH_KEY_WIDTH comment).
const AD_TYPE_SERVICE_DATA_16 = 0x16
const AD_TYPE_MANUFACTURER = 0xff

function hexToBytes(hex) {
  const clean = String(hex || '').trim()
  const out = []
  for (let i = 0; i + 1 < clean.length; i += 2) out.push(parseInt(clean.slice(i, i + 2), 16))
  return out
}

function bytesToHex(bytes) {
  return bytes.map((b) => b.toString(16).padStart(2, '0')).join('')
}

// Walks every AD structure in a raw advertisement and returns the bytes
// after the LAST occurrence of `adType`'s own 2-byte id, or null if none
// exists at all. A LAST occurrence too short to even hold the 2-byte id
// still wins over an earlier, longer one -- it yields an empty array
// rather than null, matching NimBLE's unconditional overwrite (see the
// comment at the assignment below). A structure whose length byte
// overruns the remaining buffer ends the walk right there -- whatever was
// found before the truncation still stands, nothing past a truncated
// structure is trusted.
function lastAdStructureAfterId(bytes, adType) {
  let i = 0
  let found = null
  while (i < bytes.length) {
    const segLen = bytes[i]
    if (segLen === 0) break
    if (i + 1 + segLen > bytes.length) break
    const segType = bytes[i + 1]
    // Assign unconditionally on a type match, even when segLen < 3 (too
    // short to hold the 2-byte id) -- NimBLE's ble_hs_adv_parse_fields()
    // has no minimum-length check on manufacturer data, so a short
    // trailing manufacturer structure still overwrites fields.mfg_data
    // (to a near-empty slice) and must clobber an earlier match here too,
    // not leave it standing. The service-data branch never actually hits
    // the segLen < 3 case in practice: a service-data structure too short
    // to hold its own 2-byte UUID16 fails NimBLE's parse and gets the
    // whole advertisement rejected before it reaches /api/v1/unknown --
    // this function still treats both types the same way rather than
    // encoding that difference here.
    if (segType === adType) found = segLen >= 3 ? bytes.slice(i + 4, i + 1 + segLen) : []
    i += 1 + segLen
  }
  return found
}

// The payload-slice this module exists for -- named to match what it does.
// Called from exactly one place (wrappers.jsx), right before a captured
// sample is sent to /api/v1/wrappers/test.
export function sliceWrapperPayload(hex, matchKind) {
  if (matchKind === 2) return hex   // mac_prefix: whole raw advert, unsliced
  const adType = matchKind === 0 ? AD_TYPE_SERVICE_DATA_16 : AD_TYPE_MANUFACTURER
  const body = lastAdStructureAfterId(hexToBytes(hex), adType)
  return body ? bytesToHex(body) : ''   // structure absent -- empty, never the raw advert
}
