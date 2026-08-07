import { useEffect, useRef, useState } from 'preact/hooks'

// last_seen_s on a /api/v1/plants row is already an AGE in seconds,
// computed hub-side (sensors_json.c's plant_json()/age_s()) -- unlike the
// pre-M8 dashboard, which paired a raw last-seen uptime with a separately
// fetched /api/v1/status.uptime_s (nodes.jsx's fmtAgo doc comment explains
// why that pairing is necessary there). No status fetch needed here.
function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

// Adds wall-clock seconds elapsed since the plants list was last fetched on
// top of the age captured at that fetch -- keeps "Xs ago" advancing between
// polls/SSE nudges, the same UX the pre-M8 dashboard's nowS ticker gave,
// just rebased onto an age field instead of an absolute uptime.
function liveAge(lastSeenS, elapsedS) {
  return lastSeenS == null ? null : lastSeenS + elapsedS
}

function ProbeChip({ plant, elapsedS }) {
  const p = plant.probe
  if (!p) {
    return <p class="probe-chip none">no probe — last data {fmtAge(liveAge(plant.last_seen_s, elapsedS))}</p>
  }
  const battery = p.battery != null ? ` · ${p.battery}% battery` : ''
  // Assigned but never heard on the radio yet (plant_json()'s "pending"
  // probe branch -- a pre-assigned replacement probe, api_v1.c's
  // plants_probe_post comment): rssi/via/battery are all null. The task
  // brief's chip text ("via <node>" / "direct" / "no probe") doesn't cover
  // this third case explicitly, so it's called out separately here rather
  // than silently folded into "direct" (which would claim a live radio
  // reading that doesn't exist) or "no probe" (which would hide that a
  // probe IS assigned and waiting) -- a judgment call, flagged as such.
  if (p.rssi == null) {
    return <p class="probe-chip pending">probe assigned — awaiting first reading</p>
  }
  const source = p.via ? `via ${p.via.name || p.via.mac}` : 'direct'
  // M8 final review fix (M2): show data age on the live chip too, not just
  // the probe-less one below -- otherwise a probe that's still "attached"
  // but hasn't actually reported in days (e.g. a dead battery the registry
  // hasn't evicted) reads as fine forever. Same fmtAge/liveAge formatter,
  // same plant.last_seen_s field plant_json() already populates on the
  // live branch (sensors_json.c).
  return <p class="probe-chip">{source} · {p.rssi} dBm{battery} · {fmtAge(liveAge(plant.last_seen_s, elapsedS))}</p>
}

function Card({ plant, elapsedS }) {
  return (
    <div class="tile">
      <h3>{plant.name || `Plant ${plant.id}`}</h3>
      <div class="readings">
        <div><span class="val">{plant.moisture ?? '–'}</span><span class="unit">% soil</span></div>
        <div><span class="val">{plant.temp ?? '–'}</span><span class="unit">°C</span></div>
        <div><span class="val">{plant.lux ?? '–'}</span><span class="unit">lux</span></div>
        <div><span class="val">{plant.conductivity ?? '–'}</span><span class="unit">µS/cm</span></div>
      </div>
      <ProbeChip plant={plant} elapsedS={elapsedS} />
    </div>
  )
}

