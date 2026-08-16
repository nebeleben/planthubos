import { useEffect, useRef, useState } from 'preact/hooks'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'
import { loadCaps, capLabel } from '../lib/caps.js'

// UI label -> the `range` query value GET /api/v1/history accepts (spec §7:
// the server now picks BOTH the storage tier and the window from this one
// value -- day/raw-24h, week/hourly-7d, month/hourly-30d).
const RANGES = { Day: 'day', Week: 'week', Month: 'month' }

function Chart({ points, label, unit }) {
  const ref = useRef(null)
  const plotRef = useRef(null)

  useEffect(() => {
    if (!ref.current) return
    const data = [points.map((p) => p[0]), points.map((p) => p[1])]
    const opts = {
      width: Math.min(ref.current.clientWidth || 640, 900),
      height: 320,
      series: [{}, { label: unit ? `${label} (${unit})` : label, stroke: '#4a7c4a', width: 2 }],
      axes: [{}, {}],
      scales: { x: { time: true } },
    }
    plotRef.current = new uPlot(opts, data, ref.current)
    return () => { plotRef.current && plotRef.current.destroy() }
  }, [points, label, unit])

  return <div ref={ref} class="chart" />
}

export function HistoryTab() {
  const [caps, setCaps] = useState(null)
  const [plants, setPlants] = useState([])
  const [plantId, setPlantId] = useState('')
  const [capId, setCapId] = useState(null)
  // Every capability id this plant's ring file has EVER historised for the
  // tier of the last successful fetch (the response's bare-id "available"
  // array, spec §7/history_get()'s doc comment) -- a capability that used
  // to be bound, since cleared, still shows up here so its history stays
  // reachable even though it's no longer in the plant's current bindings.
  const [availableCaps, setAvailableCaps] = useState([])
  const [range, setRange] = useState('Day')
  const [points, setPoints] = useState(null)
  const [state, setState] = useState('loading') // loading | ready | unsynced | error
  // True only for the very first fetch after switching to a plant with NO
  // current bindings: there is no real capability id to ask for yet, so the
  // fetch effect below has to guess (capability 0) purely to learn the ring
  // file's real column map from the response, then self-correct onto
  // whatever it actually finds. A plant that already has a bound
  // capability never needs this -- its first real binding is the guess.
  const bootstrapRef = useRef(false)

  useEffect(() => {
    loadCaps().then(setCaps).catch(() => {})
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    fetch('/api/v1/plants', { signal: controller.signal })
      .then((r) => r.json())
      .then((d) => {
        setPlants(d.plants)
        if (d.plants.length > 0) setPlantId(d.plants[0].id)
        else setState('ready')
      })
      .catch((err) => { if (err.name !== 'AbortError') setState('error') })
    return () => controller.abort()
  }, [])

  // Plant switched: pick a starting capability (its first current binding,
  // or capability 0 as the bootstrap guess above) and forget whatever
  // "available" set belonged to the previous plant.
  useEffect(() => {
    if (!plantId) return
    const plant = plants.find((p) => p.id === plantId)
    const bindings = plant ? plant.bindings : []
    bootstrapRef.current = bindings.length === 0
    setCapId(bindings.length > 0 ? bindings[0].cap : 0)
    setAvailableCaps([])
  }, [plantId, plants])

  useEffect(() => {
    if (!plantId || capId == null) return
    setState('loading')
    const controller = new AbortController()
    ;(async () => {
      try {
        const d = await fetch(`/api/v1/history?plant=${plantId}&cap=${capId}&range=${RANGES[range]}`,
          { signal: controller.signal }).then((r) => r.json())
        if (controller.signal.aborted) return
        const avail = d.available || []
        setAvailableCaps(avail)
        // Self-correct a bootstrap guess that missed: this plant has no
        // current binding for `capId` and the ring file never historised
        // it either, but it DOES track something else -- switch to that
        // instead of showing an empty chart for a capability that was
        // never the point. Only ever applies to the guess right after a
        // plant switch (bootstrapRef), never to a capability the operator
        // explicitly picked from the dropdown.
        if (bootstrapRef.current) {
          bootstrapRef.current = false
          if (!avail.includes(capId) && avail.length > 0) {
            setCapId(avail[0])
            return
          }
        }
        if (!d.synced) { setState('unsynced'); return }
        setPoints(d.points)
        setState('ready')
      } catch (err) {
        if (err.name !== 'AbortError') setState('error')
      }
    })()
    return () => controller.abort()
  }, [plantId, capId, range])

  const plant = plants.find((p) => p.id === plantId)
  const boundIds = plant ? plant.bindings.map((b) => b.cap) : []
  const optionIds = Array.from(new Set([...boundIds, ...availableCaps])).sort((a, b) => a - b)
  const capMeta = caps && capId != null ? caps.get(capId) : null

  return (
    <div class="panel">
      <div class="hist-controls">
        <select value={plantId} onChange={(e) => setPlantId(e.currentTarget.value)}>
          {plants.map((p) => <option key={p.id} value={p.id}>{p.name || `Plant ${p.id}`}</option>)}
        </select>
        {caps && optionIds.length > 0 && (
          <select value={capId ?? ''} onChange={(e) => setCapId(Number(e.currentTarget.value))}>
            {optionIds.map((id) => <option key={id} value={id}>{capLabel(caps, id)}</option>)}
          </select>
        )}
        {Object.keys(RANGES).map((k) => (
          <button key={k} class={k === range ? 'active' : ''} onClick={() => setRange(k)}>{k}</button>
        ))}
      </div>
      {state === 'error' && <p class="error">Hub not reachable.</p>}
      {state === 'unsynced' && <p class="placeholder">Hub clock not synced yet — history becomes available once the hub learns the time (open this page once while online).</p>}
      {state === 'loading' && <p class="placeholder">Loading…</p>}
      {state === 'ready' && plants.length === 0 && <p class="placeholder">No plants yet.</p>}
      {state === 'ready' && plants.length > 0 && optionIds.length === 0 && (
        <p class="placeholder">
          No capability history for this plant yet — bind a device on the Plants tab and
          check back after the next history snapshot.
        </p>
      )}
      {state === 'ready' && points && points.length === 0 && optionIds.length > 0 && (
        <p class="placeholder">
          No data in this range yet. The hub records a history snapshot every 15 minutes
          (the first one ~2 minutes after power-on) — live values are on the Dashboard,
          and show up here from the next snapshot onward.
        </p>
      )}
      {state === 'ready' && points && points.length > 0 && (
        <Chart points={points} label={capMeta ? capLabel(caps, capId) : `cap ${capId}`} unit={capMeta ? capMeta.unit : ''} />
      )}
    </div>
  )
}
