import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'

/* Shown by app.jsx instead of the tab shell whenever GET /api/v1/status
 * reports role === "unset" -- i.e. a device that has never been told what
 * kind of PlantHub device it is. */
export function RoleTab({ onMainChosen }) {
  const [busy, setBusy] = useState('')          // '' | 'main' | 'node'
  const [nodePhase, setNodePhase] = useState('') // '' | 'pairing' | 'gone'
  const [error, setError] = useState('')
  const pollRef = useRef(null)

  useEffect(() => () => clearInterval(pollRef.current), [])

  async function postRole(role) {
    const res = await fetch('/api/v1/role', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', ...authHeaders() },
      body: JSON.stringify({ role }),
    })
    if (!res.ok) {
      const msg = res.status === 401 ? 'Unauthorized — set the hub key in Config first.' : 'Request failed — is the hub reachable?'
      throw new Error(msg)
    }
  }

  async function chooseMain() {
    setBusy('main'); setError('')
    try {
      await postRole('main')
      onMainChosen()
    } catch (e) {
      setError(e.message)
      setBusy('')
    }
  }

  async function chooseNode() {
    setBusy('node'); setError('')
    try {
      await postRole('node')
      setNodePhase('pairing')
      // The device leaves AP mode / reboots into radio-only node mode once
      // paired -- that is expected to show up here as status simply
      // stopping to respond, not as an error.
      pollRef.current = setInterval(() => {
        fetch('/api/v1/status').catch(() => {
          clearInterval(pollRef.current)
          setNodePhase('gone')
        })
      }, 2000)
    } catch (e) {
      setError(e.message)
      setBusy('')
    }
  }

  if (nodePhase) {
    return (
      <div class="role-screen">
        <h1>{nodePhase === 'gone' ? 'Setup network gone — that’s expected' : 'Pairing…'}</h1>
        {nodePhase === 'pairing' ? (
          <p>
            This device will leave its setup network shortly. If you haven’t
            already, open pairing on your main hub now (Nodes &rarr; Add node).
          </p>
        ) : (
          <p>
            This device has left its setup network, which is exactly what a
            paired node does. Check your main hub’s node list
            (Nodes tab) to confirm it was adopted.
          </p>
        )}
      </div>
    )
  }

  return (
    <div class="role-screen">
      <h1>What kind of PlantHub device is this?</h1>
      <div class="role-choices">
        <div class="role-card">
          <h2>Main hub</h2>
          <p>Connects to your WiFi, stores history and shows this dashboard. Choose this for your first device.</p>
          <button class="btn-primary" onClick={chooseMain} disabled={busy !== ''}>
            {busy === 'main' ? 'Setting up…' : 'Choose main hub'}
          </button>
        </div>
        <div class="role-card">
          <h2>Node</h2>
          <p>Extends BLE range. Never joins WiFi; forwards readings to your main hub over its own radio link.</p>
          <p class="hint">
            Two steps: first open pairing on your main hub (Nodes &rarr; Add node),
            <em> then</em> press Continue here.
          </p>
          <button class="btn-primary" onClick={chooseNode} disabled={busy !== ''}>
            {busy === 'node' ? 'Starting…' : 'Continue as node'}
          </button>
        </div>
      </div>
      {error && <p class="error">{error}</p>}
    </div>
  )
}
