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

function Row({ n, nowS, onSaved, onForgotten }) {
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
        <button onClick={forget} disabled={forgetting}>{forgetting ? '…' : 'Forget'}</button>
      </td>
    </tr>
  )
}

export function NodesTab() {
  const [nodes, setNodes] = useState(null)
  const [total, setTotal] = useState(null)
  const [nowS, setNowS] = useState(null)
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
        .then((s) => setNowS(s.uptime_s)),
    ]).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Pairing-window timers only, not the initial-load AbortController above --
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
            <tr><th>Name</th><th>MAC</th><th>Last seen</th><th>Frames</th><th>RSSI</th><th></th></tr>
          </thead>
          <tbody>
            {nodes.map((n) => (
              <Row key={n.mac} n={n} nowS={nowS} onSaved={onSaved} onForgotten={onForgotten} />
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
