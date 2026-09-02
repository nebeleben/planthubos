import { useState } from 'preact/hooks'
import { getKey, setKey, authHeaders } from '../lib/auth.js'
import { rebootCountdown } from '../lib/reboot.js'

// Second onboarding screen (after role.jsx's main/node choice): shown by
// app.jsx while status.radio_role_set is false on a main hub. A hub runs
// ONE sensor radio -- BLE and Zigbee cannot share its antenna (radio-
// architecture findings) -- so this is a real choice, not a checklist.
// Posts the same {radio_role} the Settings panel does; the hub reboots to
// apply, except when the chosen role is already running (a fresh hub
// picking WiFi only), where it answers rebooting:false and we hand over
// to the tab shell right away.
const CHOICES = [
  { id: 'wifi_only', title: 'WiFi only',
    body: 'No sensor radio. For a hub that only serves this UI, runs rules over swarm nodes, or feeds integrations.' },
  { id: 'ble', title: 'Bluetooth (BLE)',
    body: 'MiFlora, BTHome and other Bluetooth sensors. The classic PlantHub.' },
  { id: 'zigbee', title: 'Zigbee',
    body: '802.15.4 sensors and plugs through the built-in Zigbee coordinator.' },
]

export function RadioTab({ onChosen }) {
  const [busy, setBusy] = useState('')     // '' | choice id in flight
  const [msg, setMsg] = useState('')
  const [error, setError] = useState('')
  // A claimed hub 401s the picker's POST just like it 401s Config's --
  // this form is that same recovery, in place, so the user can retry the
  // same card instead of being sent away to a Config tab they may not
  // even have a route to yet.
  const [needKey, setNeedKey] = useState(false)
  const [keyInput, setKeyInput] = useState(getKey())

  async function choose(id) {
    setBusy(id); setError(''); setNeedKey(false)
    try {
      const res = await fetch('/api/v1/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ radio_role: id }),
      })
      if (!res.ok) {
        if (res.status === 401) {
          setNeedKey(true)
          throw new Error('This hub is claimed — enter its key to continue.')
        }
        throw new Error('Request failed — is the hub reachable?')
      }
      let d
      try { d = await res.json() } catch { throw new Error('Request failed — is the hub reachable?') }
      if (d.rebooting) {
        rebootCountdown(setMsg)
      } else {
        onChosen()
        setBusy('')
      }
    } catch (e) {
      setError(e.message)
      setBusy('')
    }
  }

  function saveKey() {
    setKey(keyInput)
    setNeedKey(false)
  }

  if (msg) {
    return (
      <div class="role-screen">
        <h1>Applying radio choice…</h1>
        <p>{msg}</p>
      </div>
    )
  }

  return (
    <div class="role-screen">
      <h1>Which radio should this hub use?</h1>
      <div class="role-choices">
        {CHOICES.map((c) => (
          <div class="role-card" key={c.id}>
            <h2>{c.title}</h2>
            <p>{c.body}</p>
            <button class="btn-primary" onClick={() => choose(c.id)} disabled={busy !== ''}>
              {busy === c.id ? 'Saving…' : `Use ${c.title}`}
            </button>
          </div>
        ))}
      </div>
      <p class="hint">
        A hub runs one of these radios at a time. You can change it later in Config; changing it reboots the hub.
      </p>
      {error && <p class="error">{error}</p>}
      {needKey && (
        <div>
          <label class="keyrow">
            Hub key
            <input type="password" value={keyInput}
                   onInput={(e) => setKeyInput(e.currentTarget.value)}
                   placeholder="paste the 64-char key" />
          </label>
          <button class="btn-primary" onClick={saveKey}>Save key</button>
        </div>
      )}
    </div>
  )
}
