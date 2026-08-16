import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'
import { loadCaps, capLabel, fmtCapParts } from '../lib/caps.js'

// age_s on both a plant binding (GET /api/v1/plants) and a device capability
// slot is already an AGE in seconds, computed hub-side -- same convention
// every tab's own fmtAge duplicates rather than sharing (see rules.jsx's
// doc comment on why).
function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

// Adds wall-clock seconds elapsed since the plants list was last fetched on
// top of the age captured at that fetch -- keeps "Xs ago" advancing between
// polls/SSE nudges without re-fetching every second.
function liveAge(ageS, elapsedS) {
  return ageS == null ? null : ageS + elapsedS
}

// One bound capability's reading. `caps` is the loaded capability-metadata
// map (lib/caps.js) that supplies the label, unit and display precision --
// a plain unit like "%" is ambiguous on its own (soil.moisture, battery.level
// and air.humidity all use it), so the label always accompanies the value.
// A binding with no value yet (device assigned but never reported) renders
// "–" rather than being hidden -- only capabilities the plant has NO
// binding for at all are omitted (spec §7: "missing capabilities are simply
// absent").
function Reading({ caps, binding, elapsedS }) {
  const { text: valueText, unit } = fmtCapParts(caps, binding.cap, binding.value)
  return (
    <div class="reading">
      <div class="hint">{capLabel(caps, binding.cap)}</div>
      <div><span class="val">{valueText}</span><span class="unit">{unit}</span></div>
      <div class="hint">{fmtAge(liveAge(binding.age_s, elapsedS))}</div>
    </div>
  )
}

function Card({ plant, caps, elapsedS }) {
  return (
    <div class="tile">
      <h3>{plant.name || `Plant ${plant.id}`}</h3>
      {plant.bindings.length === 0 ? (
        <p class="hint">No capabilities bound yet — bind a device on the Plants tab.</p>
      ) : (
        <div class="readings">
          {plant.bindings.map((b) => <Reading key={b.cap} caps={caps} binding={b} elapsedS={elapsedS} />)}
        </div>
      )}
    </div>
  )
}

// Clean-start notice (spec §5, task brief step 6): GET /api/v1/notice
// reports "pending" once, the first time the UI loads after a first-V2-boot
// wipe (data_fmt_apply()); dismissing it is permanent (NVS-latched
// server-side, data_fmt_dismiss_notice()). Lives on Dashboard specifically
// because it's the landing tab -- app.jsx's own brief is scoped to just the
// Probes->Devices label rename, so this can't live in a shared shell
// component without exceeding that scope.
function Notice() {
  const [notice, setNotice] = useState(null)   // null = not loaded yet
  const [dismissing, setDismissing] = useState(false)
  const [error, setError] = useState(false)

  useEffect(() => {
    const controller = new AbortController()
    fetch('/api/v1/notice', { signal: controller.signal })
      .then((r) => r.json())
      .then(setNotice)
      .catch((err) => { if (err.name !== 'AbortError') setNotice({ pending: false }) })
    return () => controller.abort()
  }, [])

  async function dismiss() {
    setDismissing(true)
    setError(false)
    try {
      const res = await fetch('/api/v1/notice/dismiss', { method: 'POST', headers: authHeaders() })
      if (res.ok) setNotice({ pending: false })
      else setError(true)
    } catch {
      setError(true)
    }
    setDismissing(false)
  }

  if (!notice || !notice.pending) return null
  return (
    <p class="infobox">
      {notice.message}
      {' '}
      <button type="button" onClick={dismiss} disabled={dismissing}>{dismissing ? '…' : 'Dismiss'}</button>
      {error && <span class="error"> failed — try again</span>}
    </p>
  )
}

export function DashboardTab() {
  const [caps, setCaps] = useState(null)
  const [plants, setPlants] = useState([])
  const [status, setStatus] = useState('loading') // loading | live | error
  const [reconnecting, setReconnecting] = useState(false)
  const [elapsedS, setElapsedS] = useState(0)   // seconds since the last successful /plants fetch
  // Trailing-edge SSE debounce (carried over from the pre-M8/M8 dashboard):
  // a burst of device updates close together collapses into a single
  // /api/v1/plants refetch 3s after the LAST frame in the burst, not one
  // refetch per frame. The M2 SSE payload's plant_ids is always [] (see
  // devices_json.c/sse.c's on_sensor_update) -- it's a "something changed"
  // nudge only, never itself a source of binding data, so there is nothing
  // to reconcile locally; every nudge just re-triggers the same
  // authoritative /api/v1/plants fetch.
  const debounceTimerRef = useRef(null)

  useEffect(() => {
    loadCaps().then(setCaps).catch(() => {})
  }, [])

  useEffect(() => {
    let alive = true
    let es
    let ticker
    let retryTimer
    const controller = new AbortController()

    function fetchPlants() {
      return fetch('/api/v1/plants', { signal: controller.signal })
        .then((r) => r.json())
        .then((d) => {
          if (!alive) return
          setPlants(d.plants)
          setElapsedS(0)
        })
    }

    function flushRefetch() {
      debounceTimerRef.current = null
      fetchPlants().catch(() => {})
    }

    // Same reconnect discipline as rules.jsx's EventFeed: the hub's SSE
    // endpoint caps at 2 clients and answers a 3rd with a plain HTTP 503,
    // which permanently closes the browser's EventSource with no
    // auto-reconnect -- onerror rebuilds it from scratch.
    function connect() {
      es = new EventSource('/api/v1/events')
      es.onopen = () => setReconnecting(false)
      es.onmessage = () => {
        setReconnecting(false)
        if (debounceTimerRef.current) clearTimeout(debounceTimerRef.current)
        debounceTimerRef.current = setTimeout(flushRefetch, 3000)
      }
      es.onerror = () => {
        setReconnecting(true)
        if (es.readyState === EventSource.CLOSED) {
          es.close()
          retryTimer = setTimeout(() => { if (alive) connect() }, 5000)
        }
      }
    }

    fetchPlants()
      .then(() => {
        if (!alive) return
        setStatus('live')
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

  return (
    <div>
      <Notice />
      {status === 'loading' && <p class="placeholder">Loading…</p>}
      {status === 'error' && <p class="error">Hub not reachable — retrying via SSE…</p>}
      {status === 'live' && plants.length === 0 && (
        <p class="placeholder">No plants yet. Create one on the Plants tab and bind a device to it.</p>
      )}
      {status === 'live' && plants.length > 0 && !caps && <p class="placeholder">Loading…</p>}
      {status === 'live' && plants.length > 0 && caps && (
        <div>
          {reconnecting && <p class="banner">Reconnecting to hub…</p>}
          <div class="tiles">{plants.map((p) => <Card key={p.id} plant={p} caps={caps} elapsedS={elapsedS} />)}</div>
        </div>
      )}
    </div>
  )
}
