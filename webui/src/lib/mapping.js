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
