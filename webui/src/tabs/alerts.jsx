import { useEffect, useRef, useState } from 'preact/hooks'
import { levelLabel, alertCodeLabel, parseAlertMessage } from '../lib/actuators.js'

// Ages/timestamps: same split as rules.jsx's own fmtAge/fmtEventTs
// (duplicated locally rather than shared -- this codebase's own convention,
// see devices.jsx's fmtAge doc comment). Event feed timestamps are absolute
// epoch seconds (spec §6); age is derived client-side against the browser's
// own clock, the best available reference for a feed the operator is
// actively watching.
function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}
function fmtEventTs(ts) {
  if (!ts) return '—'
  const ageS = Math.floor(Date.now() / 1000) - ts
  return fmtAge(Math.max(0, ageS))
}

// Same shared bookmark app.jsx's tab-badge poll and rules.jsx's inline feed
// both already read/write -- writing it here too means having EITHER
// events-carrying tab open keeps the badge clear, and a stale localStorage
// value from before the M5b Task 5 event-log format bump (see app.jsx's own
// clamp) self-heals the same way regardless of which tab wrote it.
const EVENTS_SEEN_KEY = 'planthub_events_seen'

// events_json_get() (sse.c) caps every response at 50 rows -- see
// rules.jsx's identical fetchEventsCatchUp for the full reasoning (a single
// after=0 fetch can silently drop a gap on a busy hub with >50 events
// total). Duplicated here rather than shared, matching this codebase's
// established per-tab convention for this exact function.
const EVENTS_MAX_PAGES = 6
const EVENTS_KEEP = 200

async function fetchEventsCatchUp(afterSeq, signal) {
  let after = afterSeq
  let events = []
  let maxSeq = afterSeq
  let lastSeq = afterSeq
  for (let page = 0; page < EVENTS_MAX_PAGES; page++) {
    const d = await fetch(`/api/v1/events?after=${after}`, { signal }).then((r) => r.json())
    lastSeq = d.last_seq != null ? d.last_seq : lastSeq
    if (d.events.length > 0) {
      events = events.concat(d.events)
      maxSeq = d.events[d.events.length - 1].seq
      after = maxSeq
    }
    if (d.events.length < 50 || maxSeq >= lastSeq) break
  }
  return { events, maxSeq, lastSeq }
}

const LEVEL_FILTERS = ['all', 'critical', 'alert', 'notify', 'log']

// One row's structured columns (M5b Task 11's alert_ring.c collapsing,
// alert.c's fixed message shape): device/action/param recovered by
// parseAlertMessage() when the row IS one of those firmware-generated
// alerts, alertCodeLabel() turning the bare numeric code back into the
// sentence its own enum comment already carries. A rule's own log()/
// notify() text (or the ring-overflow message) doesn't match that shape --
// parseAlertMessage() returns null for it, and this renders the raw
// message as-is in the Detail column instead of fabricating columns the
// wire format never carried for that row.
function EventRow({ e }) {
  const parsed = parseAlertMessage(e.msg)
  const critical = e.level === 'critical'
  return (
    <tr class={critical ? 'level-critical-row' : ''}>
      <td class="hint">{fmtEventTs(e.ts)}</td>
      <td><span class={`level-badge level-${e.level}`}>{levelLabel(e.level)}</span></td>
      <td>{parsed ? `dev ${parsed.dev}` : '—'}</td>
      <td class="mono">{parsed ? (parsed.action || '—') : '—'}</td>
      <td>{parsed ? parsed.param : '—'}</td>
      <td>
        {parsed ? alertCodeLabel(parsed.code) : e.msg}
        {parsed && parsed.repeat > 1 ? ` (×${parsed.repeat})` : ''}
      </td>
    </tr>
  )
}

