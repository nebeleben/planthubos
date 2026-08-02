import { useEffect, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'

function Row({ s, onSaved }) {
  const [name, setName] = useState(s.name || '')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth

  async function save(e) {
    e.preventDefault()
    setState('saving')
    try {
      const res = await fetch(`/api/v1/sensors/${s.mac.replaceAll(':', '')}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ name }),
      })
      if (res.ok) { setState('saved'); onSaved(s.mac, name) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <tr>
      <td>
        <form onSubmit={save} class="namef">
          <input value={name} maxlength={32} placeholder={s.mac}
                 onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
          <button type="submit" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>
      </td>
      <td class="mono">{s.mac}</td>
      <td>{s.battery != null ? `${s.battery}%` : '–'}</td>
      <td>{s.moisture != null ? `${s.moisture}%` : '–'}</td>
      {/* Direct BLE reception (the hub hearing it on its own radio) is the
          strictly-better case and renders nothing here; a non-null via
          means a node currently relays this sensor (strongest RSSI wins --
          see registry.h) and names which one, so re-attribution after
          moving a plant is visible instead of mysterious. */}
      <td class="via">{s.via ? `via ${s.via.name || s.via.mac} · ${s.via.rssi} dBm` : ''}</td>
    </tr>
  )
}

export function DevicesTab() {
  const [sensors, setSensors] = useState(null)
  const [error, setError] = useState(false)

  useEffect(() => {
    fetch('/api/v1/sensors')
      .then((r) => r.json())
      .then((d) => setSensors(d.sensors))
      .catch(() => setError(true))
  }, [])

  function onSaved(mac, name) {
    setSensors((prev) => prev.map((s) => (s.mac === mac ? { ...s, name } : s)))
  }

  if (error) return <p class="error">Hub not reachable.</p>
  if (!sensors) return <p class="placeholder">Loading…</p>
  if (sensors.length === 0) return <p class="placeholder">No sensors discovered yet.</p>
  return (
    <table class="devices">
      <thead><tr><th>Name</th><th>MAC</th><th>Battery</th><th>Moisture</th><th>Via</th></tr></thead>
      <tbody>{sensors.map((s) => <Row key={s.mac} s={s} onSaved={onSaved} />)}</tbody>
    </table>
  )
}
