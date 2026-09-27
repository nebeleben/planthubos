// Multi-endpoint Zigbee (Task 11): Task 9's devices_json.c now emits an
// `endpoint` integer on EVERY capability and action object, and a cap/action
// `name` present on more than one endpoint (a dual valve, a multi-gang
// switch) appears as multiple objects -- one per endpoint -- rather than one
// merged row. devices.jsx's caps table and ActionsSection both need the same
// answer to "does this name repeat across endpoints, or is this device
// single-endpoint": a single-endpoint device must render with NO endpoint
// label at all (byte-identical to the pre-Task-9 UI), while a name that
// repeats gets a "· ep N" suffix on every one of its rows. Factored out as a
// pure, name-counting helper (rather than inlined per call site) so both
// render sites -- and this file's own test -- share one definition of
// "repeats".
export function multiEndpointNames(items) {
  const endpointsByName = new Map()
  for (const item of items) {
    let seen = endpointsByName.get(item.name)
    if (!seen) {
      seen = new Set()
      endpointsByName.set(item.name, seen)
    }
    seen.add(item.endpoint)
  }
  const repeated = new Set()
  for (const [name, endpoints] of endpointsByName) {
    if (endpoints.size > 1) repeated.add(name)
  }
  return repeated
}

// "Switch" -> "Switch · ep 2" when `showEp` is true, else the label
// unchanged -- the single suffix format used by both the caps table
// (per-cap label) and ActionsSection's per-action row/header lines, so the
// "· ep N" convention can't drift between the two render sites.
export function epSuffixed(label, endpoint, showEp) {
  return showEp ? `${label} · ep ${endpoint}` : label
}
