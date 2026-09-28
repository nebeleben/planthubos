// Pure shaping/validation/preview for the devices-tab Mapping section
// (device-mapping-profiles §4/§6). No DOM, no fetch -- unit-tested with
// node:test. The preview is the safety gate: every proposed entry is shown
// evaluated against the latest live sample before it is confirmed.

export function previewEntry(entry, sample, unitByCap) {
  if (entry.kind === 'suppress') {
    return { label: `Suppress cluster ${entry.source_cluster}`, value: 'dropped' }
  }
  const scale = Number(entry.scale)
  const offset = Number(entry.offset || 0)
  const unit = (unitByCap && unitByCap[entry.cap]) || ''
  if (!sample || sample.value == null || !Number.isFinite(scale)) {
    return { label: `DP ${entry.dp_id} → ${entry.cap}`, value: 'no sample yet' }
  }
  const out = Number(sample.value) * scale + offset
  const shown = Number.isInteger(out) ? String(out) : out.toFixed(2)
  return { label: `DP ${entry.dp_id} = ${sample.value} → ${entry.cap}`, value: `${shown} ${unit}`.trim() }
}

export function validateEntry(entry, capNames) {
  if (entry.kind === 'suppress') {
    const c = String(entry.source_cluster || '')
    if (!/^0x[0-9a-fA-F]+$|^\d+$/.test(c)) return { ok: false, error: 'bad source_cluster' }
    return { ok: true, error: '' }
  }
  if (entry.kind === 'dp') {
    if (!capNames.has(entry.cap)) return { ok: false, error: `unknown cap "${entry.cap}"` }
    if (!Number.isFinite(Number(entry.scale))) return { ok: false, error: 'scale is not a number' }
    if (entry.offset != null && !Number.isFinite(Number(entry.offset))) return { ok: false, error: 'offset is not a number' }
    if (!Number.isInteger(Number(entry.dp_id))) return { ok: false, error: 'bad dp_id' }
    return { ok: true, error: '' }
  }
  return { ok: false, error: `unknown kind "${entry.kind}"` }
}

export function shapeMapping(resp) {
  return {
    applied: Array.isArray(resp.applied) ? resp.applied : [],
    suppress: Array.isArray(resp.suppress) ? resp.suppress : [],
    proposals: Array.isArray(resp.proposals) ? resp.proposals : [],
    observed: Array.isArray(resp.observed_unmapped) ? resp.observed_unmapped : [],
  }
}

// Turns the API's provenance string into a small UI tag so a reviewer can see
// where an applied mapping came from (spec §3 provenance).
export function provenanceLabel(provenance) {
  if (provenance === 'profile') return 'via profile'
  if (provenance === 'ai') return 'via AI'
  return 'manual'
}

// "0x0405" style hex string for a raw ZCL cluster id, matching
// dev_profiles_json.c's own established "0x"-prefixed-string convention
// for a cluster (the mapping wire format itself renders it as a plain
// NUMBER -- api_v1.c's mapping_entry_json() doc comment -- so this is
// purely a display choice, not a wire-format one).
export function clusterHex(cluster) {
  return `0x${Number(cluster).toString(16).padStart(4, '0')}`
}

// A STABLE identity for one proposal entry, independent of its position
// in whatever array currently holds it. Devices-tab fix round 1: a list
// rendered with an array-INDEX key lets Preact reuse a row's component
// instance (and its own local `draft` state) for whatever entry shifts
// into that slot after a reject/confirm shrinks the array -- the row then
// shows a stale preview for the wrong proposal, and Confirm would POST
// that stale draft. Every proposal/observed/applied row in this file is
// now keyed by this identity instead of its index, so Preact remounts (or
// correctly re-associates) by the actual entry, never by slot position.
export function proposalKey(entry) {
  return entry.kind === 'suppress' ? `suppress:${entry.cluster}` : `dp:${entry.dp_id}`
}

// Reshapes one GET .../mapping "proposals" entry (kind/dp_id/cap/scale/
// offset for "dp", kind/cluster for "suppress" -- api_v1.c's
// mapping_entry_json()) into the draft shape previewEntry/validateEntry
// above expect back (a suppress draft's cluster field is `source_cluster`,
// not the wire's `cluster`). A pure, single-argument function of the one
// proposal passed in -- never touches array position -- so a caller that
// re-derives a row's draft from THIS on every relevant re-render (keyed by
// proposalKey(), not index) can never end up with another entry's data.
export function proposalDraft(p) {
  if (p.kind === 'suppress') return { kind: 'suppress', source_cluster: clusterHex(p.cluster) }
  return { kind: 'dp', dp_id: p.dp_id, cap: p.cap, scale: p.scale, offset: p.offset }
}

// Inverse-ish of proposalDraft, for POSTing a (possibly hand-edited) draft
// back to /api/v1/devices/{id}/mapping: numeric dp_id/scale/offset, and
// `cluster` (not `source_cluster` -- devices_mapping_post() reads
// "cluster", the same field name GET's own proposals already use).
// `cluster` is sent as whatever string was typed/derived rather than
// pre-parsed to a number -- the hub already accepts either a JSON number
// or a "0x..."/decimal string for that field (its own parser leniency).
export function toWireEntry(entry, provenance) {
  if (entry.kind === 'suppress') {
    return { kind: 'suppress', cluster: entry.source_cluster, provenance }
  }
  return {
    kind: 'dp',
    dp_id: Number(entry.dp_id),
    cap: entry.cap,
    scale: Number(entry.scale),
    offset: Number(entry.offset || 0),
    provenance,
  }
}
