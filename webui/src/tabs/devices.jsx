import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'
import { loadCaps, capLabel, fmtCap } from '../lib/caps.js'
import { hasAiKey } from '../lib/ai/settings.js'
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
function ActionControl({ deviceId, action, fetchedAtS, nowS, switchConfirmedAtS }) {
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
        body: JSON.stringify({ param: validation.param }),
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
      <span class="mono">{action.name}</span>
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
  const switchCap = d.caps.find((c) => c.name === 'switch.state')
  const lockout = d.actions[0].lockout
  // Rendered whenever this device declares switch.on/switch.off, not only
  // once the capability has been confirmed at least once: on a brand-new
  // pairing the capability is absent from d.caps entirely (device_json.c
  // only lists a capability once e->caps[c].valid), and a silently missing
  // line there could read as "this device has no switch state" instead of
  // "not confirmed yet" -- the same ambiguity the brief's design points
  // warn against elsewhere. switchStateLabel(undefined) already renders
  // 'unknown' for exactly this case.
  const hasSwitch = switchCap || d.actions.some((a) => a.name === 'switch.on' || a.name === 'switch.off')
  // Absolute epoch second of the switch.state capability's last confirmed
  // read -- the SAME fetchedAtS-derived shape action.last_fired_s uses
  // (see ActionControl), so resolveActionSend() can compare it against a
  // baseline the same way. null when the capability has never been
  // confirmed at all (switchCap absent, or its age_s itself null) -- which
  // is exactly "this action's dispatch can never resolve past 'dispatched'"
  // for a wrapper with no confirm block, resolveActionSend()'s own
  // documented ceiling for that case.
  const switchConfirmedAtS = switchCap && switchCap.age_s != null ? fetchedAtS - switchCap.age_s : null

  return (
    <div class="actions-section">
      <div class="node-card-row">
        <span class="hint">Actuator controls</span>
        {hasSwitch && (
          <span class="hint">
            Switch: <strong>{switchStateLabel(switchCap && switchCap.value)}</strong>
            {switchCap ? ` (confirmed ${fmtAge(switchCap.age_s)})` : ' (not confirmed yet)'}
          </span>
        )}
      </div>
      <LockoutControl deviceId={d.id} firstActionName={d.actions[0].name} lockout={lockout}
                       onChanged={(newLockout) => onLockoutChanged(d.id, newLockout)} />
      {d.actions.map((a) => (
        <ActionControl key={a.id} deviceId={d.id} action={a} fetchedAtS={fetchedAtS} nowS={nowS}
                        switchConfirmedAtS={switchConfirmedAtS} />
      ))}
    </div>
  )
}

// Same collapsible-card shape as nodes.jsx's NodeCard / rules.jsx's
// RuleCard: name/id + last-seen while collapsed, details in the body.
function DeviceCard({ d, caps, plantNameById, open, onToggle, onRenamed, nowS, fetchedAtS, onLockoutChanged }) {
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

  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{d.name || d.id}</span>
          {d.name && <span class="node-card-mac mono">{d.id}</span>}
        </span>
        <span class="node-card-age hint">{fmtAge(d.last_seen_s)}</span>
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
        {d.caps.length === 0 ? (
          <p class="hint">No live capabilities yet.</p>
        ) : (
          <div class="table-scroll">
            <table class="devices">
              <thead><tr><th>Capability</th><th>Value</th><th>Age</th></tr></thead>
              <tbody>
                {d.caps.map((c) => (
                  <tr key={c.id}>
                    <td>{capLabel(caps, c.id)}</td>
                    <td>{fmtCap(caps, c.id, c.value)}</td>
                    <td class="hint">{fmtAge(c.age_s)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
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
function UnknownDeviceCard({ d, open, onToggle, onAddWrapper, onGenerateWrapper }) {
  const newest = d.samples[d.samples.length - 1]   // s[] is oldest-first, newest-last (api_v1.c's unknown_get)
  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name mono">{d.id}</span>
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
        </div>
      </div>
    </div>
  )
}

function UnknownDevicesSection({ onAddWrapper, onGenerateWrapper }) {
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
                                onAddWrapper={onAddWrapper} onGenerateWrapper={onGenerateWrapper} />
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
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})
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
    ])
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
                              nowS={nowS} fetchedAtS={fetchedAtS} onLockoutChanged={onLockoutChanged} />
                ))}
              </div>
            </div>
          ))
        )}
      </div>
      <UnknownDevicesSection onAddWrapper={onAddWrapper} onGenerateWrapper={onGenerateWrapper} />
    </div>
  )
}
