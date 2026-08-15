import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'

// last_seen_s is already an AGE in seconds, computed hub-side (swarm.c's
// swarm_node_list_json -- fix, M5c hardware round 4, defect 3: it used to be
// the hub's raw uptime at the moment it last heard the node, which this tab
// then paired with status.uptime_s the same way the Dashboard/Devices tabs
// still do for sensors -- but that field is a live snapshot from the
// /api/v1/nodes response, not a ticking clock, so no further subtraction is
// needed or correct here. It only advances on the next poll (10s background,
// 5s during a pairing window -- see NodesTab below), unlike the Dashboard's
// per-second local ticker.
function fmtAgo(ageS) {
  if (ageS == null) return '–'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

// swarm_frame.h's OTA_ST_* enum, mirrored here for the GET .../ota "state" field
// (OTA_ST_IDLE=0, OTA_ST_RECEIVING=1 are only ever seen via prog.active, not by value).
// 4 is node_ota.h's NODE_OTA_ST_PENDING_WAKE (M7) -- a hub-side-only pseudo-state for
// a battery node's push parked until its next checkin, reported like any other OTA_ST_*
// value but only ever seen together with prog.active=true (never as a terminal state).
const OTA_DONE = 2, OTA_FAILED = 3, OTA_QUEUED = 4

function macPath(mac) {
  return mac.replaceAll(':', '')
}

// swarm_store.h's SWARM_PM_* enum, mirrored here for the GET /api/v1/nodes entry's
// "reported_mode" field (the node's own last-CHECKIN-reported mode -- as opposed to
// "power_mode", which is hub-side DESIRED state and is a string, not this numeric enum).
const PM_ALWAYS_ON = 0, PM_BATTERY_15 = 1, PM_BATTERY_60 = 2

// Wire strings for "power_mode" (swarm.c's power_mode_str()), and the labels this tab
// shows for them in the selector below.
const POWER_MODE_LABELS = {
  always_on: 'always on',
  battery_15: 'battery 15 min',
  battery_60: 'battery 60 min',
}

// Final-review fix (F4): the small "beside last-seen" hint used to show the DESIRED
// mode (n.power_mode) -- but that's what the operator just ASKED for, not what the
// node is actually doing. A node still genuinely always-on, with a battery_60 change
// pending, showed "battery 60 min" there: exactly the label that makes prolonged
// silence from a now-sleeping node look normal, defeating the one thing this hint is
// for. Keyed by the numeric reported_mode (PM_*) rather than power_mode's wire
// strings, reusing the same label text above; "—" when reported_mode_valid is false
// (never checked in this boot -- see pendingHint()'s comment for why that must not be
// presented as any particular mode, reported or otherwise).
const REPORTED_MODE_LABELS = {
  [PM_ALWAYS_ON]: POWER_MODE_LABELS.always_on,
  [PM_BATTERY_15]: POWER_MODE_LABELS.battery_15,
  [PM_BATTERY_60]: POWER_MODE_LABELS.battery_60,
}

// While power_mode_pending, the node hasn't yet confirmed the desired mode via a
// CHECKIN, so it's still running whatever cadence it last reported (reported_mode) --
// that OLD cadence, not the new desired one (n.power_mode), bounds how soon the
// change can land, so the desired mode plays no part in this decision at all.
// reported_mode_valid=false (never checked in this boot) does NOT justify claiming
// any time bound (final-review fix F3): swarm_store.h's fresh-node default really is
// ALWAYS_ON, but that's the node's PERSISTED state, not necessarily what it's
// currently running -- after a hub reboot wipes this RAM-only stat, a battery_60 node
// mid-cycle is still bound by its real 60-minute cadence, and claiming "≤ a few
// minutes" for it would be a false promise. So an unconfirmed node gets no bound
// claimed at all, only the honest "next checkin" -- whenever that turns out to be.
function pendingHint(n) {
  if (!n.reported_mode_valid) {
    return "applies at the node's next checkin"
  }
  if (n.reported_mode === PM_ALWAYS_ON) {
    return "applies at the node's next checkin (≤ a few minutes)"
  }
  return n.reported_mode === PM_BATTERY_15
    ? "applies at the node's next checkin (≤ 15 min)"
    : "applies at the node's next checkin (≤ 60 min)"
}

// Per-node OTA push control: an Update button that starts the hub-side push
// (node_ota.c) and, once active, polls GET /api/v1/nodes/{mac}/ota every 2s
// for a progress bar, same discipline (AbortController + cleanup on unmount)
// as the rest of this tab's polling. Self-contained per row -- node_ota.c
// only ever runs one session hub-wide at a time (node_ota.h), so clicking
// Update on a second node while another is mid-transfer just gets a 409
// from the hub, surfaced inline below rather than tracked as separate
// cross-row state.
function OtaControl({ mac, fwVersion, onActiveChange }) {
  const [prog, setProg] = useState(null)   // last GET .../ota response, or null before the first poll
  const [msg, setMsg] = useState('')
  const [acting, setActing] = useState(false)  // starting/aborting request in flight
  const pollRef = useRef(null)
  const controllerRef = useRef(null)

  // Surfaces this node's active/inactive OTA status to NodesTab, which uses it
  // (via onOtaActive -> otaActiveMap -> isOpen) to force the card open while a
  // push is in flight -- collapsing mid-transfer would hide the progress bar.
  // Fires whenever `active` actually flips, including back to false on
  // completion/abort/mount finding nothing in flight, so a card doesn't stay
  // force-open after its OTA ends.
  useEffect(() => {
    onActiveChange?.(mac, !!prog?.active)
  }, [prog?.active])
  // Set by doAbort() right before it fires the request, so the poll loop can
  // tell "the operator clicked Abort" apart from any other OTA_ST_FAILED
  // (the err byte alone can't: node_ota.h's node_ota_progress_t.err is either
  // a hub-detected NODE_OTA_ERR_* code or the node's own OTA_STATUS.err byte
  // passed through verbatim, and the two numberspaces aren't disambiguated on
  // the wire -- see that struct's comment). Reset whenever a fresh push starts.
  const userAbortedRef = useRef(false)

  function stopPolling() {
    clearInterval(pollRef.current)
    pollRef.current = null
    controllerRef.current?.abort()
    controllerRef.current = null
  }

  function poll() {
    const controller = new AbortController()
    controllerRef.current = controller
    fetch(`/api/v1/nodes/${macPath(mac)}/ota`, { signal: controller.signal })
      .then((r) => r.json())
      .then((d) => {
        setProg(d)
        if (d.active) {
          // Covers both the interval below and the resume-on-mount poll:
          // once we learn a session is active, make sure it's ticking.
          if (!pollRef.current) pollRef.current = setInterval(poll, 2000)
        } else {
          stopPolling()
          if (d.state === OTA_DONE) setMsg('Update complete — node is rebooting onto the new firmware.')
          else if (d.state === OTA_FAILED) {
            setMsg(userAbortedRef.current ? 'Update cancelled.' : `Update failed (err ${d.err}).`)
          }
        }
      })
      .catch(() => {})
  }

  // Poll once on mount: if a push is already in flight (e.g. the operator
  // reloaded the page mid-transfer, or another tab/session started one),
  // this resumes the progress bar instead of showing a plain Update button
  // that would then 409 on click. Deliberately does not set `msg` for a
  // terminal state found on mount (DONE/FAILED) -- that reflects whatever
  // the LAST session on this node was, possibly long over, and would be
  // misleading to surface as if it just happened.
  useEffect(() => {
    const controller = new AbortController()
    controllerRef.current = controller
    fetch(`/api/v1/nodes/${macPath(mac)}/ota`, { signal: controller.signal })
      .then((r) => r.json())
      .then((d) => {
        if (d.active) {
          setProg(d)
          pollRef.current = setInterval(poll, 2000)
        }
      })
      .catch(() => {})
    return stopPolling
  }, [])

  async function start() {
    if (!confirm(
      `Push firmware${fwVersion ? ` ${fwVersion}` : ''} to this node? It will reboot once the transfer ` +
      `completes and validates. An interrupted update is safe -- the node keeps running its current ` +
      `firmware and stays paired; you can retry from here.`
    )) return
    setActing(true); setMsg('')
    userAbortedRef.current = false
    try {
      const res = await fetch(`/api/v1/nodes/${macPath(mac)}/ota`, { method: 'POST', headers: authHeaders() })
      if (res.ok) {
        setMsg('')
        stopPolling()
        poll()
        pollRef.current = setInterval(poll, 2000)
      } else if (res.status === 401) {
        setMsg('unauthorized — set the hub key in Config')
      } else if (res.status === 409) {
        setMsg('an update is already in progress (on this or another node)')
      } else {
        setMsg('failed to start update')
      }
    } catch {
      setMsg('hub not reachable')
    }
    setActing(false)
  }

  async function doAbort() {
    setActing(true)
    userAbortedRef.current = true
    try {
      const res = await fetch(`/api/v1/nodes/${macPath(mac)}/ota/abort`, { method: 'POST', headers: authHeaders() })
      if (!res.ok) {
        userAbortedRef.current = false
        setMsg(res.status === 401 ? 'unauthorized — set the hub key in Config' : 'abort failed')
      }
    } catch {
      userAbortedRef.current = false
      setMsg('hub not reachable')
    }
    setActing(false)
  }

  const active = prog?.active
  // Queued (state 4): the push is parked hub-side waiting for the node's next wake,
  // so prog.sent/total aren't moving yet -- a 0% bar there would read as stalled
  // rather than as "hasn't started because the node is asleep".
  const queued = active && prog.state === OTA_QUEUED
  const pct = active && prog.total ? Math.min(100, Math.round((100 * prog.sent) / prog.total)) : 0

  return (
    <span class="ota-control">
      {!active && (
        <button class="btn-primary" onClick={start} disabled={acting}>
          {acting ? '…' : 'Update'}
        </button>
      )}
      {active && queued && (
        <span class="hint">queued — starts at the node's next wake</span>
      )}
      {active && !queued && (
        <>
          <progress max="100" value={pct} />
          <span class="hint">{pct}%</span>
        </>
      )}
      {active && <button class="btn-destructive" onClick={doAbort} disabled={acting}>{acting ? '…' : 'Abort'}</button>}
      {msg && <span class="error">{msg}</span>}
    </span>
  )
}

// Per-node power-mode control: a three-option select that POSTs
// {"power_mode": ...} ALONE on change (per Task 6's contract -- resending name
// isn't needed and isn't sent). Controlled straight off `n.power_mode` rather
// than buffered local state (contrast NodeCard's `name` field, which has an
// explicit Save button): there's no separate save step here, the select's
// onChange IS the save, so the parent's node list is the only source of truth.
function PowerModeControl({ n, onSaved }) {
  const [state, setState] = useState('idle') // idle | saving | error | unauth

  async function onChange(e) {
    const mode = e.currentTarget.value
    setState('saving')
    try {
      const res = await fetch(`/api/v1/nodes/${macPath(n.mac)}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ power_mode: mode }),
      })
      if (res.ok) { setState('idle'); onSaved(n.mac, mode) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <span class="power-mode-control">
      <select value={n.power_mode} disabled={state === 'saving'} onChange={onChange}>
        <option value="always_on">{POWER_MODE_LABELS.always_on}</option>
        <option value="battery_15">{POWER_MODE_LABELS.battery_15}</option>
        <option value="battery_60">{POWER_MODE_LABELS.battery_60}</option>
      </select>
      {n.power_mode_pending && <span class="hint">{pendingHint(n)}</span>}
      {state === 'error' && <span class="error">failed</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </span>
  )
}

// Collapsible per-node card. `open`/`onToggle` are lifted to NodesTab (keyed by
// mac) rather than kept as local state here, so a node whose OTA goes active
// can be forced open from the parent without this component needing to know
// why -- see NodesTab's `isOpen`, which ORs the operator's own toggle state
// with the OTA poll's `active` flag (reported back up via onOtaActive below).
function NodeCard({ n, fwVersion, open, onToggle, onSaved, onForgotten, onPowerModeSaved, onOtaActive }) {
  const [name, setName] = useState(n.name || '')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth
  const [forgetting, setForgetting] = useState(false)

  async function save(e) {
    e.preventDefault()
    setState('saving')
    try {
      const res = await fetch(`/api/v1/nodes/${n.mac.replaceAll(':', '')}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ name }),
      })
      if (res.ok) { setState('saved'); onSaved(n.mac, name) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  async function forget() {
    if (!confirm(
      `Forget node ${n.name || n.mac}? The hub notifies it over the air so it re-enters pairing ` +
      `mode on its own — but that's best-effort (the node must be powered on and listening right ` +
      `now). If it never gets the message, force it back into pairing mode yourself: unplug/replug ` +
      `power 5 times quickly, or hold its BOOT button for 10 seconds.`
    )) return
    setForgetting(true)
    try {
      const res = await fetch(`/api/v1/nodes/${n.mac.replaceAll(':', '')}`, {
        method: 'DELETE',
        headers: authHeaders(),
      })
      if (res.ok) { onForgotten(n.mac); return }
      alert(res.status === 401 ? 'unauthorized — set the hub key in Config' : 'forget failed')
    } catch {
      alert('hub not reachable')
    }
    setForgetting(false)
  }

  // `open` here is already the OR of the lifted per-mac toggle state and this
  // node's own OTA-active flag (see NodesTab's `isOpen`) -- this component
  // doesn't need to know which one is responsible, only whether to show the
  // body. The body itself stays mounted at all times rather than being
  // conditionally rendered on `open` (only its visibility is toggled, via the
  // `.node-card.open` CSS below): OtaControl's poll loop must keep running
  // while collapsed so a push that goes active can report that back up
  // through onActiveChange and force the card open in the first place.
  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{n.name || n.mac}</span>
          {n.name && <span class="node-card-mac mono">{n.mac}</span>}
        </span>
        <span class="node-card-age hint">{n.last_seen_s != null ? fmtAgo(n.last_seen_s) : 'never'}</span>
      </button>
      <div class="node-card-body">
        <form onSubmit={save} class="namef">
          <input value={name} maxlength={24} placeholder={n.mac}
                 onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
          <button type="submit" class="btn-primary" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>
        <div class="node-card-row">
          <PowerModeControl n={n} onSaved={onPowerModeSaved} />
          <span class="hint">reported: {n.reported_mode_valid ? (REPORTED_MODE_LABELS[n.reported_mode] || n.reported_mode) : '—'}</span>
        </div>
        <div class="node-card-row">
          <span class="hint">{n.rssi != null ? `${n.rssi} dBm` : '–'}</span>
          <span class="hint">{n.frames_rx} frames</span>
        </div>
        <div class="node-card-row">
          <OtaControl mac={n.mac} fwVersion={fwVersion} onActiveChange={onOtaActive} />
        </div>
        <div class="node-card-footer">
          <button class="btn-destructive" onClick={forget} disabled={forgetting}>{forgetting ? '…' : 'Forget'}</button>
        </div>
      </div>
    </div>
  )
}

