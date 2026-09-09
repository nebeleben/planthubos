import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'

// Consecutive poll(GET /api/v1/zigbee) failures before the on-screen
// countdown stops being presented as authoritative -- see the `stale`
// state in ZigbeeTab. One dropped request at the 2s in-window cadence is
// noise; three in a row (~6s of silence) is a real outage worth flagging.
const STALE_AFTER_FAILURES = 3

// One joined device's row. `caps`/`actions` here are already the mapped
// NAME strings GET /api/v1/zigbee's device objects carry (unlike the
// Devices tab's numeric capability ids) -- nothing to look up against the
// capability table, just render them.
//
// interviewed=false renders as its own visible state rather than being
// hidden or treated as an error (Task 10 brief, design point 2): a device
// that joined the network but never got mapped is only recoverable by
// factory-reset if the operator can't even see it's there. `clusters` is
// shown for it either way -- currently ALWAYS the mapped subset (a known
// gap tracked separately, not something to work around here), so an
// unmappable device may still show an empty list; that's the server's
// honesty gap to close, not this tab's.
function DeviceCard({ d, open, onToggle, onRenamed, onRemoved, onRetryPairing, retryBusy }) {
  const [name, setName] = useState(d.name || '')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth
  const [removing, setRemoving] = useState(false)

  async function save(e) {
    e.preventDefault()
    setState('saving')
    try {
      const res = await fetch(`/api/v1/zigbee/devices/${d.id}`, {
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

  async function remove() {
    if (!confirm(`Remove ${d.name || d.id} from the Zigbee network? This does not reset the device -- it will show up again if it rejoins.`)) return
    setRemoving(true)
    try {
      const res = await fetch(`/api/v1/zigbee/devices/${d.id}`, { method: 'DELETE', headers: authHeaders() })
      if (res.ok) { onRemoved(d.id); return }
      alert(res.status === 401 ? 'unauthorized — set the hub key in Config' : 'remove failed')
    } catch {
      alert('hub not reachable')
    }
    setRemoving(false)
  }

  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{d.name || d.id}</span>
          {d.name && <span class="node-card-mac mono">{d.id}</span>}
        </span>
        {!d.interviewed && <span class="level-badge level-notify">joined, not interviewed</span>}
      </button>
      <div class="node-card-body">
        <form onSubmit={save} class="namef">
          <input value={name} maxlength={32} placeholder={d.id}
                 onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
          <button type="submit" class="btn-primary" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>
        <div class="node-card-row">
          <span class="hint">short addr <span class="mono">{d.short_addr}</span> · endpoint {d.endpoint}</span>
        </div>
        {d.interviewed ? (
          <>
            <div class="node-card-row">
              <span class="hint">capabilities:</span>{' '}
              {d.caps && d.caps.length > 0
                ? <span class="mono">{d.caps.join(', ')}</span>
                : <span class="hint">none mapped</span>}
            </div>
            <div class="node-card-row">
              <span class="hint">actions:</span>{' '}
              {d.actions && d.actions.length > 0
                ? <span class="mono">{d.actions.join(', ')}</span>
                : <span class="hint">none mapped</span>}
            </div>
          </>
        ) : (
          // No dedicated retry-interview endpoint exists (Task 9's surface
          // is permit/rename/delete only) -- reopening the permit-join
          // window is the actual retry path, since the coordinator
          // re-attempts the interview when a joined-but-uninterviewed
          // device is next seen inside an open window.
          <div class="node-card-row">
            <span class="hint">
              Joined the network but the interview hasn't completed, so no capabilities or actions
              are known yet. Reopen the pairing window and power-cycle the device to retry, or
              remove it below.
            </span>
          </div>
        )}
        {!d.interviewed && (
          <div class="node-card-row">
            <button type="button" class="btn-secondary" onClick={onRetryPairing} disabled={retryBusy}>
              {retryBusy ? '…' : 'Reopen pairing window'}
            </button>
          </div>
        )}
        <div class="node-card-row">
          <span class="hint">clusters:</span>{' '}
          {d.clusters && d.clusters.length > 0
            ? <span class="mono">{d.clusters.join(', ')}</span>
            : <span class="hint">none reported</span>}
        </div>
        <div class="node-card-footer">
          <button class="btn-destructive" onClick={remove} disabled={removing}>{removing ? '…' : 'Remove'}</button>
        </div>
      </div>
    </div>
  )
}

// Task 9's coordinator key: null for the hub's own local coordinator, the
// bridge node's mac12 otherwise. Used both as the poll/permit request body
// value and as this tab's per-card React key / busy-tracking key (with
// 'local' standing in for null wherever a plain string key is needed, e.g.
// object property names and React `key`s, which can't be null).
function keyOf(node) {
  return node ?? 'local'
}

// One coordinator's card -- the local hub radio (c.node === null) or one
// Zigbee-bridge node (c.node === mac12). Same body shape either way
// (network line, permit control, device list); only the pairing-outage
// countdown/stale-connection treatment is local-only, since only the
// local coordinator shares an antenna with the hub's own WiFi (a bridge's
// permit window is just a queued ESP-NOW command -- see radio-architecture
// findings). `busyKey` is the tab-wide "which card's permit POST is in
// flight" tracker; this card is busy iff busyKey === keyOf(c.node).
function CoordinatorCard({ c, busyKey, onPermit, onRenamed, onRemoved, stale, pairingOutage }) {
  const [openMap, setOpenMap] = useState({})
  const isLocal = c.node === null
  const busy = busyKey === keyOf(c.node)
  const label = c.name || c.node

  function toggle(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  // Defensive only: app.jsx/ZigbeeTab gate on radioRole/hasBridges, so a
  // local entry with enabled=false shouldn't normally reach here -- but if
  // it does (coordinator failed to start this boot), say so plainly rather
  // than rendering a permit button that can only 409.
  if (isLocal && !c.enabled) {
    return (
      <div class="panel">
        <h2>Zigbee — this hub</h2>
        <p class="placeholder">Zigbee radio is not running. Check the hub log; the Radio panel in Config selects it.</p>
      </div>
    )
  }

  return (
    <div class="panel">
      <h2>{isLocal ? 'Zigbee — this hub' : `Zigbee bridge — ${label}`}</h2>
      {c.reported_role !== 'zigbee' && (
        <p class="hint">bridge reports role {c.reported_role}, waiting for reboot</p>
      )}
      <div class="node-card-row">
        <span class="hint">
          {c.formed
            ? `Network formed — channel ${c.channel}, PAN 0x${c.pan_id.toString(16)}`
            : 'Network not formed yet.'}
        </span>
      </div>
      <p>
        {c.permit_s > 0 ? (
          <span class="hint">
            {isLocal
              ? `Pairing open — put the device into pairing mode now (${c.permit_s}s left).`
              : `Pairing open on ${label} — ${c.permit_s}s left`}
            {isLocal && pairingOutage && !stale && (
              <span class="hint">
                {' '}(hub briefly unreachable while pairing — this is normal)
              </span>
            )}
            {isLocal && stale && (
              <span class="level-badge level-alert">
                connection lost — this countdown may be stale
              </span>
            )}
          </span>
        ) : (
          <button class="btn-primary" onClick={() => onPermit(c.node)} disabled={busy}>
            {busy ? 'Opening…' : isLocal ? 'Pair a device' : `Pair a device on ${label}`}
          </button>
        )}
      </p>
      {c.devices.length === 0 ? (
        <p class="placeholder">No Zigbee devices paired yet.</p>
      ) : (
        <div class="node-cards">
          {c.devices.map((d) => (
            <DeviceCard key={d.id} d={d} open={!!openMap[d.id]} onToggle={() => toggle(d.id)}
                        onRenamed={(id, name) => onRenamed(c.node, id, name)}
                        onRemoved={(id) => onRemoved(c.node, id)}
                        onRetryPairing={() => onPermit(c.node)} retryBusy={busy} />
          ))}
        </div>
      )}
    </div>
  )
}

export function ZigbeeTab() {
  const [data, setData] = useState(null)   // last GET /api/v1/zigbee response's `coordinators` array
  const [error, setError] = useState(false)
  const [busyKey, setBusyKey] = useState('')  // '' | keyOf(node) of the card whose permit POST is in flight
  const pollTimerRef = useRef(null)
  const controllerRef = useRef(null)
  // Mirrors the LOCAL coordinator's last-known permit_s outside React state
  // so a poll FAILURE (no fresh data to read) can still pick the right
  // retry cadence -- see poll()'s catch branch. Only the local coordinator
  // matters here: it's the only one whose pairing window can make the hub
  // itself briefly unreachable (see windowEndRef below).
  const lastPermitRef = useRef(0)
  // Fix round 1: a poll failure used to leave `data` (and the countdown it
  // drives) untouched and silently retry -- fine for one dropped request at
  // a 2s cadence (noise), but if the hub drops out mid-window the tab kept
  // showing whatever permit_s it last saw, forever, with nothing marking it
  // stale. That is exactly the "confident lie" design point 1 warns
  // against: the operator keeps holding a device up to a window that may
  // already be closed. consecFailsRef counts consecutive failures (reset to
  // 0 on any success); once it reaches STALE_AFTER_FAILURES, `stale` flips
  // true and the countdown renders with a visible connection-lost marker
  // instead of pretending to still be authoritative. Polling itself keeps
  // going at the same cadence throughout -- this only changes what's shown.
  const consecFailsRef = useRef(0)
  const [stale, setStale] = useState(false)
  // M6b UX: while a pairing window is open on the LOCAL coordinator, the
  // hub's WiFi is expected to be unreachable (the radio is deliberately
  // handed to Zigbee -- see the radio-role-config spec section 8). A window
  // THIS tab opened is therefore not a connection loss: windowEndRef
  // records (client clock) when that window closes, poll failures before
  // that instant drive a client-side countdown instead of the stale
  // marker, and pairingOutage swaps the alarming badge for a calm
  // explanation. Only windows this tab opened on the LOCAL coordinator get
  // the treatment -- a bridge's permit window never blocks the hub's own
  // WiFi, and a failure outside a local window is still a real connection
  // problem and keeps the fix-round-1 stale semantics.
  const windowEndRef = useRef(0)
  const [pairingOutage, setPairingOutage] = useState(false)

  // Self-rescheduling poll (recursive setTimeout, not setInterval): the
  // cadence must flip the INSTANT any pairing window opens or closes, not
  // wait for whichever interval happens to be running. 2s while ANY
  // coordinator's permit_s > 0 (design point 1 -- a stale countdown that
  // says 40s after the window already shut is worse than no countdown,
  // since the operator keeps holding a device to a network that stopped
  // listening), 10s otherwise, matching nodes.jsx's background cadence.
  //
  // A failed fetch does not clear already-loaded data or force the
  // disabled/error screen -- only the very first load (data still null)
  // surfaces "hub not reachable"; a later transient failure just retries,
  // same discipline as nodes.jsx's refreshNodes.
  function poll() {
    fetch('/api/v1/zigbee', { signal: controllerRef.current.signal })
      .then((r) => r.json())
      .then((d) => {
        const coordinators = d.coordinators || []
        setData(coordinators)
        setError(false)
        consecFailsRef.current = 0
        setStale(false)
        setPairingOutage(false)
        const local = coordinators.find((c) => c.node === null)
        lastPermitRef.current = local ? local.permit_s || 0 : 0
        const anyOpen = coordinators.some((c) => c.permit_s > 0)
        pollTimerRef.current = setTimeout(poll, anyOpen ? 2000 : 10000)
      })
      .catch((err) => {
        if (err.name === 'AbortError') return
        const windowRemainingS = Math.ceil((windowEndRef.current - Date.now()) / 1000)
        if (windowRemainingS > 0) {
          // Expected pairing outage (local coordinator only): keep the
          // countdown running off the client clock (the server can't
          // answer to run it for us) and do NOT count this toward the
          // stale threshold.
          setPairingOutage(true)
          setData((prev) => (prev
            ? prev.map((c) => (c.node === null ? { ...c, permit_s: windowRemainingS } : c))
            : prev))
          lastPermitRef.current = windowRemainingS
          pollTimerRef.current = setTimeout(poll, 2000)
          return
        }
        consecFailsRef.current += 1
        if (consecFailsRef.current >= STALE_AFTER_FAILURES) setStale(true)
        setData((prev) => {
          if (prev == null) setError(true)
          return prev
        })
        pollTimerRef.current = setTimeout(poll, lastPermitRef.current > 0 ? 2000 : 10000)
      })
  }

  useEffect(() => {
    controllerRef.current = new AbortController()
    poll()
    return () => {
      controllerRef.current.abort()
      clearTimeout(pollTimerRef.current)
    }
  }, [])

  // node: null for the local coordinator, mac12 for a bridge -- Task 9's
  // POST /api/v1/zigbee/permit body shape. The response differs (local:
  // {ok,permit_s}; bridge: {ok,queued,node}), so only the local branch can
  // reflect the new window immediately client-side; a bridge's window
  // shows up once the hub hears back over ESP-NOW and the next poll picks
  // it up -- polling is restarted at the fast cadence right away either
  // way so that's at most 2s off.
  async function doPermit(node) {
    setBusyKey(keyOf(node))
    try {
      const res = await fetch('/api/v1/zigbee/permit', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ node }),
      })
      const body = await res.json().catch(() => ({}))
      if (res.ok && body.ok) {
        if (node === null) {
          setData((prev) => (prev
            ? prev.map((c) => (c.node === null ? { ...c, permit_s: body.permit_s } : c))
            : prev))
          lastPermitRef.current = body.permit_s || 0
          windowEndRef.current = Date.now() + (body.permit_s || 0) * 1000
          // This POST just succeeded, so the hub is reachable right now --
          // clear any stale-countdown state immediately rather than
          // waiting for poll()'s own next success to do it.
          consecFailsRef.current = 0
          setStale(false)
        }
        clearTimeout(pollTimerRef.current)
        poll()
      } else {
        alert(res.status === 401 ? 'unauthorized — wrong key' : (body.error || 'pairing failed'))
      }
    } catch {
      alert('hub not reachable')
    }
    setBusyKey('')
  }

  function onRenamed(node, id, name) {
    setData((prev) => prev.map((c) => (c.node === node
      ? { ...c, devices: c.devices.map((d) => (d.id === id ? { ...d, name } : d)) }
      : c)))
  }

  function onRemoved(node, id) {
    setData((prev) => prev.map((c) => (c.node === node
      ? { ...c, devices: c.devices.filter((d) => d.id !== id) }
      : c)))
  }

  if (error && !data) return <p class="error">Hub not reachable.</p>
  if (!data) return <p class="placeholder">Loading…</p>

  if (data.length === 0) {
    return (
      <div class="panel">
        <h2>Zigbee</h2>
        <p class="placeholder">
          No Zigbee coordinator: switch this hub's radio to Zigbee in Config, or make a node a
          Zigbee bridge in Nodes.
        </p>
      </div>
    )
  }

  return (
    <>
      {data.map((c) => (
        <CoordinatorCard key={keyOf(c.node)} c={c} busyKey={busyKey} onPermit={doPermit}
                          onRenamed={onRenamed} onRemoved={onRemoved}
                          stale={stale} pairingOutage={pairingOutage} />
      ))}
    </>
  )
}
