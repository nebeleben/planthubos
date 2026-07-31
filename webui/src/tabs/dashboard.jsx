import { useEffect, useState } from 'preact/hooks'

function ago(lastSeenS, nowS) {
  const d = Math.max(0, nowS - lastSeenS)
  if (d < 90) return `${d}s ago`
  if (d < 5400) return `${Math.round(d / 60)}m ago`
  return `${Math.round(d / 3600)}h ago`
}

function Tile({ s, nowS }) {
  return (
    <div class="tile">
      <h3>{s.name || s.mac}</h3>
      <div class="readings">
        <div><span class="val">{s.moisture ?? '–'}</span><span class="unit">% soil</span></div>
        <div><span class="val">{s.temp ?? '–'}</span><span class="unit">°C</span></div>
        <div><span class="val">{s.lux ?? '–'}</span><span class="unit">lux</span></div>
        <div><span class="val">{s.conductivity ?? '–'}</span><span class="unit">µS/cm</span></div>
      </div>
      <p class="seen">{ago(s.last_seen_s, nowS)}</p>
    </div>
  )
}

export function DashboardTab() {
  const [sensors, setSensors] = useState({})   // keyed by mac
  const [status, setStatus] = useState('loading') // loading | live | error
  const [reconnecting, setReconnecting] = useState(false)
  const [nowS, setNowS] = useState(0)

  useEffect(() => {
    let alive = true
    let es
    let ticker
    let retryTimer

    // The hub's SSE endpoint caps at 2 clients and answers a 3rd with a
    // plain HTTP 503, not a dropped connection -- and a non-200 response
    // permanently closes the browser's EventSource with no auto-reconnect.
    // Factored out so onerror can rebuild the connection from scratch.
    function connect() {
      es = new EventSource('/api/v1/events')
      es.onopen = () => setReconnecting(false)
      es.onmessage = (ev) => {
        const s = JSON.parse(ev.data)
        setSensors((prev) => ({ ...prev, [s.mac]: s }))
        setNowS((prev) => Math.max(prev, s.last_seen_s))
        setReconnecting(false)
      }
      es.onerror = () => {
        setReconnecting(true)
        if (es.readyState === EventSource.CLOSED) {
          // Permanently closed (e.g. the 503 case above) -- EventSource
          // won't retry on its own, so schedule a fresh connection.
          es.close()
          retryTimer = setTimeout(() => {
            if (alive) connect()
          }, 5000)
        }
        // Otherwise it's a transient drop; the browser is already retrying
        // this same EventSource, so just leave the "reconnecting" banner up.
      }
    }

    fetch('/api/v1/sensors')
      .then((r) => r.json())
      .then((data) => {
        if (!alive) return
        const byMac = {}
        for (const s of data.sensors) byMac[s.mac] = s
        setSensors(byMac)
        // hub reports uptime-based last_seen; use the freshest as "now"
        setNowS(Math.max(0, ...data.sensors.map((s) => s.last_seen_s)))
        setStatus('live')

        // Local 1s ticker so "Xs ago" keeps advancing for sensors that go
        // dark, instead of freezing at their last SSE message's last_seen_s.
        ticker = setInterval(() => setNowS((prev) => prev + 1), 1000)

        connect()
      })
      .catch(() => {
        if (alive) setStatus('error')
      })
    return () => {
      alive = false
      es && es.close()
      ticker && clearInterval(ticker)
      retryTimer && clearTimeout(retryTimer)
    }
  }, [])

  const list = Object.values(sensors)
  if (status === 'loading') return <p class="placeholder">Loading…</p>
  if (status === 'error') return <p class="error">Hub not reachable — retrying via SSE…</p>
  if (list.length === 0)
    return <p class="placeholder">No sensors found yet. MiFlora devices are discovered automatically — bring one in range.</p>
  return (
    <div>
      {reconnecting && <p class="banner">Reconnecting to hub…</p>}
      <div class="tiles">{list.map((s) => <Tile key={s.mac} s={s} nowS={nowS} />)}</div>
    </div>
  )
}
