import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'

// last_seen_s is the hub's own uptime at the moment it last heard the node,
// not wall-clock -- pair it with status.uptime_s, same as the Dashboard and
// Devices tabs do for sensors.
function fmtAgo(lastSeenS, nowS) {
  if (lastSeenS == null || nowS == null) return '–'
  const d = Math.max(0, nowS - lastSeenS)
  if (d < 90) return `${d}s ago`
  if (d < 5400) return `${Math.round(d / 60)}m ago`
  return `${Math.round(d / 3600)}h ago`
}

// swarm_frame.h's OTA_ST_* enum, mirrored here for the GET .../ota "state" field
// (OTA_ST_IDLE=0, OTA_ST_RECEIVING=1 are only ever seen via prog.active, not by value).
const OTA_DONE = 2, OTA_FAILED = 3

function macPath(mac) {
  return mac.replaceAll(':', '')
}

// Per-node OTA push control: an Update button that starts the hub-side push
// (node_ota.c) and, once active, polls GET /api/v1/nodes/{mac}/ota every 2s
// for a progress bar, same discipline (AbortController + cleanup on unmount)
// as the rest of this tab's polling. Self-contained per row -- node_ota.c
// only ever runs one session hub-wide at a time (node_ota.h), so clicking
// Update on a second node while another is mid-transfer just gets a 409
// from the hub, surfaced inline below rather than tracked as separate
// cross-row state.
function OtaControl({ mac, fwVersion }) {
  const [prog, setProg] = useState(null)   // last GET .../ota response, or null before the first poll
  const [msg, setMsg] = useState('')
  const [acting, setActing] = useState(false)  // starting/aborting request in flight
  const pollRef = useRef(null)
  const controllerRef = useRef(null)

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
        if (!d.active) {
          stopPolling()
          if (d.state === OTA_DONE) setMsg('Update complete — node is rebooting onto the new firmware.')
          else if (d.state === OTA_FAILED) setMsg(`Update failed (err ${d.err}).`)
        }
      })
      .catch(() => {})
  }

  useEffect(() => stopPolling, [])

  async function start() {
    if (!confirm(
      `Push firmware${fwVersion ? ` ${fwVersion}` : ''} to this node? It will reboot once the transfer ` +
      `completes and validates. An interrupted update is safe -- the node keeps running its current ` +
      `firmware and stays paired; you can retry from here.`
    )) return
    setActing(true); setMsg('')
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
    try {
      const res = await fetch(`/api/v1/nodes/${macPath(mac)}/ota/abort`, { method: 'POST', headers: authHeaders() })
      if (!res.ok) setMsg(res.status === 401 ? 'unauthorized — set the hub key in Config' : 'abort failed')
    } catch {
      setMsg('hub not reachable')
    }
    setActing(false)
  }

  const active = prog?.active
  const pct = active && prog.total ? Math.min(100, Math.round((100 * prog.sent) / prog.total)) : 0

  return (
    <span class="ota-control">
      {!active && (
        <button onClick={start} disabled={acting}>
          {acting ? '…' : 'Update'}
        </button>
      )}
      {active && (
        <>
          <progress max="100" value={pct} />
          <span class="hint">{pct}%</span>
          <button onClick={doAbort} disabled={acting}>{acting ? '…' : 'Abort'}</button>
        </>
      )}
      {msg && <span class="error">{msg}</span>}
    </span>
  )
}

function Row({ n, nowS, fwVersion, onSaved, onForgotten }) {
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
      `Forget node ${n.name || n.mac}? The hub will notify it over the air so it re-enters pairing ` +
      `mode on its own -- but that notification is best-effort (it needs the node to be powered on ` +
      `and listening right now). If it was off, or otherwise never receives it, it will keep ` +
      `believing it is paired -- the hub silently drops its readings after radio-acking them, so it ` +
      `sees "successful" sends and never resyncs by itself. In that case, recover it by physically ` +
      `holding its BOOT button for 10 seconds to force it back into pairing mode.`
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

  return (
    <tr>
      <td>
        <form onSubmit={save} class="namef">
          <input value={name} maxlength={24} placeholder={n.mac}
                 onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
          <button type="submit" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>
      </td>
      <td class="mono">{n.mac}</td>
      <td>{n.last_seen_s != null ? fmtAgo(n.last_seen_s, nowS) : 'never'}</td>
      <td>{n.frames_rx}</td>
      <td>{n.rssi != null ? `${n.rssi} dBm` : '–'}</td>
      <td>
        <OtaControl mac={n.mac} fwVersion={fwVersion} />
      </td>
      <td>
        <button onClick={forget} disabled={forgetting}>{forgetting ? '…' : 'Forget'}</button>
      </td>
    </tr>
  )
}

export function NodesTab() {
  const [nodes, setNodes] = useState(null)
  const [total, setTotal] = useState(null)
  const [nowS, setNowS] = useState(null)
  const [fwVersion, setFwVersion] = useState(null)  // hub's own version -- what an Update push sends (node_ota.c
                                                     // always sources the hub's OWN running partition)
  const [error, setError] = useState(false)
  const [busy, setBusy] = useState('')          // '' | pair
  const [pairSecondsLeft, setPairSecondsLeft] = useState(0)
  const pairTickRef = useRef(null)
  const nodesPollRef = useRef(null)

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
        .then((s) => { setNowS(s.uptime_s); setFwVersion(s.version) }),
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
    <div>
      <h2>Nodes</h2>
      {nodes.length === 0 ? (
        <p class="placeholder">No nodes paired yet — nodes extend BLE range by relaying readings to this hub over ESP-NOW.</p>
      ) : (
        <table class="devices">
          <thead>
            <tr><th>Name</th><th>MAC</th><th>Last seen</th><th>Frames</th><th>RSSI</th><th>Firmware</th><th></th></tr>
          </thead>
          <tbody>
            {nodes.map((n) => (
              <Row key={n.mac} n={n} nowS={nowS} fwVersion={fwVersion} onSaved={onSaved} onForgotten={onForgotten} />
            ))}
          </tbody>
        </table>
      )}
      {total != null && <p class="hint">Frames received across all nodes: {total}</p>}
      {pairSecondsLeft > 0 ? (
        <p class="hint">Pairing open — put the node into pairing mode now ({pairSecondsLeft}s left).</p>
      ) : (
        <p>
          <button onClick={doAddNode} disabled={busy === 'pair'}>
            {busy === 'pair' ? 'Opening…' : 'Add node'}
          </button>
        </p>
      )}
    </div>
  )
}
