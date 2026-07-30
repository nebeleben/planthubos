import { useEffect, useState } from 'preact/hooks'

export function NetworkTab() {
  const [networks, setNetworks] = useState(null)
  const [ssid, setSsid] = useState('')
  const [password, setPassword] = useState('')
  const [state, setState] = useState('idle') // idle | scanning | joining | sent | error

  async function scan() {
    setState('scanning')
    try {
      const res = await fetch('/api/v1/wifi/scan')
      const data = await res.json()
      setNetworks(data.networks)
      setState('idle')
    } catch {
      setState('error')
    }
  }

  useEffect(() => { scan() }, [])

  async function join(e) {
    e.preventDefault()
    setState('joining')
    try {
      const res = await fetch('/api/v1/wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid, password }),
      })
      setState(res.ok ? 'sent' : 'error')
    } catch {
      setState('error')
    }
  }

  if (state === 'sent') {
    return (
      <div>
        <h2>Joining "{ssid}"...</h2>
        <p>
          The hub is switching to your WiFi. Reconnect your device to that network,
          then find the hub at <code>http://planthub.local</code> (or check your
          router for its IP). If the hub cannot join, the PlantHub access point
          reappears after ~30 seconds.
        </p>
      </div>
    )
  }

  return (
    <div>
      <h2>WiFi Setup</h2>
      <button onClick={scan} disabled={state === 'scanning'}>
        {state === 'scanning' ? 'Scanning…' : 'Rescan'}
      </button>
      <ul class="networks">
        {networks?.map((n) => (
          <li key={n.ssid}>
            <button onClick={() => setSsid(n.ssid)}>
              {n.ssid} {n.secure ? '🔒' : ''} ({n.rssi} dBm)
            </button>
          </li>
        ))}
      </ul>
      <form onSubmit={join}>
        <label>
          SSID
          <input value={ssid} onInput={(e) => setSsid(e.currentTarget.value)} required maxlength={32} />
        </label>
        <label>
          Password
          <input type="password" value={password} onInput={(e) => setPassword(e.currentTarget.value)} maxlength={64} />
        </label>
        <button type="submit" disabled={state === 'joining' || !ssid}>
          {state === 'joining' ? 'Sending…' : 'Join'}
        </button>
      </form>
      {state === 'error' && <p class="error">Request failed — is the hub reachable?</p>}
    </div>
  )
}