export function NodesTab() {
  const [nodes, setNodes] = useState(null)
  const [total, setTotal] = useState(null)
  const [fwVersion, setFwVersion] = useState(null)  // hub's own version -- what an Update push sends (node_ota.c
                                                     // always sources the hub's OWN running partition)
  const [error, setError] = useState(false)
  const [busy, setBusy] = useState('')          // '' | pair
  const [pairSecondsLeft, setPairSecondsLeft] = useState(0)
  const pairTickRef = useRef(null)
  const nodesPollRef = useRef(null)

  // Per-mac collapsed/expanded state for the node cards below, plus each
  // node's own OTA-active flag (reported by its OtaControl via
  // onOtaActive). Neither persists across a reload -- both default to
  // "false"/absent, i.e. collapsed and idle, for any mac not yet a key. A
  // card is actually shown open when EITHER is true (see `isOpen`), so an
  // active push forces its card open regardless of what the operator last
  // clicked, without this state needing to track why.
  const [openMap, setOpenMap] = useState({})
  const [otaActiveMap, setOtaActiveMap] = useState({})

  function toggleNode(mac) {
    setOpenMap((prev) => ({ ...prev, [mac]: !prev[mac] }))
  }

  function onOtaActive(mac, active) {
    setOtaActiveMap((prev) => (prev[mac] === active ? prev : { ...prev, [mac]: active }))
  }

  function isOpen(mac) {
    return !!openMap[mac] || !!otaActiveMap[mac]
  }

  // Called both awaited (initial load, via Promise.all below, which still
  // needs to see a real failure to set the error state) and fire-and-forget
  // (the pairing countdown and 5s node-list poll, neither of which has
  // anywhere to send a rejection) -- the trailing catch is silent so a
  // transient failure during the pairing window (exactly when the hub is
  // busiest) never surfaces as an unhandled rejection from those uncaught
  // call sites. AbortError (unmount) is swallowed here too, same as
  // app.jsx's refreshRole(); the initial-load effect's own catch below only
  // ever sees the /api/v1/status half now, and still filters AbortError the
  // same way.
  function refreshNodes(signal) {
    return fetch('/api/v1/nodes', { signal })
      .then((r) => r.json())
      .then((d) => { setNodes(d.nodes || []); setTotal(d.frames_rx_total ?? null) })
      .catch(() => {})
  }

  useEffect(() => {
    const controller = new AbortController()
    Promise.all([
      refreshNodes(controller.signal),
      fetch('/api/v1/status', { signal: controller.signal })
        .then((r) => r.json())
        .then((s) => { setFwVersion(s.version) }),
    ]).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Background keep-fresh poll, independent of the pairing window's own
  // faster 5s poll (nodesPollRef, started only while a pairing countdown is
  // active -- see doAddNode). Without this, the tab fetched /api/v1/nodes
  // exactly once at mount and then never again outside a pairing window, so
  // "Last seen"/"frames_rx" went stale the moment an operator just left the
  // tab open. 10s is plenty for a background tick nobody is actively
  // watching count down (contrast the pairing poll's 5s, sized for a human
  // watching a countdown). Own AbortController + cleanup, same discipline as
  // the mount effect above: aborts any in-flight fetch on unmount so a late
  // response never calls setState on an unmounted component.
  useEffect(() => {
    const controller = new AbortController()
    const id = setInterval(() => refreshNodes(controller.signal), 10000)
    return () => { clearInterval(id); controller.abort() }
  }, [])

  // Pairing-window timers only, not the initial-load AbortControllers above --
  // these belong to the "Add node" flow and must not leak across unmounts.
  useEffect(() => () => {
    clearInterval(pairTickRef.current)
    clearInterval(nodesPollRef.current)
  }, [])

  function onSaved(mac, name) {
    setNodes((prev) => prev.map((n) => (n.mac === mac ? { ...n, name } : n)))
  }

  function onForgotten(mac) {
    setNodes((prev) => prev.filter((n) => n.mac !== mac))
  }

  // Optimistically flips power_mode_pending to true: right after a successful
  // POST, the node's last-reported mode almost certainly still differs from
  // the newly-desired one (matching swarm.c's own pending computation), so
  // this avoids a dead window showing neither the old nor the pending state
  // until the next 10s background poll (refreshNodes) catches up. reported_mode/
  // reported_mode_valid are left untouched -- those only ever change via a real
  // CHECKIN, never via this operator-initiated POST.
  function onPowerModeSaved(mac, power_mode) {
    setNodes((prev) => prev.map((n) => (n.mac === mac ? { ...n, power_mode, power_mode_pending: true } : n)))
  }

  async function doAddNode() {
    setBusy('pair')
    try {
      const res = await fetch('/api/v1/nodes/pair', { method: 'POST', headers: authHeaders() })
      const data = await res.json().catch(() => ({}))
      if (res.ok && data.ok) {
        const seconds = data.window_s || 120
        setPairSecondsLeft(seconds)
        clearInterval(pairTickRef.current)
        pairTickRef.current = setInterval(() => {
          setPairSecondsLeft((s) => {
            if (s <= 1) {
              clearInterval(pairTickRef.current)
              clearInterval(nodesPollRef.current)
              refreshNodes()
              return 0
            }
            return s - 1
          })
        }, 1000)
        refreshNodes()
        clearInterval(nodesPollRef.current)
        nodesPollRef.current = setInterval(refreshNodes, 5000)
      } else {
        alert(res.status === 401 ? 'unauthorized — wrong key' : (data.error || 'pairing failed'))
      }
    } catch { alert('hub not reachable') }
    setBusy('')
  }

  if (error) return <p class="error">Hub not reachable.</p>
  if (!nodes) return <p class="placeholder">Loading…</p>

  return (
    <div class="panel">
      <h2>Nodes</h2>
      {nodes.length === 0 ? (
        <p class="placeholder">No nodes paired yet — nodes extend BLE range by relaying readings to this hub over ESP-NOW.</p>
      ) : (
        <div class="node-cards">
          {nodes.map((n) => (
            <NodeCard key={n.mac} n={n} fwVersion={fwVersion} open={isOpen(n.mac)}
                      onToggle={() => toggleNode(n.mac)} onSaved={onSaved} onForgotten={onForgotten}
                      onPowerModeSaved={onPowerModeSaved} onOtaActive={onOtaActive} />
          ))}
        </div>
      )}
      {total != null && <p class="hint">Frames received across all nodes: {total}</p>}
      {pairSecondsLeft > 0 ? (
        <p class="hint">Pairing open — put the node into pairing mode now ({pairSecondsLeft}s left).</p>
      ) : (
        <p>
          <button class="btn-primary" onClick={doAddNode} disabled={busy === 'pair'}>
            {busy === 'pair' ? 'Opening…' : 'Add node'}
          </button>
        </p>
      )}
    </div>
  )
}
