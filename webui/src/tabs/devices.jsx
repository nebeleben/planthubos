import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'
import { loadCaps, capLabel, fmtCap } from '../lib/caps.js'
import { multiEndpointNames, epSuffixed } from '../lib/endpoints.js'
import {
  previewEntry, validateEntry, shapeMapping, provenanceLabel, clusterHex, proposalKey, proposalDraft, toWireEntry,
} from '../lib/mapping.js'
import { hasAiKey, normEndpoint } from '../lib/ai/settings.js'
import { aiComplete, AiError } from '../lib/ai/provider.js'
import { buildProfilePrompt, parseAiProfile } from '../lib/ai/profileFromAi.js'
import { resolveVendor } from '../lib/vendors.js'
import {
  fmtRemainingCooldown, fmtBudget, verdictLabel, switchStateLabel, resolveActionSend, validateDuration,
} from '../lib/actuators.js'

function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

const KIND_LABEL = { ble: 'Bluetooth', espnow: 'ESP-NOW', zb: 'Zigbee' }
// Fixed display order regardless of which kinds are actually present --
// stable groupings read better than "whatever order the registry happened
// to return them in".
const KIND_ORDER = ['ble', 'espnow', 'zb']

function plantLabel(p) {
  return p.name || `Plant ${p.id}`
}

// Bind-key material is WRITE-ONLY (spec §4: "Keys are never returned by any
// GET" -- bthome.h's bindkey_get()/bindkey_has() contract). This field only
// ever POSTs a key or a null clear to /api/v1/devices/{id}/key; it never
// tries to read one back, and `has_key` (already on every GET /api/v1/devices
// entry regardless of kind) is the only state it renders between edits.
// Only BLE devices can currently have a key set (api_v1.c's devices_json.c
// comment: bindkey_has() is checked for every kind, but POST .../key only
// ever matters for BTHome's AES-CCM payloads) -- devices.jsx's own isBle
// gate already limits this component's caller to BLE rows.
function BindKeyField({ deviceId, hasKey }) {
  const [key, setKey] = useState('')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth | invalid

  async function submit(newKey) {
    setState('saving')
    try {
      const res = await fetch(`/api/v1/devices/${deviceId}/key`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ key: newKey }),
      })
      if (res.ok) { setState('saved'); setKey('') }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  function onSet(e) {
    e.preventDefault()
    if (!/^[0-9a-fA-F]{32}$/.test(key)) { setState('invalid'); return }
    submit(key)
  }

  function onClear() {
    if (!confirm('Clear the bind key for this device?')) return
    submit(null)
  }

  return (
    <form onSubmit={onSet} class="namef">
      <span class="hint">{hasKey ? 'key set' : 'no key'}</span>
      <input value={key} maxlength={32} placeholder="32 hex chars"
             onInput={(e) => { setKey(e.currentTarget.value); setState('idle') }} />
      <button type="submit" class="btn-primary" disabled={state === 'saving'}>
        {state === 'saving' ? '…' : 'Set key'}
      </button>
      {hasKey && (
        <button type="button" class="btn-destructive" onClick={onClear} disabled={state === 'saving'}>
          Clear
        </button>
      )}
      {state === 'saved' && <span class="hint">saved</span>}
      {state === 'invalid' && <span class="error">key must be 32 hex chars</span>}
      {state === 'error' && <span class="error">failed</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </form>
  )
}

// GET /api/v1/devices' `id` is the canonical device-id string (spec §2,
// e.g. "ble:A4C138xxxxxx") -- the rename route below is still mac-keyed
// (POST /api/v1/sensors/{MAC12}, api_v1.c's sensors_rename_post) and only
// ever resolves a name for BLE-kind devices (devices_json.c's device_json:
// app_config_get_sensor_name() is only consulted when e->id.kind ==
// DEV_KIND_BLE). Strips the "ble:" prefix to recover the bare 12 hex chars
// that route expects.
function mac12FromBleId(id) {
  const i = id.indexOf(':')
  return i < 0 ? id : id.slice(i + 1)
}

// Best-effort error text for a non-ok wrapper-bind response. 409 ("binding
// table full", api_v1.c's devices_wrapper_post()) sends a JSON {"error":..}
// body via send_409; 400/404 use httpd_resp_send_err()'s default body,
// which is not JSON, so this falls back to `fallback` rather than throwing
// on a failed .json() parse.
async function wrapperErrText(res, fallback) {
  try {
    const body = await res.json()
    if (body && typeof body.error === 'string' && body.error) return body.error
  } catch {}
  return `${fallback} (${res.status})`
}

// Device-level stop button (M5b Task 12, spec §7 design points: "Lockout
// sits next to the device it governs"). PUT .../actions/{action}/guards
// only accepts a body keyed by ONE action's URL, but actor_set_lockout()
// applies it to the whole device regardless of which action named the URL
// (api_v1.c's devices_guards_put() comment) -- callers just need any one of
// the device's declared actions, so this always uses the first.
//
// Deliberately does NOT disable the manual controls below when lockout is
// on: actor_table.h's own contract is "lockout refuses ACTOR_SRC_RULE;
// permits MANUAL and SAFETY" -- a manual press bypasses lockout by design
// (the operator's own hand on the button is not the automation lockout
// exists to stop). Rendering this as if it blocked manual presses too would
// be exactly the "control ambiguous about whether it just fired" defect the
// brief warns about, so the hint text says what lockout actually does.
function LockoutControl({ deviceId, firstActionName, lockout, onChanged }) {
  const [busy, setBusy] = useState(false)
  const [state, setState] = useState('idle') // idle | error | unauth

  async function toggle() {
    setBusy(true)
    try {
      const res = await fetch(`/api/v1/devices/${deviceId}/actions/${firstActionName}/guards`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ lockout: !lockout }),
      })
      if (res.ok) { setState('idle'); onChanged(!lockout) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
    setBusy(false)
  }

  return (
    <div class="node-card-row lockout-row">
      <button type="button" class={lockout ? 'btn-destructive' : 'btn-secondary'}
              onClick={toggle} disabled={busy}>
        {busy ? '…' : lockout ? 'Locked out — release' : 'Lockout this device'}
      </button>
      <span class="hint">
        {lockout
          ? 'Rule-triggered commands are blocked. Manual controls below and safety closes still work.'
          : 'Stops the rules engine from firing this device automatically. Manual controls and safety closes are never blocked.'}
      </span>
      {state === 'error' && <span class="error">failed to update lockout</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </div>
  )
}

// How long a manual command is given to show up confirmed before this row
// stops waiting and tells the operator nothing came back -- generous over
// the connect+write+confirm round trip a GATT actuator needs (actor.h's own
// ACTOR_MANUAL_TTL_S queue deadline is 30s; this is the UI-side twin of
// that budget, not a guess).
const ACTION_CONFIRM_TIMEOUT_S = 30

