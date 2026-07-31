import { useEffect, useRef, useState } from 'preact/hooks'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'

const RANGES = {
  Day:   { seconds: 86400,    tier: 'raw' },
  Week:  { seconds: 7 * 86400,  tier: 'hourly' },
  Month: { seconds: 30 * 86400, tier: 'hourly' },
}

function Chart({ points }) {
  const ref = useRef(null)
  const plotRef = useRef(null)

  useEffect(() => {
    if (!ref.current) return
    const data = [
      points.map((p) => p[0]),
      points.map((p) => p[2]),   // moisture
      points.map((p) => p[1]),   // temp
      points.map((p) => p[3]),   // lux
      points.map((p) => p[4]),   // conductivity
    ]
    const opts = {
      width: Math.min(ref.current.clientWidth || 640, 900),
      height: 320,
      series: [
        {},
        { label: 'Moisture %', stroke: '#4a7c4a', width: 2 },
        { label: 'Temp °C', stroke: '#c0504d', width: 2, scale: 't' },
        { label: 'Lux', stroke: '#c8a234', width: 1, show: false, scale: 'lx' },
        { label: 'µS/cm', stroke: '#4472c4', width: 1, show: false, scale: 'us' },
      ],
      axes: [
        {},
        { scale: 'y' },
        { scale: 't', side: 1, grid: { show: false } },
      ],
      scales: { x: { time: true } },
    }
    plotRef.current = new uPlot(opts, data, ref.current)
    return () => { plotRef.current && plotRef.current.destroy() }
  }, [points])

  return <div ref={ref} class="chart" />
}

export function HistoryTab() {
  const [sensors, setSensors] = useState([])
  const [mac, setMac] = useState('')
  const [range, setRange] = useState('Day')
  const [points, setPoints] = useState(null)
  const [state, setState] = useState('loading') // loading | ready | unsynced | error

  useEffect(() => {
    const controller = new AbortController()
    fetch('/api/v1/sensors', { signal: controller.signal })
      .then((r) => r.json())
      .then((d) => {
        setSensors(d.sensors)
        if (d.sensors.length > 0) setMac(d.sensors[0].mac)
        else setState('ready')
      })
      .catch((err) => { if (err.name !== 'AbortError') setState('error') })
    return () => controller.abort()
  }, [])

  useEffect(() => {
    if (!mac) return
    setState('loading')
    const controller = new AbortController()
    const r = RANGES[range]
    ;(async () => {
      try {
        // The hub's own clock (epoch_s) is the authority its stored
        // timestamps live on -- querying with the browser's clock would
        // clip or empty the range under any skew, so fetch status first.
        const st = await fetch('/api/v1/status', { signal: controller.signal }).then((res) => res.json())
        if (!st.time_synced) { setState('unsynced'); return }
        const to = st.epoch_s
        const from = to - r.seconds
        const d = await fetch(`/api/v1/sensors/${mac.replaceAll(':', '')}/history?tier=${r.tier}`
            + `&from=${from}&to=${to}`, { signal: controller.signal }).then((res) => res.json())
        if (!d.synced) { setState('unsynced'); return }
        setPoints(d.points)
        setState('ready')
      } catch (err) {
        if (err.name !== 'AbortError') setState('error')
      }
    })()
    return () => controller.abort()
  }, [mac, range])

  return (
    <div>
      <div class="hist-controls">
        <select value={mac} onChange={(e) => setMac(e.currentTarget.value)}>
          {sensors.map((s) => <option key={s.mac} value={s.mac}>{s.name || s.mac}</option>)}
        </select>
        {Object.keys(RANGES).map((k) => (
          <button key={k} class={k === range ? 'active' : ''} onClick={() => setRange(k)}>{k}</button>
        ))}
      </div>
      {state === 'error' && <p class="error">Hub not reachable.</p>}
      {state === 'unsynced' && <p class="placeholder">Hub clock not synced yet — history becomes available once the hub learns the time (open this page once while online).</p>}
      {state === 'loading' && <p class="placeholder">Loading…</p>}
      {state === 'ready' && sensors.length === 0 && <p class="placeholder">No sensors yet.</p>}
      {state === 'ready' && points && points.length === 0 && <p class="placeholder">No data in this range yet.</p>}
      {state === 'ready' && points && points.length > 0 && <Chart points={points} />}
    </div>
  )
}