// Live event feed: GET .../events?after=0 on mount (rendered newest-first),
// then SSE's "event"-typed messages trigger a follow-up catch-up fetch --
// the identical shape as rules.jsx's own EventFeed (see that file's much
// longer comment for the full reasoning on catch-up paging, dedup and SSE
// reconnect discipline). This tab additionally renders every level (not
// just log/notify) with alert/critical visually distinct, is filterable by
// level, and decodes the safety core's alert message shape into separate
// device/action/parameter columns rather than showing raw text for those
// rows.
export function AlertsTab() {
  const [events, setEvents] = useState(null)
  const [error, setError] = useState(false)
  const [filter, setFilter] = useState('all')
  const lastSeqRef = useRef(0)
  const pollingRef = useRef(false)
  const dirtyRef = useRef(false)

  useEffect(() => {
    let alive = true
    let es
    let retryTimer
    const controller = new AbortController()

    function markSeen(seq) {
      localStorage.setItem(EVENTS_SEEN_KEY, String(seq))
    }

    async function pollNew() {
      if (pollingRef.current) { dirtyRef.current = true; return }
      pollingRef.current = true
      try {
        let again = true
        while (again) {
          dirtyRef.current = false
          const before = lastSeqRef.current
          let caught
          try {
            caught = await fetchEventsCatchUp(before, controller.signal)
          } catch {
            break
          }
          if (!alive) return
          const { events: fresh, maxSeq, lastSeq } = caught
          if (fresh.length > 0) {
            setEvents((prev) => {
              const existing = prev || []
              const known = new Set(existing.map((e) => e.seq))
              const toAdd = fresh.filter((e) => e.seq > before && !known.has(e.seq))
              if (toAdd.length === 0) return existing
              return [...toAdd.slice().reverse(), ...existing].slice(0, EVENTS_KEEP)
            })
            lastSeqRef.current = maxSeq
          } else if (lastSeq > lastSeqRef.current) {
            lastSeqRef.current = lastSeq
          }
          markSeen(lastSeqRef.current)
          again = dirtyRef.current
        }
      } finally {
        pollingRef.current = false
      }
    }

    function connect() {
      es = new EventSource('/api/v1/events')
      es.addEventListener('event', () => { pollNew().catch(() => {}) })
      es.onerror = () => {
        if (es.readyState === EventSource.CLOSED) {
          es.close()
          retryTimer = setTimeout(() => { if (alive) connect() }, 5000)
        }
      }
    }

    async function loadInitial() {
      try {
        const { events: collected, maxSeq, lastSeq } = await fetchEventsCatchUp(0, controller.signal)
        if (!alive) return
        lastSeqRef.current = collected.length > 0 ? maxSeq : lastSeq
        markSeen(lastSeqRef.current)
        setEvents(collected.slice().reverse().slice(0, EVENTS_KEEP))
        connect()
      } catch (err) {
        if (alive && err.name !== 'AbortError') setError(true)
      }
    }

    loadInitial()

    return () => {
      alive = false
      controller.abort()
      es && es.close()
      retryTimer && clearTimeout(retryTimer)
    }
  }, [])

  const filtered = events ? events.filter((e) => filter === 'all' || e.level === filter) : null

  return (
    <div class="panel">
      <h2>Alerts</h2>
      <div class="hist-controls level-filter">
        {LEVEL_FILTERS.map((f) => (
          <button key={f} type="button" class={f === filter ? 'active' : ''} onClick={() => setFilter(f)}>
            {f === 'all' ? 'All' : levelLabel(f)}
          </button>
        ))}
      </div>
      {error && <p class="error">Hub not reachable.</p>}
      {!error && events === null && <p class="placeholder">Loading…</p>}
      {!error && events && events.length === 0 && (
        <p class="placeholder">No events yet — rule fires, actuator commands and safety-core alerts appear here as they happen.</p>
      )}
      {!error && filtered && events.length > 0 && filtered.length === 0 && (
        <p class="placeholder">No {levelLabel(filter).toLowerCase()} events yet.</p>
      )}
      {!error && filtered && filtered.length > 0 && (
        <div class="table-scroll">
          <table class="devices">
            <thead>
              <tr><th>When</th><th>Level</th><th>Device</th><th>Action</th><th>Param</th><th>Detail</th></tr>
            </thead>
            <tbody>
              {filtered.map((e) => <EventRow key={e.seq} e={e} />)}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}