// One action's control: a button (parameterless) or a bounded duration
// input plus a button, the guard text a refusal would otherwise surprise
// the operator with, and the send/confirm lifecycle for the round trip the
// brief calls out -- POSTing gets a 202 (QUEUED, api_v1.c's own comment:
// "the command is QUEUED, not necessarily dispatched yet"), not a
// confirmation that the actuator moved. This row stays in an explicit
// "sent — awaiting confirmation" state, distinguishable from both "idle"
// and "done", resolved ONLY by resolveActionSend() (lib/actuators.js) --
// last_fired_s advancing (dispatch reached the radio) and switch.state's
// own confirmed-at time advancing (a confirm read landed), never by
// `action.would_refuse_now`. That field is a live pre-check of a
// HYPOTHETICAL press evaluated right now, not this command's outcome (Task
// 12 fix round 1, CRITICAL finding 1: the previous design read it to decide
// "confirmed" vs "refused after queueing", which misreports in both
// directions -- see resolveActionSend()'s own doc comment for exactly how).
//
// `showEndpoint` (Task 11: multi-endpoint Zigbee) is true only when this
// action's name repeats across more than one endpoint on this same device
// (a dual valve, a multi-gang switch) -- ActionsSection computes it once for
// the whole d.actions list via multiEndpointNames() so every row for that
// name gets the "· ep N" suffix consistently, while a single-endpoint
// device's rows render with no suffix at all, byte-identical to before
// Task 9 started emitting `endpoint` on every action object. `action.endpoint`
// itself is always sent to the actuate route below regardless of
// `showEndpoint`, since sending the real endpoint is safe even when it's the
// device's only one (api_v1.c defaults it to the lowest declared endpoint
// when the body omits it -- prior behaviour).
function ActionControl({ deviceId, action, fetchedAtS, nowS, switchConfirmedAtS, showEndpoint }) {
  const isDuration = action.param === 'duration_s'
  const [paramStr, setParamStr] = useState('')
  // idle | sending | pending | dispatched | confirmed | refused | timeout | unauth | error
  const [sendState, setSendState] = useState('idle')
  const [sendMsg, setSendMsg] = useState('')
  const pendingSinceRef = useRef(0)
  // Baselines captured at the MOMENT this row sends its request -- not
  // compared against the wall clock (see resolveActionSend()'s doc comment
  // for why a wall-clock compare can false-positive on stale data), but
  // against the LATEST poll, to see whether either has since advanced.
  const dispatchBaselineRef = useRef(null)
  const confirmBaselineRef = useRef(null)

  const lastFiredAtS = action.last_fired_s == null ? null : fetchedAtS - action.last_fired_s
  const cooldownGuard = { cooldownS: action.cooldown_s, lastFiredAtS }
  const budgetGuard = { activationsThisHour: action.activations_this_hour, maxPerHour: action.max_per_hour }
  const remaining = fmtRemainingCooldown(cooldownGuard, nowS)
  const budgetExhausted = action.max_per_hour > 0 && action.activations_this_hour >= action.max_per_hour

  // Re-resolves on every fresh poll of `action`/`switchConfirmedAtS`
  // (devices.jsx's 10s refresh) and the live `nowS` ticker, while this row
  // is still watching (pending, or dispatched-but-hoping-for-a-late-confirm).
  // No request id travels with the command, so this is the same
  // "watch the state, not a promise" approach the confirm-read GATT layer
  // itself uses one level down.
  useEffect(() => {
    if (sendState !== 'pending' && sendState !== 'dispatched') return
    const outcome = resolveActionSend({
      dispatchBaselineS: dispatchBaselineRef.current,
      dispatchNowS: lastFiredAtS,
      confirmBaselineS: confirmBaselineRef.current,
      confirmNowS: switchConfirmedAtS,
      sentAtS: pendingSinceRef.current,
      nowS,
      timeoutS: ACTION_CONFIRM_TIMEOUT_S,
    })
    if (outcome !== sendState) setSendState(outcome)
  }, [sendState, lastFiredAtS, switchConfirmedAtS, nowS])

  // An out-of-range duration (blank, non-numeric, zero or over param_max)
  // must refuse to fire rather than silently defaulting or clamping --
  // irrigation.open/pump.run are not reversible once dispatched, so a value
  // the operator never actually chose must never reach the radio (Task 12
  // fix round 1, finding 2: an over-max value used to be silently clamped
  // down to param_max with no feedback).
  const validation = isDuration ? validateDuration(paramStr, action.param_max) : { valid: true, param: 0, reason: null }

  async function fire() {
    if (!validation.valid) return
    setSendState('sending')
    setSendMsg('')
    try {
      const res = await fetch(`/api/v1/devices/${deviceId}/actions/${action.name}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ param: validation.param, endpoint: action.endpoint }),
      })
      if (res.status === 202) {
        dispatchBaselineRef.current = lastFiredAtS
        confirmBaselineRef.current = switchConfirmedAtS
        pendingSinceRef.current = nowS
        setSendState('pending')
      } else if (res.status === 401) {
        setSendState('unauth')
      } else if (res.status === 409) {
        // This is a DIFFERENT, real-time signal from would_refuse_now above
        // -- the server's own synchronous refusal of THIS specific attempt
        // (api_v1.c's manual_refusal_reason()), not a polled pre-check --
        // so using it here is not the bug the resolver above exists to
        // avoid.
        const body = await res.json().catch(() => ({}))
        setSendState('refused')
        setSendMsg(verdictLabel(body.error || 'unknown'))
      } else {
        setSendState('error')
      }
    } catch {
      setSendState('error')
    }
  }

  const busy = sendState === 'sending' || sendState === 'pending'
  const disabled = busy || remaining != null || budgetExhausted || !validation.valid

  return (
    <div class="node-card-row action-row">
      <span class="mono">{epSuffixed(action.name, action.endpoint, showEndpoint)}</span>
      {isDuration && (
        <input type="number" min="1" max={action.param_max} step="1"
               placeholder={`1–${action.param_max}s`} value={paramStr}
               onInput={(e) => setParamStr(e.currentTarget.value)} disabled={busy} />
      )}
      <button type="button" class="btn-primary" onClick={fire} disabled={disabled}>
        {sendState === 'sending' ? '…' : sendState === 'pending' ? 'Sent — awaiting confirmation…' : 'Run'}
      </button>
      <span class="hint">
        right now: {verdictLabel(action.would_refuse_now)}
        {remaining ? ` · cooldown ${remaining} left` : ''}
        {' · '}{fmtBudget(budgetGuard)}
      </span>
      {isDuration && paramStr.trim() !== '' && !validation.valid && <span class="error">{validation.reason}</span>}
      {sendState === 'confirmed' && <span class="hint">✓ dispatched and confirmed</span>}
      {sendState === 'dispatched' && <span class="hint">dispatched — no confirmation received (check the Alerts tab)</span>}
      {sendState === 'refused' && <span class="error">refused — {sendMsg}</span>}
      {sendState === 'timeout' && <span class="error">no confirmation yet — check the Alerts tab</span>}
      {sendState === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
      {sendState === 'error' && <span class="error">request failed</span>}
    </div>
  )
}

// The actuator surface for one device (M5b Task 12): only rendered when
// devices_json.c added an "actions" key at all (an ordinary sensor gets
// none). switch.state (capability, not an action) is looked up by name out
// of the same d.caps every other capability renders from -- shown here,
// next to the controls that change it, rather than only in the generic
// capabilities table below, so the operator sees the confirmed state right
// beside the button that moves it.
function ActionsSection({ d, nowS, fetchedAtS, onLockoutChanged }) {
  const lockout = d.actions[0].lockout
  // Task 11 (multi-endpoint Zigbee): d.actions may now carry the same
  // action `name` more than once -- one object per endpoint (a dual valve,
  // a multi-gang switch) -- per Task 9's devices_json.c contract. This is
  // the set of names that actually repeat, so ActionControl below can add
  // its "· ep N" suffix ONLY where it's needed; a single-endpoint device's
  // action names all fall outside this set and render with no suffix at
  // all, byte-identical to before Task 9.
  const actionMultiEp = multiEndpointNames(d.actions)
  // Rendered whenever this device declares switch.on/switch.off on some
  // endpoint, or has a switch.state cap on some endpoint -- not only once
  // the capability has been confirmed at least once: on a brand-new
  // pairing the capability is absent from d.caps entirely (device_json.c
  // only lists a capability once e->caps[c].valid), and a silently missing
  // line there could read as "this device has no switch state" instead of
  // "not confirmed yet" -- the same ambiguity the brief's design points
  // warn against elsewhere. switchStateLabel(undefined) already renders
  // 'unknown' for exactly this case.
  //
  // A dual valve/multi-gang switch declares switch.on/off (and
  // switch.state) on MORE THAN ONE endpoint, so this is now a per-endpoint
  // list of distinct endpoints rather than a single device-wide flag --
  // one "Switch: ..." status line per endpoint that has one, each looked
  // up against the switch.state cap for THAT SAME endpoint (never just any
  // switch.state cap on the device). A single-endpoint device still gets
  // exactly one line with no "· ep N" suffix -- byte-identical to before.
  const switchEndpoints = [...new Set([
    ...d.caps.filter((c) => c.name === 'switch.state').map((c) => c.endpoint),
    ...d.actions.filter((a) => a.name === 'switch.on' || a.name === 'switch.off').map((a) => a.endpoint),
  ])].sort((x, y) => x - y)
  const multiSwitchEp = switchEndpoints.length > 1

  return (
    <div class="actions-section">
      <div class="node-card-row">
        <span class="hint">Actuator controls</span>
        {switchEndpoints.map((ep) => {
          const switchCap = d.caps.find((c) => c.name === 'switch.state' && c.endpoint === ep)
          return (
            <span class="hint" key={`switch:${ep}`}>
              {epSuffixed('Switch', ep, multiSwitchEp)}: <strong>{switchStateLabel(switchCap && switchCap.value)}</strong>
              {switchCap ? ` (confirmed ${fmtAge(switchCap.age_s)})` : ' (not confirmed yet)'}
            </span>
          )
        })}
      </div>
      <LockoutControl deviceId={d.id} firstActionName={d.actions[0].name} lockout={lockout}
                       onChanged={(newLockout) => onLockoutChanged(d.id, newLockout)} />
      {d.actions.map((a) => {
        // Per-endpoint switch confirmation (see the block comment above):
        // the SAME fetchedAtS-derived shape action.last_fired_s uses (see
        // ActionControl), so resolveActionSend() can compare it against a
        // baseline the same way -- but now matched against THIS action's
        // own endpoint's switch.state cap, not the device's only one. null
        // when that endpoint's capability has never been confirmed at all
        // (absent, or its age_s itself null) -- which is exactly "this
        // action's dispatch can never resolve past 'dispatched'" for a
        // wrapper with no confirm block, resolveActionSend()'s own
        // documented ceiling for that case.
        const switchCap = d.caps.find((c) => c.name === 'switch.state' && c.endpoint === a.endpoint)
        const switchConfirmedAtS = switchCap && switchCap.age_s != null ? fetchedAtS - switchCap.age_s : null
        return (
          <ActionControl key={`${a.id}:${a.endpoint}`} deviceId={d.id} action={a} fetchedAtS={fetchedAtS} nowS={nowS}
                          switchConfirmedAtS={switchConfirmedAtS} showEndpoint={actionMultiEp.has(a.name)} />
        )
      })}
    </div>
  )
}

// Zigbee EF00 (Tuya) datapoint ids are conventionally read/written as hex
// (vendor docs and zigbee2mqtt alike label them "DP 0x02" etc.) -- this is
// display-only, distinct from the URL's plain-decimal dp_id (api_v1.c's
// datapoints route parses base-0, so a zero-padded/hex path segment would
// misparse; the fetch calls below always send `dp.dp_id` as-is, a Number).
function fmtDpId(dpId) {
  return dpId.toString(16).padStart(2, '0')
}

// One observed Tuya datapoint + its map/unmap controls (task 7, backend
// contract already merged: GET .../datapoints, POST/DELETE
// .../datapoints/{dp_id}). Factored out the same way BindKeyField/
// ActionControl are: its own local select/scale/request state, independent
// of every other row's.
//
// Unlike ActionControl's fire-and-poll-for-confirmation dance, a map/unmap
// POST/DELETE here is a plain synchronous write (`{"ok":true}`, no queueing)
// -- so this simply re-fetches the parent's list on success rather than
// tracking a pending/confirmed lifecycle.
function DatapointRow({ deviceId, dp, caps, onChanged }) {
  const isMapped = dp.cap_id != null
  const [sel, setSel] = useState(isMapped ? String(dp.cap_id) : '')
  const [scaleStr, setScaleStr] = useState(dp.scale != null ? String(dp.scale) : '1')
  const [state, setState] = useState('idle') // idle | busy | error | unauth

  async function send(method, body) {
    setState('busy')
    try {
      const res = await fetch(`/api/v1/devices/${deviceId}/datapoints/${dp.dp_id}`, {
        method,
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: body ? JSON.stringify(body) : undefined,
      })
      if (res.ok) { setState('idle'); onChanged() }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  function onMap() {
    if (!sel) return
    send('POST', { cap_id: Number(sel), scale: Number(scaleStr) || 1 })
  }

  function onUnmap() {
    send('DELETE')
  }

  return (
    <tr>
      <td class="mono">DP 0x{fmtDpId(dp.dp_id)}</td>
      <td>{dp.value}</td>
      <td class="hint">{fmtAge(dp.age_s)}</td>
      <td>
        {isMapped
          ? <span>{capLabel(caps, dp.cap_id)} × {dp.scale}</span>
          : <span class="hint">unmapped</span>}
      </td>
      <td>
        <select value={sel} onChange={(e) => setSel(e.currentTarget.value)} disabled={state === 'busy'}>
          <option value="">— capability —</option>
          {[...caps.values()].map((c) => (
            <option key={c.id} value={c.id}>{capLabel(caps, c.id)}</option>
          ))}
        </select>
        {' '}
        <input type="number" step="any" value={scaleStr} size="4"
               onInput={(e) => setScaleStr(e.currentTarget.value)} disabled={state === 'busy'} />
        {' '}
        <button type="button" class="btn-primary" onClick={onMap} disabled={!sel || state === 'busy'}>
          Map
        </button>
        {isMapped && (
          <button type="button" class="btn-destructive" onClick={onUnmap} disabled={state === 'busy'}>
            Unmap
          </button>
        )}
        {state === 'error' && <span class="error">failed</span>}
        {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
      </td>
    </tr>
  )
}

// The Zigbee EF00 (Tuya) datapoint surface (task 7, spec's tuya-ef00-
// datapoints amendment): only worth fetching once a device card is
// expanded (the operator opened it to look at exactly this kind of detail)
// and only worth rendering once the fetch actually returns datapoints --
// an ordinary non-Tuya Zigbee device (or one that hasn't reported yet)
// gets an empty {"datapoints":[]} and this renders nothing at all, same
// discipline as the caps table's own "no live capabilities yet" branch
// just below it not needing a heading when there's nothing to show.
function DatapointsSection({ deviceId, caps, open }) {
  const [dps, setDps] = useState([])
  const [dpError, setDpError] = useState(false)

  function refresh(signal) {
    return fetch(`/api/v1/devices/${deviceId}/datapoints`, { signal })
      .then((r) => r.json())
      .then((body) => { setDps(body.datapoints || []); setDpError(false) })
  }

  // Fetched only while the card is expanded -- collapsed cards stay
  // mounted (see DeviceCard's own body, always in the DOM regardless of
  // `open`) but have no reason to poll a detail the operator isn't looking
  // at, matching the brief's "do NOT fetch when collapsed" instruction.
  useEffect(() => {
    if (!open) return
    const controller = new AbortController()
    refresh(controller.signal).catch((err) => { if (err.name !== 'AbortError') setDpError(true) })
    return () => controller.abort()
  }, [open, deviceId])

  if (dps.length === 0) return dpError ? <p class="hint">Datapoints unavailable.</p> : null

  return (
    <div class="node-card-row">
      <span class="hint">Tuya datapoints</span>
      <div class="table-scroll">
        <table class="devices">
          <thead><tr><th>DP</th><th>Raw</th><th>Age</th><th>Mapped to</th><th>Map / unmap</th></tr></thead>
          <tbody>
            {dps.map((dp) => (
              <DatapointRow key={dp.dp_id} deviceId={deviceId} dp={dp} caps={caps}
                            onChanged={() => refresh().catch(() => {})} />
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}

// "soil.moisture" -> "Soil Moisture", same humanisation rule caps.js'
// capLabel() applies to a capability id's own `.name` -- duplicated here
// (rather than imported) because every value this mapping surface renders
// a capability THROUGH is already the dotted name string itself (the
// mapping wire format's own convention, api_v1.c's mapping_entry_json()),
// never a numeric id, so there is no `caps.get(id)` lookup to route
// through capLabel in the first place.
function capNameLabel(name) {
  if (!name) return ''
  return name.split('.').map((w) => w.charAt(0).toUpperCase() + w.slice(1)).join(' ')
}

// toWireEntry/proposalDraft/clusterHex/proposalKey now live in
// ../lib/mapping.js (fix round 1: moved out of this file so the identity
// keying that fixes the proposals-list stale-row bug -- see proposalKey's
// own doc comment -- has pure, node:test-reachable coverage; devices.jsx
// has no component-level test harness of its own).

// Shared editable fields for one dp/suppress draft entry -- used by both
// ProposalRow's Edit control (kind fixed to whatever the proposal already
// is; `allowKindChange` false) and ManualAddForm (operator picks the kind
// first; `allowKindChange` true). `caps` is the full loaded capability
// table (lib/caps.js's loadCaps()), not just this device's own live `caps`
// list -- a mapping can legitimately target a capability this device
// hasn't reported a live value for yet (dev_profile_resolve_cap() on the
// hub side validates by name, not by "already seen on this device").
function EntryFields({ draft, onChange, caps, allowKindChange, disabled }) {
  function set(patch) { onChange({ ...draft, ...patch }) }
  function setKind(kind) {
    onChange(kind === 'suppress'
      ? { kind: 'suppress', source_cluster: '' }
      : { kind: 'dp', dp_id: '', cap: '', scale: '1', offset: '0' })
  }
  return (
    <span class="assign-control">
      {allowKindChange && (
        <select value={draft.kind} onChange={(e) => setKind(e.currentTarget.value)} disabled={disabled}>
          <option value="dp">Map a DP</option>
          <option value="suppress">Suppress a cluster</option>
        </select>
      )}
      {draft.kind === 'suppress' ? (
        <input value={draft.source_cluster} placeholder="0x0405 or 1029" size="10"
               onInput={(e) => set({ source_cluster: e.currentTarget.value })} disabled={disabled} />
      ) : (
        <>
          <input type="number" min="0" max="255" value={draft.dp_id} placeholder="DP id" size="4"
                 onInput={(e) => set({ dp_id: e.currentTarget.value })} disabled={disabled} />
          <select value={draft.cap} onChange={(e) => set({ cap: e.currentTarget.value })} disabled={disabled}>
            <option value="">— capability —</option>
            {[...caps.values()].map((c) => (
              <option key={c.id} value={c.name}>{capLabel(caps, c.id)}</option>
            ))}
          </select>
          <input type="number" step="any" value={draft.scale} placeholder="scale" size="4"
                 onInput={(e) => set({ scale: e.currentTarget.value })} disabled={disabled} />
          <input type="number" step="any" value={draft.offset} placeholder="offset" size="4"
                 onInput={(e) => set({ offset: e.currentTarget.value })} disabled={disabled} />
        </>
      )}
    </span>
  )
}

// One proposed entry (device-mapping-profiles Task 9, spec §4/§6): the
// preview row IS the review safety gate -- previewEntry evaluates the
// proposal against its own latest observed sample (or reports "no sample
// yet" rather than a misleading guess) so the operator sees what a
// Confirm would actually apply before ever pressing it. Edit re-runs that
// same preview/validate against whatever the operator changes, live, and
// Confirm is disabled until validateEntry says the (possibly-edited) draft
// is well-formed. Reject is LOCAL ONLY -- there is no dismiss/hide
// endpoint on the hub, so a rejected proposal simply drops out of this
// card's own in-memory list; nothing is written until Confirm is pressed
// (Confirm is the only control here that ever calls fetch).
function ProposalRow({ proposal, sample, caps, capNames, unitByCap, provenance, onConfirm, onReject }) {
  const [editing, setEditing] = useState(false)
  const [draft, setDraft] = useState(() => proposalDraft(proposal))
  const [state, setState] = useState('idle') // idle | busy | error | unauth

  const preview = previewEntry(draft, sample, unitByCap)
  const check = validateEntry(draft, capNames)

  async function confirm() {
    if (!check.ok) return
    setState('busy')
    const status = await onConfirm(toWireEntry(draft, provenance))
    setState(status === 'ok' ? 'idle' : status)
  }

  return (
    <tr>
      <td>{preview.label}</td>
      <td class="mono">{preview.value}</td>
      <td>
        {editing
          ? <EntryFields draft={draft} onChange={setDraft} caps={caps} allowKindChange={false} disabled={state === 'busy'} />
          : (
            <span class="hint">
              {draft.kind === 'suppress' ? `suppress ${draft.source_cluster}` : `${capNameLabel(draft.cap)} × ${draft.scale}${Number(draft.offset) ? ` + ${draft.offset}` : ''}`}
            </span>
          )}
      </td>
      <td>
        <button type="button" class="btn-primary" onClick={confirm} disabled={!check.ok || state === 'busy'}>
          {state === 'busy' ? '…' : 'Confirm'}
        </button>
        {' '}
        <button type="button" class="btn-secondary" onClick={() => setEditing((e) => !e)} disabled={state === 'busy'}>
          {editing ? 'Done' : 'Edit'}
        </button>
        {' '}
        <button type="button" class="btn-destructive" onClick={onReject} disabled={state === 'busy'}>
          Reject
        </button>
        {editing && !check.ok && <span class="error">{check.error}</span>}
        {state === 'error' && <span class="error">failed</span>}
        {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
      </td>
    </tr>
  )
}

// Hand-added mapping (device-mapping-profiles Task 9): the manual-add path
// for a DP the device has reported (observed-unmapped, below) but no
// profile proposes a mapping for, or a suppress the operator wants without
// waiting on a proposal at all. Submit is disabled until validateEntry
// accepts the current draft, same gate ProposalRow's Confirm uses --
// nothing here ever reaches fetch() on a draft the hub would 400 anyway.
function ManualAddForm({ caps, capNames, onAdd }) {
  const [draft, setDraft] = useState({ kind: 'dp', dp_id: '', cap: '', scale: '1', offset: '0' })
  const [state, setState] = useState('idle') // idle | busy | error | unauth
  const [touched, setTouched] = useState(false)

  const check = validateEntry(draft, capNames)

  async function submit(e) {
    e.preventDefault()
    if (!check.ok) return
    setState('busy')
    const status = await onAdd(toWireEntry(draft, 'manual'))
    setState(status === 'ok' ? 'idle' : status)
    if (status === 'ok') {
      setDraft({ kind: 'dp', dp_id: '', cap: '', scale: '1', offset: '0' })
      setTouched(false)
    }
  }

  return (
    <form onSubmit={submit} class="assign-control">
      <EntryFields draft={draft} onChange={(d) => { setDraft(d); setTouched(true) }} caps={caps}
                   allowKindChange disabled={state === 'busy'} />
      <button type="submit" class="btn-primary" disabled={!check.ok || state === 'busy'}>
        {state === 'busy' ? '…' : 'Add mapping'}
      </button>
      {touched && !check.ok && <span class="error">{check.error}</span>}
      {state === 'error' && <span class="error">failed</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </form>
  )
}

// The device-mapping-profiles Task 9 mapping surface: applied caps +
// suppress entries (each with its own provenance tag), the matched
// profile's outstanding proposals (each reviewed live via ProposalRow's
// preview before Confirm), observed-but-unmapped DPs, and the manual-add
// path -- GET/POST /api/v1/devices/{id}/mapping (Task 8). Fetched only
// while the card is open, same discipline as DatapointsSection just above
// (a detail the operator isn't looking at is not worth polling), and a
// POST's response (devices_mapping_post() replies with the SAME shape its
// own GET does) is applied directly to this component's state instead of
// triggering a second round-trip fetch.
function MappingSection({ deviceId, caps, open }) {
  const [mapping, setMapping] = useState(null)
  const [mapError, setMapError] = useState(false)

  // Task 10 (device-mapping-profiles, WebUI AI assist): the hub-persisted
  // AI config (GET /api/v1/config/ai, Task 8) -- base_url/api_key/model
  // for the SAME OpenAI-compatible dialect provider.js already speaks,
  // deliberately distinct from ../lib/ai/settings.js's browser-local
  // config (a different feature, used by wrapper/rule generation). null
  // while unfetched or on any failure (including a 401 on a claimed hub
  // with no key set) -- "Ask AI" simply stays disabled/hidden rather than
  // guessing at a config that didn't load.
  const [aiCfg, setAiCfg] = useState(null)
  const [aiBusy, setAiBusy] = useState(false)
  const [aiError, setAiError] = useState('')
  // AI-sourced proposals, kept separate from the matched-profile's own
  // `mapping.proposals` so each row can carry its own provenance ("ai" vs
  // "profile") into ProposalRow/toWireEntry -- see the render below.
  const [aiProposals, setAiProposals] = useState([])

  function refresh(signal) {
    return fetch(`/api/v1/devices/${deviceId}/mapping`, { signal })
      .then((r) => r.json())
      .then((body) => { setMapping(shapeMapping(body)); setMapError(false) })
  }

  useEffect(() => {
    if (!open) return
    const controller = new AbortController()
    refresh(controller.signal).catch((err) => { if (err.name !== 'AbortError') setMapError(true) })
    return () => controller.abort()
  }, [open, deviceId])

  // Fetched only while the card is open, same discipline as the mapping
  // fetch just above -- and auth-gated on a claimed hub (config_ai_get()
  // hands back the real api_key, unlike every other unauthenticated GET
  // on this tab), so a missing/expired key here just means "Ask AI" stays
  // disabled rather than throwing.
  useEffect(() => {
    if (!open) return
    const controller = new AbortController()
    fetch('/api/v1/config/ai', { signal: controller.signal, headers: authHeaders() })
      .then((r) => (r.ok ? r.json() : null))
      .then(setAiCfg)
      .catch(() => {})
    return () => controller.abort()
  }, [open, deviceId])

  // POST reply carries the mapping's whole new state (applied/suppress/
  // proposals recomputed against it, observed_unmapped unchanged) -- no
  // separate re-fetch needed after a successful apply.
  async function apply(entry) {
    try {
      const res = await fetch(`/api/v1/devices/${deviceId}/mapping`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ entries: [entry] }),
      })
      if (res.ok) {
        setMapping(shapeMapping(await res.json()))
        return 'ok'
      }
      return res.status === 401 ? 'unauth' : 'error'
    } catch {
      return 'error'
    }
  }

  // Keyed by proposalKey() identity, not array index -- fix round 1: an
  // index-based removal (and the matching index-based `key` the list used
  // to render with) let Preact hand the next proposal that shifted into a
  // removed row's slot the SAME component instance, stale `draft` and all.
  // Filtering by identity here keeps this in step with the identity `key`
  // the proposals list is now rendered with below.
  function rejectProposal(key) {
    setMapping((prev) => ({ ...prev, proposals: prev.proposals.filter((p) => proposalKey(p) !== key) }))
  }

  function rejectAiProposal(key) {
    setAiProposals((prev) => prev.filter((p) => proposalKey(p) !== key))
  }

  // "Ask AI" (Task 10, spec §5): fetch this device's prompt-inputs from
  // the hub (Task 8 -- fingerprint/observed DPs/canonical caps, no prompt
  // text and no provider call on that end), assemble the prompt, call the
  // configured OpenAI-compatible endpoint DIRECTLY FROM THE BROWSER
  // (aiComplete/provider.js -- the hub never sees the response), then
  // validate every returned entry against THIS device's own canonical cap
  // list before it is ever shown. Nothing here writes anything -- a valid
  // entry becomes an aiProposals row, reviewed through the exact same
  // ProposalRow preview/Confirm/Edit/Reject gate the matched-profile's own
  // proposals use below, just tagged provenance "ai" instead of "profile".
  // Every failure mode (unreachable/CORS, bad key, bad JSON, a
  // hallucinated cap name) lands in aiError as a plain message and leaves
  // manual mapping (ManualAddForm, further below) untouched -- never a
  // crash, never an auto-applied entry.
  async function askAi() {
    if (!aiCfg || !aiCfg.base_url) return
    setAiBusy(true)
    setAiError('')
    try {
      const inpRes = await fetch(`/api/v1/devices/${deviceId}/ai-prompt-inputs`)
      if (!inpRes.ok) throw new Error(`could not read prompt inputs from the hub (${inpRes.status})`)
      const inputs = await inpRes.json()
      const { system, user } = buildProfilePrompt(inputs)
      const text = await aiComplete({
        system,
        user,
        settings: {
          kind: 'openai',
          endpoint: normEndpoint(aiCfg.base_url),
          model: aiCfg.model || '',
          key: aiCfg.api_key || '',
        },
      })
      // Validated against THIS call's own canonical cap list (from
      // ai-prompt-inputs), not the wider webui `caps` table passed into
      // this component -- the two are expected to agree, but the
      // hub-supplied list is the one the prompt itself was built from.
      const capNameSet = new Set((inputs.caps || []).map((c) => c.name))
      const result = parseAiProfile(text, capNameSet)
      if (!result.ok) {
        setAiError(result.error || 'the AI response could not be used')
      } else {
        setAiProposals((prev) => {
          const merged = new Map(prev.map((p) => [proposalKey(p), p]))
          for (const e of result.entries) merged.set(proposalKey(e), e)
          return [...merged.values()]
        })
      }
    } catch (err) {
      setAiError(err instanceof AiError ? err.message : (err && err.message) || 'AI request failed')
    }
    setAiBusy(false)
  }

  if (!mapping) return mapError ? <p class="hint">Mapping unavailable.</p> : null

  const capNames = new Set([...caps.values()].map((c) => c.name))
  const unitByCap = {}
  for (const c of caps.values()) unitByCap[c.name] = c.unit
  function sampleFor(dpId) {
    const o = mapping.observed.find((x) => x.dp_id === dpId)
    return o ? { value: o.value } : null
  }

  const askAiTitle = aiCfg && aiCfg.base_url
    ? undefined
    : 'Configure the mapping AI endpoint in Config → AI mapping assist'

  return (
    <div class="node-card-row">
      <span class="hint">Capability mapping</span>
      <span class="node-card-row">
        <button type="button" class="btn-secondary" onClick={askAi}
                disabled={!aiCfg || !aiCfg.base_url || aiBusy} title={askAiTitle}>
          {aiBusy ? 'Asking AI…' : 'Ask AI'}
        </button>
        {aiError && <span class="error"> AI: {aiError}</span>}
      </span>
      {mapping.applied.length === 0 && mapping.suppress.length === 0 ? (
        <p class="hint">No mappings applied yet.</p>
      ) : (
        <div class="table-scroll">
          <table class="devices">
            <thead><tr><th>DP</th><th>Capability</th><th>Scale</th><th>Offset</th><th>Source</th></tr></thead>
            <tbody>
              {mapping.applied.map((a) => (
                <tr key={a.dp_id}>
                  <td class="mono">DP 0x{fmtDpId(a.dp_id)}</td>
                  <td>{capNameLabel(a.cap)}</td>
                  <td>{a.scale}</td>
                  <td>{a.offset}</td>
                  <td><span class="hint">{provenanceLabel(a.provenance)}</span></td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {mapping.suppress.length > 0 && (
        <p class="hint">
          Suppressed clusters: {mapping.suppress.map((s) => `${clusterHex(s.cluster)} (${provenanceLabel(s.provenance)})`).join(', ')}
        </p>
      )}
      {mapping.proposals.length > 0 && (
        <>
          <span class="hint">Proposed (from matched profile)</span>
          <div class="table-scroll">
            <table class="devices">
              <thead><tr><th>Proposal</th><th>Preview</th><th>Entry</th><th>Actions</th></tr></thead>
              <tbody>
                {mapping.proposals.map((p) => (
                  <ProposalRow key={proposalKey(p)} proposal={p} caps={caps} capNames={capNames} unitByCap={unitByCap}
                               provenance="profile" sample={p.kind === 'dp' ? sampleFor(p.dp_id) : null}
                               onConfirm={apply} onReject={() => rejectProposal(proposalKey(p))} />
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
      {aiProposals.length > 0 && (
        <>
          <span class="hint">Proposed (from AI — review before confirming)</span>
          <div class="table-scroll">
            <table class="devices">
              <thead><tr><th>Proposal</th><th>Preview</th><th>Entry</th><th>Actions</th></tr></thead>
              <tbody>
                {aiProposals.map((p) => (
                  <ProposalRow key={proposalKey(p)} proposal={p} caps={caps} capNames={capNames} unitByCap={unitByCap}
                               provenance="ai" sample={p.kind === 'dp' ? sampleFor(p.dp_id) : null}
                               onConfirm={async (entry) => {
                                 const status = await apply(entry)
                                 if (status === 'ok') rejectAiProposal(proposalKey(p))
                                 return status
                               }}
                               onReject={() => rejectAiProposal(proposalKey(p))} />
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
      {mapping.observed.length > 0 && (
        <>
          <span class="hint">Observed, not yet mapped</span>
          <div class="table-scroll">
            <table class="devices">
              <thead><tr><th>DP</th><th>Type</th><th>Raw</th><th>Age</th></tr></thead>
              <tbody>
                {mapping.observed.map((o) => (
                  <tr key={o.dp_id}>
                    <td class="mono">DP 0x{fmtDpId(o.dp_id)}</td>
                    <td>{o.type}</td>
                    <td>{o.value}</td>
                    <td class="hint">{fmtAge(o.age_s)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
      <span class="hint">Add a mapping</span>
      <ManualAddForm caps={caps} capNames={capNames} onAdd={apply} />
    </div>
  )
}

// Zigbee-only (device-mapping-profiles Task 8's ".../identify" route 400s
// on anything else): triggers a Basic-cluster (0x0000) manufacturer/model
// re-read for an already-joined device. Fire-and-forget on the hub side
// (zigbee_read_identity() is void -- the answer lands asynchronously via a
// re-announce, api_v1.c's own doc comment), so a 202 here only means
// "queued", not "identity updated yet" -- the next GET /api/v1/devices poll
// picks up a changed manufacturer/model on its own, same as every other
// live field on this tab.
function IdentifyButton({ deviceId }) {
  const [state, setState] = useState('idle') // idle | busy | done | notfound | unauth | error

  async function onClick() {
    setState('busy')
    try {
      const res = await fetch(`/api/v1/zigbee/devices/${deviceId}/identify`, {
        method: 'POST',
        headers: authHeaders(),
      })
      if (res.ok) setState('done')
      else setState(res.status === 404 ? 'notfound' : res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <span class="namef">
      <button type="button" class="btn-secondary" onClick={onClick} disabled={state === 'busy'}>
        {state === 'busy' ? '…' : 'Re-read identity'}
      </button>
      {state === 'done' && <span class="hint">requested — check back shortly</span>}
      {state === 'notfound' && <span class="error">device not found</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
      {state === 'error' && <span class="error">failed</span>}
    </span>
  )
}

// Same collapsible-card shape as nodes.jsx's NodeCard / rules.jsx's
// RuleCard: name/id + last-seen while collapsed, details in the body.
function DeviceCard({ d, caps, plantNameById, open, onToggle, onRenamed, nowS, fetchedAtS, onLockoutChanged, wrappers, onUnassignWrapper }) {
  const isBle = d.kind === 'ble'
  const [name, setName] = useState(d.name || '')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth

  async function save(e) {
    e.preventDefault()
    setState('saving')
    try {
      const res = await fetch(`/api/v1/sensors/${mac12FromBleId(d.id)}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ name }),
      })
      if (res.ok) { setState('saved'); onRenamed(d.id, name) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  const plantNames = d.plant_ids.map((id) => plantNameById.get(id) || `Plant ${id}`)
  // Task 11 (multi-endpoint Zigbee): d.caps carries one object per (name,
  // endpoint) instance per Task 9's devices_json.c contract -- a name on
  // more than one endpoint (a dual valve, a multi-gang switch) appears as
  // multiple rows below. Only THOSE names get a "· ep N" suffix; a
  // single-endpoint device's caps all fall outside this set, so its table
  // renders with no suffix at all -- byte-identical to before Task 9.
  const capMultiEp = multiEndpointNames(d.caps)

  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{d.name || d.id}</span>
          {d.name && <span class="node-card-mac mono">{d.id}</span>}
        </span>
        <span class="node-card-age hint">
          {d.stale
            ? `last known${d.last_seen_s == null ? '' : ' · ' + fmtAge(d.last_seen_s)}`
            : fmtAge(d.last_seen_s)}
        </span>
      </button>
      <div class="node-card-body">
        {/* Only a BLE device's addr is mac-keyed, which is the only key the
            rename store understands (see mac12FromBleId's doc comment) --
            ESP-NOW/Zigbee devices have no display-name form yet. */}
        {isBle && (
          <form onSubmit={save} class="namef">
            <input value={name} maxlength={32} placeholder={d.id}
                   onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
            <button type="submit" class="btn-primary" disabled={state === 'saving'}>
              {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
            </button>
            {state === 'error' && <span class="error">failed</span>}
            {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
          </form>
        )}
        <div class="node-card-row">
          <span class="hint">{d.via ? `via ${d.via}` : 'direct'} · {d.rssi} dBm</span>
        </div>
        {/* Device-mapping-profiles Task 9: the announced fingerprint
            (manufacturer/model, Task 7's devices_json.c emission) --
            omitted entirely by the hub until a Basic-cluster read lands, so
            a Zigbee device with none yet still gets this row (with the
            re-read control) rather than disappearing; a non-Zigbee kind
            with no fingerprint (the common case today) renders nothing at
            all, matching this file's own "absent beats null" convention. */}
        {(d.manufacturer || d.model || d.kind === 'zb') && (
          <div class="node-card-row">
            <span class="hint">
              Fingerprint: {(d.manufacturer || d.model) ? `${d.manufacturer || '?'} ${d.model || ''}`.trim() : 'not read yet'}
            </span>
            {d.kind === 'zb' && <IdentifyButton deviceId={d.id} />}
          </div>
        )}
        {/* M5a Task 7 (spec §5, amended): GATT read status. Present only for
            devices whose matched wrapper declares a connect plan
            (devices_json.c) -- an advertisement-only device gets no
            `d.gatt` at all, so this row simply doesn't render for it. Since
            the amended spec cut M5a's per-attempt event-log write, this row
            is the ENTIRE visibility surface for a connect block that never
            succeeds -- so a device with no successful read yet (last_read_s
            null, fmtAge renders "never") gets `error` styling rather than
            the muted `hint` a healthy device gets, matching the weight
            wrappers.jsx's WrapperCard already gives its own last_error (M4
            review: a security/reliability-relevant line must not be styled
            as transient status). A radio-ok-but-decode-emitted-nothing
            attempt (gatt_sched_attempt(), Task 6) leaves last_read_s at
            whatever the last REAL success was -- not necessarily null, so
            not necessarily red -- while last_error still explains the
            current problem; the two are rendered as separate spans so a
            stale-but-real timestamp next to an explanatory error reads as
            "worked before, here's what's wrong now" rather than
            contradicting itself. */}
        {d.gatt && (
          <div class="node-card-row">
            <span class={`hint${d.gatt.last_read_s == null ? ' error' : ''}`}>
              GATT · every {d.gatt.interval_s}s · last read {fmtAge(d.gatt.last_read_s)}
              {d.gatt.fails > 0 ? ` · ${d.gatt.fails} failed attempt${d.gatt.fails === 1 ? '' : 's'}` : ''}
            </span>
            {d.gatt.last_error && <span class="error">{d.gatt.last_error}</span>}
          </div>
        )}
        {d.actions && d.actions.length > 0 && (
          <ActionsSection d={d} nowS={nowS} fetchedAtS={fetchedAtS} onLockoutChanged={onLockoutChanged} />
        )}
        {isBle && (
          <div class="node-card-row">
            <BindKeyField deviceId={d.id} hasKey={d.has_key} />
          </div>
        )}
        {d.wrapper_id > 0 && (
          <div class="node-card-row">
            <span class="hint">wrapper: {(wrappers.find((w) => w.id === d.wrapper_id) || {}).name || d.wrapper_id}</span>
            {' '}
            <button type="button" onClick={() => onUnassignWrapper(d)}>Unassign</button>
          </div>
        )}
        {d.caps.length === 0 ? (
          <p class="hint">No live capabilities yet.</p>
        ) : (
          <div class="table-scroll">
            <table class="devices">
              <thead><tr><th>Capability</th><th>Value</th><th>Age</th></tr></thead>
              <tbody>
                {d.caps.map((c) => (
                  <tr key={`${c.id}:${c.endpoint}`}>
                    <td>{epSuffixed(capLabel(caps, c.id), c.endpoint, capMultiEp.has(c.name))}</td>
                    <td>{fmtCap(caps, c.id, c.value)}</td>
                    <td class="hint">{fmtAge(c.age_s)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
        <DatapointsSection deviceId={d.id} caps={caps} open={open} />
        <MappingSection deviceId={d.id} caps={caps} open={open} />
        <div class="node-card-row">
          <span class="hint">
            {plantNames.length > 0 ? `Bound to ${plantNames.join(', ')}` : 'Not bound to any plant'}
          </span>
        </div>
      </div>
    </div>
  )
}

// Renders a hex byte string spaced every 2 chars for readability -- same
// idea as the wrapper editor's disassembly <pre>, just for raw bytes.
function fmtHexBytes(hex) {
  return hex.match(/.{1,2}/g)?.join(' ') || hex
}

// Same collapsible node-card shape as DeviceCard: header carries the
// device-id + age, body carries RSSI, every captured sample's hex payload,
// and two DIFFERENT hops into WrappersTab's editor (M3 Task 8's
// hand-written "Add wrapper", M4 Task 7's "Generate wrapper with AI"
// alongside it) -- spec §5/§6's unknown-device discovery surface, devices
// no wrapper currently claims, captured so an operator or M4's AI can
// write one for it. Both hand app.jsx the whole raw device (onAddWrapper's
// contract changed in M4 Task 7 from just the newest hex to the full
// device -- app.jsx derives the newest hex itself, same as this card used
// to) since Generate needs every captured sample to build a prompt, not
// just one; WrapperTemplate.build({device}) / redact.js do the actual
// redaction, this file never touches device fields for that purpose.
// GET /api/v1/unknown's shape (id/rssi/last_seen_s/samples[{hex,len,ts}])
// is M4's own input contract -- rendered here as-is, never reshaped.
//
// Fix round 1: the two buttons used to call the identical onAddWrapper(d)
// and differ only in disabled state, so "Generate wrapper with AI" prefilled
// and switched tabs same as "Add wrapper" without ever generating anything
// -- a label promising something the click didn't do. onGenerateWrapper is
// a distinct callback so app.jsx can tell WrappersTab to actually start a
// generation on arrival; onAddWrapper (and its hand-written path) is
// untouched.
function UnknownDeviceCard({ d, open, onToggle, onAddWrapper, onGenerateWrapper, wrappers, onAssignWrapper }) {
  const newest = d.samples[d.samples.length - 1]   // s[] is oldest-first, newest-last (api_v1.c's unknown_get)
  const vendor = resolveVendor(d)                  // best-effort maker label (company id / OUI)
  const [sel, setSel] = useState('')
  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name mono">{d.id}</span>
          {vendor && <span class="node-card-mac hint">{vendor}</span>}
        </span>
        <span class="node-card-age hint">{fmtAge(d.last_seen_s)}</span>
      </button>
      <div class="node-card-body">
        <div class="node-card-row">
          <span class="hint">
            {d.rssi} dBm · {d.samples.length} sample{d.samples.length === 1 ? '' : 's'} captured
          </span>
        </div>
        <div class="table-scroll">
          <table class="devices">
            <thead><tr><th>Payload</th><th>Bytes</th></tr></thead>
            <tbody>
              {d.samples.map((s, i) => (
                <tr key={i}>
                  <td class="mono">{fmtHexBytes(s.hex)}</td>
                  <td class="hint">{s.len}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <div class="node-card-footer">
          <button type="button" class="btn-primary" onClick={() => onAddWrapper(d)}>
            Add wrapper
          </button>
          {' '}
          <button type="button" onClick={() => onGenerateWrapper(d)} disabled={!hasAiKey()}
                  title={hasAiKey() ? undefined : 'Set an API key in Config to use AI generation'}>
            Generate wrapper with AI
          </button>
          {wrappers.length > 0 && (
            <span>
              {' '}
              <select value={sel} onChange={(e) => setSel(e.target.value)}>
                <option value="">— existing wrapper —</option>
                {wrappers.map((w) => <option key={w.id} value={w.id}>{w.name || `wrapper ${w.id}`}</option>)}
              </select>
              {' '}
              <button type="button" disabled={!sel} onClick={() => onAssignWrapper(d, Number(sel))}>Assign</button>
            </span>
          )}
        </div>
      </div>
    </div>
  )
}

function UnknownDevicesSection({ onAddWrapper, onGenerateWrapper, wrappers, onAssignWrapper, reloadKey }) {
  const [devices, setDevices] = useState(null)
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})

  function refresh(signal) {
    return fetch('/api/v1/unknown', { signal }).then((r) => r.json()).then((d) => setDevices(d.devices || []))
  }

  useEffect(() => {
    const controller = new AbortController()
    refresh(controller.signal).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Same 10s keep-fresh cadence as the rest of this tab -- new captures
  // arrive purely from radio activity the operator didn't initiate here.
  useEffect(() => {
    const controller = new AbortController()
    const id = setInterval(() => refresh(controller.signal).catch(() => {}), 10000)
    return () => { clearInterval(id); controller.abort() }
  }, [])

  // Bumped by the tab after an assign POST -- a device leaves this list only
  // once it decodes, which the background 10s poll above would eventually
  // reflect anyway, but the operator just clicked Assign and expects the
  // card to disappear right away rather than waiting out the interval.
  useEffect(() => {
    if (!reloadKey) return
    const controller = new AbortController()
    refresh(controller.signal).catch(() => {})
    return () => controller.abort()
  }, [reloadKey])

  function toggle(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  return (
    <div class="panel">
      <h2>Unknown devices</h2>
      {error && <p class="error">Hub not reachable.</p>}
      {!error && !devices && <p class="placeholder">Loading…</p>}
      {!error && devices && devices.length === 0 && (
        <p class="placeholder">No unclaimed BLE devices captured yet — devices no wrapper matches show up here.</p>
      )}
      {!error && devices && devices.length > 0 && (
        <div class="node-cards">
          {devices.map((d) => (
            <UnknownDeviceCard key={d.id} d={d} open={!!openMap[d.id]} onToggle={() => toggle(d.id)}
                                onAddWrapper={onAddWrapper} onGenerateWrapper={onGenerateWrapper}
                                wrappers={wrappers} onAssignWrapper={onAssignWrapper} />
          ))}
        </div>
      )}
    </div>
  )
}

export function DevicesTab({ onAddWrapper, onGenerateWrapper, radioRole }) {
  const [caps, setCaps] = useState(null)
  const [devices, setDevices] = useState(null)
  const [plants, setPlants] = useState(null)
  const [wrappers, setWrappers] = useState([])
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})
  // Bumped after an assign POST to nudge UnknownDevicesSection's own list
  // to re-poll immediately, rather than waiting out its 10s interval, since
  // that's a separate component with its own local `devices` state.
  const [unknownReloadKey, setUnknownReloadKey] = useState(0)
  // The instant GET /api/v1/devices' `actions[].last_fired_s` (an AGE, not
  // an absolute time) was read -- lets ActionControl convert that age into
  // an absolute epoch second exactly once per poll, then tick a cooldown
  // countdown against the live `nowS` below between polls, rather than the
  // countdown only updating once every 10s refresh.
  const [fetchedAtS, setFetchedAtS] = useState(() => Math.floor(Date.now() / 1000))

  // Live clock for the cooldown countdown (fmtRemainingCooldown's `nowS`)
  // and the manual-command confirmation timeout -- both need to progress
  // between the 10s device-list poll below, or a countdown would visibly
  // freeze for seconds at a time.
  const [nowS, setNowS] = useState(() => Math.floor(Date.now() / 1000))
  useEffect(() => {
    const id = setInterval(() => setNowS(Math.floor(Date.now() / 1000)), 1000)
    return () => clearInterval(id)
  }, [])

  function refresh(signal) {
    const fetchedAt = Math.floor(Date.now() / 1000)
    return Promise.all([
      fetch('/api/v1/devices', { signal }).then((r) => r.json()).then((d) => {
        setDevices(d.devices)
        setFetchedAtS(fetchedAt)
      }),
      fetch('/api/v1/plants', { signal }).then((r) => r.json()).then((d) => setPlants(d.plants)),
      // Same shape wrappers.jsx's own WrappersTab already consumes
      // (GET /api/v1/wrappers -> {wrappers:[{id,name,...}]}) -- needed here
      // only for id->name lookups on the assign/unassign controls below.
      fetch('/api/v1/wrappers', { signal, headers: authHeaders() }).then((r) => r.json()).then((d) => setWrappers(d.wrappers || [])),
    ])
  }

  // POST/DELETE .../wrapper (Task 3's binding routes). Both re-poll rather
  // than update optimistically: an assign moves the device off the unknown
  // list only once it actually decodes (not guaranteed the instant the POST
  // returns), and an unassign's only visible effect (the wrapper row
  // disappearing) is exactly what the next `devices` poll already shows --
  // neither is the "must feel responsive" case onLockoutChanged's comment
  // calls out.
  async function onAssignWrapper(d, id) {
    try {
      const res = await fetch(`/api/v1/devices/${d.id}/wrapper`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ wrapper_id: id }),
      })
      if (!res.ok) {
        alert(res.status === 401 ? 'unauthorized — set the hub key in Config' : await wrapperErrText(res, 'assign failed'))
        return
      }
    } catch {
      alert('hub not reachable')
      return
    }
    refresh().catch(() => {})
    setUnknownReloadKey((k) => k + 1)
  }

  async function onUnassignWrapper(d) {
    try {
      const res = await fetch(`/api/v1/devices/${d.id}/wrapper`, {
        method: 'DELETE',
        headers: authHeaders(),
      })
      if (!res.ok) {
        alert(res.status === 401 ? 'unauthorized — set the hub key in Config' : await wrapperErrText(res, 'unassign failed'))
        return
      }
    } catch {
      alert('hub not reachable')
      return
    }
    refresh().catch(() => {})
  }

  useEffect(() => {
    loadCaps().then(setCaps).catch(() => {})
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    refresh(controller.signal).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Background keep-fresh poll -- same 10s cadence/discipline as
  // rules.jsx's own rule-list poll: live values and ages change purely from
  // radio activity the operator didn't initiate here.
  useEffect(() => {
    const controller = new AbortController()
    const id = setInterval(() => refresh(controller.signal).catch(() => {}), 10000)
    return () => { clearInterval(id); controller.abort() }
  }, [])

  function toggleDevice(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  function onRenamed(id, name) {
    setDevices((prev) => prev.map((d) => (d.id === id ? { ...d, name } : d)))
  }

  // Optimistic: PUT .../guards already confirmed the write (LockoutControl
  // only calls this on a 2xx), so reflecting it immediately here matches
  // what the very next 10s poll would show anyway -- and lockout is exactly
  // the "must feel responsive" control the brief's stop-button framing
  // cares about, not something to leave stale for up to 10s.
  function onLockoutChanged(id, lockout) {
    setDevices((prev) => prev.map((d) => (
      d.id === id ? { ...d, actions: d.actions.map((a) => ({ ...a, lockout })) } : d
    )))
  }

  if (error) return <p class="error">Hub not reachable.</p>
  if (!devices || !plants || !caps) return <p class="placeholder">Loading…</p>

  const plantNameById = new Map(plants.map((p) => [p.id, plantLabel(p)]))
  const byKind = new Map()
  for (const d of devices) {
    if (!byKind.has(d.kind)) byKind.set(d.kind, [])
    byKind.get(d.kind).push(d)
  }

  return (
    <div>
      <div class="panel">
        <h2>Devices</h2>
        {radioRole === 'wifi_only' && (
          <p class="hint">No sensor radio is enabled. Choose Bluetooth or Zigbee in Config → Radio.</p>
        )}
        {devices.length === 0 ? (
          <p class="placeholder">No devices discovered yet. MiFlora devices are discovered automatically — bring one in range.</p>
        ) : (
          KIND_ORDER.filter((k) => byKind.has(k)).map((k) => (
            <div key={k}>
              <h3>{KIND_LABEL[k] || k}</h3>
              <div class="node-cards">
                {byKind.get(k).map((d) => (
                  <DeviceCard key={d.id} d={d} caps={caps} plantNameById={plantNameById}
                              open={!!openMap[d.id]} onToggle={() => toggleDevice(d.id)} onRenamed={onRenamed}
                              nowS={nowS} fetchedAtS={fetchedAtS} onLockoutChanged={onLockoutChanged}
                              wrappers={wrappers} onUnassignWrapper={onUnassignWrapper} />
                ))}
              </div>
            </div>
          ))
        )}
      </div>
      <UnknownDevicesSection onAddWrapper={onAddWrapper} onGenerateWrapper={onGenerateWrapper}
                             wrappers={wrappers} onAssignWrapper={onAssignWrapper} reloadKey={unknownReloadKey} />
    </div>
  )
}