export function DashboardTab() {
  const [plants, setPlants] = useState([])
  const [status, setStatus] = useState('loading') // loading | live | error
  const [reconnecting, setReconnecting] = useState(false)
  const [elapsedS, setElapsedS] = useState(0)   // seconds since the last successful /plants fetch
  // The set of macs this tab currently "knows about" -- every plant's
  // assigned probe mac, plus every mac in the probe pool (even ones not
  // assigned to any plant). Kept in a ref rather than state: it's an SSE
  // dispatch aid, not something that should trigger its own re-render.
  const knownMacsRef = useRef(new Set())
  // H2 (final M8 review): SSE-driven refetch coalescing. Every SSE frame
  // used to trigger its own immediate GET /plants (plus, for an unknown
  // mac, a GET /sensors too) -- a burst of frames from several sensors
  // reporting close together meant a burst of full refetches, each doing
  // its own pre-sync per-request ring scan for every probe-less plant
  // (plants_last_values()). One shared timer per mount, trailing-edge only:
  // every message pushes it out another 3s rather than firing one per
  // message, so a burst collapses into a single pair of fetches 3s after
  // the LAST frame in the burst, not one pair per frame.
  const debounceTimerRef = useRef(null)
  const pendingSensorsRef = useRef(false)

  useEffect(() => {
    let alive = true
    let es
    let ticker
    let retryTimer
    const controller = new AbortController()

    function applyPlants(list) {
      if (!alive) return
      setPlants(list)
      setElapsedS(0)
      const macs = knownMacsRef.current
      for (const p of list) if (p.probe) macs.add(p.probe.mac)
    }

    function fetchPlants() {
      return fetch('/api/v1/plants', { signal: controller.signal })
        .then((r) => r.json())
        .then((d) => applyPlants(d.plants))
    }

    // /api/v1/sensors is the full probe pool -- broader than "assigned to a
    // plant" (a freshly discovered, still-unassigned sensor lives here but
    // not in any plant's "probe" field). Folded into knownMacsRef purely so
    // the NEXT event from that same mac is recognised as known too, instead
    // of re-triggering the double-fetch below every single time it reports.
    function fetchKnownSensors() {
      return fetch('/api/v1/sensors', { signal: controller.signal })
        .then((r) => r.json())
        .then((d) => { if (alive) for (const s of d.sensors) knownMacsRef.current.add(s.mac) })
    }

    // Trailing-edge debounce (H2): fires 3s after the LAST SSE frame in a
    // burst, not once per frame. Whether ANY frame in the coalesced window
    // named an unknown mac is tracked in pendingSensorsRef -- set on the
    // triggering message (known-ness is evaluated then, off knownMacsRef's
    // state at that instant), consumed once the timer actually fires -- so
    // an unknown mac still refetches both lists, just after the same
    // debounce window as everything else, instead of jumping the queue.
    function flushRefetch() {
      debounceTimerRef.current = null
      const needSensors = pendingSensorsRef.current
      pendingSensorsRef.current = false
      fetchPlants().catch(() => {})
      if (needSensors) fetchKnownSensors().catch(() => {})
    }

    // The hub's SSE endpoint caps at 2 clients and answers a 3rd with a
    // plain HTTP 503, not a dropped connection -- and a non-200 response
    // permanently closes the browser's EventSource with no auto-reconnect.
    // Factored out so onerror can rebuild the connection from scratch.
    function connect() {
      es = new EventSource('/api/v1/events')
      es.onopen = () => setReconnecting(false)
      es.onmessage = (ev) => {
        const s = JSON.parse(ev.data)
        setReconnecting(false)
        const known = knownMacsRef.current.has(s.mac)
        if (!known) pendingSensorsRef.current = true
        if (debounceTimerRef.current) clearTimeout(debounceTimerRef.current)
        debounceTimerRef.current = setTimeout(flushRefetch, 3000)
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

    Promise.all([fetchPlants(), fetchKnownSensors()])
      .then(() => {
        if (!alive) return
        setStatus('live')
        // Local 1s ticker so "Xs ago" keeps advancing for plants that go
        // dark, instead of freezing at the last fetch's last_seen_s.
        ticker = setInterval(() => setElapsedS((s) => s + 1), 1000)
        connect()
      })
      .catch((err) => {
        if (alive && err.name !== 'AbortError') setStatus('error')
      })

    return () => {
      alive = false
      controller.abort()
      es && es.close()
      ticker && clearInterval(ticker)
      retryTimer && clearTimeout(retryTimer)
      debounceTimerRef.current && clearTimeout(debounceTimerRef.current)
    }
  }, [])

  if (status === 'loading') return <p class="placeholder">Loading…</p>
  if (status === 'error') return <p class="error">Hub not reachable — retrying via SSE…</p>
  if (plants.length === 0)
    return <p class="placeholder">No plants yet. Plants appear automatically once a MiFlora sensor is heard, or add one manually on the Probes tab.</p>
  return (
    <div>
      {reconnecting && <p class="banner">Reconnecting to hub…</p>}
      <div class="tiles">{plants.map((p) => <Card key={p.id} plant={p} elapsedS={elapsedS} />)}</div>
    </div>
  )
}
