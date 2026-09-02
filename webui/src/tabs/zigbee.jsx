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

export function ZigbeeTab() {
  const [data, setData] = useState(null)   // last GET /api/v1/zigbee response in full
  const [error, setError] = useState(false)
  const [busy, setBusy] = useState('')     // '' | permit
  const [openMap, setOpenMap] = useState({})
  const pollTimerRef = useRef(null)
  const controllerRef = useRef(null)
  // Mirrors the last-known permit_s outside React state so a poll FAILURE
  // (no fresh d.permit_s to read) can still pick the right retry cadence --
  // see poll()'s catch branch.
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
  // M6b UX: while a pairing window is open the hub's WiFi is expected to be
  // unreachable (the radio is deliberately handed to Zigbee -- see the
  // radio-role-config spec section 8). A window THIS tab opened is
  // therefore not a connection loss: windowEndRef records (client clock)
  // when that window closes, poll failures before that instant drive a
  // client-side countdown instead of the stale marker, and pairingOutage
  // swaps the alarming badge for a calm explanation. Only windows this tab
  // opened get the treatment -- a failure outside one is still a real
  // connection problem and keeps the fix-round-1 stale semantics.
  const windowEndRef = useRef(0)
  const [pairingOutage, setPairingOutage] = useState(false)

  function toggle(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  // Self-rescheduling poll (recursive setTimeout, not setInterval): the
  // cadence must flip the INSTANT a pairing window opens or closes, not
  // wait for whichever interval happens to be running. 2s while permit_s >
  // 0 (design point 1 -- a stale countdown that says 40s after the window
  // already shut is worse than no countdown, since the operator keeps
  // holding a device to a network that stopped listening), 10s otherwise,
  // matching nodes.jsx's background cadence.
  //
  // A failed fetch does not clear already-loaded data or force the
  // disabled/error screen -- only the very first load (data still null)
  // surfaces "hub not reachable"; a later transient failure just retries,
  // same discipline as nodes.jsx's refreshNodes.
  function poll() {
    fetch('/api/v1/zigbee', { signal: controllerRef.current.signal })
      .then((r) => r.json())
      .then((d) => {
        setData(d)
        setError(false)
        consecFailsRef.current = 0
        setStale(false)
        setPairingOutage(false)
        lastPermitRef.current = d.permit_s || 0
        pollTimerRef.current = setTimeout(poll, d.permit_s > 0 ? 2000 : 10000)
      })
      .catch((err) => {
        if (err.name === 'AbortError') return
        const windowRemainingS = Math.ceil((windowEndRef.current - Date.now()) / 1000)
        if (windowRemainingS > 0) {
          // Expected pairing outage: keep the countdown running off the
          // client clock (the server can't answer to run it for us) and do
          // NOT count this toward the stale threshold.
          setPairingOutage(true)
          setData((prev) => (prev ? { ...prev, permit_s: windowRemainingS } : prev))
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

  async function doPermit() {
    setBusy('permit')
    try {
      const res = await fetch('/api/v1/zigbee/permit', { method: 'POST', headers: authHeaders() })
      const body = await res.json().catch(() => ({}))
      if (res.ok && body.ok) {
        // Reflect the new window immediately instead of waiting for the
        // next scheduled tick, and restart the poll loop right now so it
        // switches onto the 2s cadence straight away.
        setData((prev) => (prev ? { ...prev, permit_s: body.permit_s } : prev))
        lastPermitRef.current = body.permit_s || 0
        windowEndRef.current = Date.now() + (body.permit_s || 0) * 1000
        // This POST just succeeded, so the hub is reachable right now --
        // clear any stale-countdown state immediately rather than waiting
        // for poll()'s own next success to do it.
        consecFailsRef.current = 0
        setStale(false)
        clearTimeout(pollTimerRef.current)
        poll()
      } else {
        alert(res.status === 401 ? 'unauthorized — wrong key' : (body.error || 'pairing failed'))
      }
    } catch {
      alert('hub not reachable')
    }
    setBusy('')
  }

  function onRenamed(id, name) {
    setData((prev) => ({ ...prev, devices: prev.devices.map((d) => (d.id === id ? { ...d, name } : d)) }))
  }

  function onRemoved(id) {
    setData((prev) => ({ ...prev, devices: prev.devices.filter((d) => d.id !== id) }))
  }

  if (error && !data) return <p class="error">Hub not reachable.</p>
  if (!data) return <p class="placeholder">Loading…</p>

  // Defensive only: app.jsx hides this tab unless the radio role is
  // zigbee, so reaching here means the coordinator failed to start.
  if (!data.enabled) {
    return (
      <div class="panel">
        <h2>Zigbee</h2>
        <p class="placeholder">Zigbee radio is not running. Check the hub log; the Radio panel in Config selects it.</p>
      </div>
    )
  }

  return (
    <div class="panel">
      <h2>Zigbee</h2>
      <div class="node-card-row">
        <span class="hint">
          {data.formed
            ? `Network formed — channel ${data.channel}, PAN 0x${data.pan_id.toString(16)}`
            : 'Network not formed yet.'}
        </span>
      </div>
      <p>
        {data.permit_s > 0 ? (
          <span class="hint">
            Pairing open — put the device into pairing mode now ({data.permit_s}s left).
            {pairingOutage && !stale && (
              <span class="hint">
                {' '}(hub briefly unreachable while pairing — this is normal)
              </span>
            )}
            {stale && (
              <span class="level-badge level-alert">
                connection lost — this countdown may be stale
              </span>
            )}
          </span>
        ) : (
          <button class="btn-primary" onClick={doPermit} disabled={busy === 'permit'}>
            {busy === 'permit' ? 'Opening…' : 'Pair a device'}
          </button>
        )}
      </p>
      {data.devices.length === 0 ? (
        <p class="placeholder">No Zigbee devices paired yet.</p>
      ) : (
        <div class="node-cards">
          {data.devices.map((d) => (
            <DeviceCard key={d.id} d={d} open={!!openMap[d.id]} onToggle={() => toggle(d.id)}
                        onRenamed={onRenamed} onRemoved={onRemoved}
                        onRetryPairing={doPermit} retryBusy={busy === 'permit'} />
          ))}
        </div>
      )}
    </div>
  )
}
