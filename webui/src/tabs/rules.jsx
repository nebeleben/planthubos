import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'
import { compile, disassemble } from '../lib/psc/index.js'

// Ages come back from GET /api/v1/rules as SECONDS AGO already (api_v1.c's
// rule_status_json), same shape as devices.jsx/nodes.jsx's own fmtAge --
// duplicated locally rather than shared, matching how every other tab keeps
// its own copy of this exact helper.
function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

// Event feed timestamps are absolute epoch seconds (spec §6), not
// hub-computed ages -- 0 means the hub's clock wasn't synced yet when the
// event fired (history.jsx's own "unsynced" handling is the same signal on
// a different endpoint). Age is derived client-side against the browser's
// own clock, which is the best available reference for a feed the operator
// is actively watching.
function fmtEventTs(ts) {
  if (!ts) return '—'
  const ageS = Math.floor(Date.now() / 1000) - ts
  return fmtAge(Math.max(0, ageS))
}

// Spec §1's worked example, commented out so Compile isn't the very first
// thing a blank editor demands -- it doubles as the in-editor syntax
// reference the brief asks for.
const TEMPLATE = `# rule "monstera dry"
# when plant("Monstera").soil.moisture < 22%
# then notify("Monstera is dry: {plant("Monstera").soil.moisture}%")
# mode edge
# cooldown 2h
# every 30min
`

// rule_status_json() already resolves ready vs errored vs not-ready into a
// single "last_error" string (VM error text takes priority over the generic
// not-ready reason when both would apply) -- this just lays it out with the
// fire counter, it doesn't need to re-derive which reason wins.
function ruleStatusText(r) {
  const cond = r.ready ? 'ready' : `not ready — ${r.last_error || 'error'}`
  return `${cond} · fired ${r.fire_count}× · last ${fmtAge(r.last_fire_age_s)}`
}

// Inline dry-run result (spec §6's /test response): condition + would-fire
// verdict, the per-ref values the VM resolved (or couldn't), and the
// actions it would have run -- rendered strings only, never executed.
function TestResultBlock({ result }) {
  return (
    <div class="infobox">
      <p>
        {result.ready
          ? `condition ${result.cond ? 'true' : 'false'} — ${result.would_fire ? 'would fire' : 'would not fire'}`
          : 'not ready — cannot evaluate'}
      </p>
      {result.refs.length > 0 && (
        <div class="table-scroll">
          <table class="devices">
            <thead><tr><th>Ref</th><th>Value</th><th>Age</th><th>Ready</th></tr></thead>
            <tbody>
              {result.refs.map((rf, i) => (
                <tr key={i}>
                  <td>{rf.ref}</td>
                  <td>{rf.ready ? rf.value : '–'}</td>
                  <td>{rf.ready ? fmtAge(rf.age_s) : '–'}</td>
                  <td>{rf.ready ? 'yes' : 'no'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {result.actions.length > 0 && (
        <ul>
          {result.actions.map((a, i) => <li key={i} class="mono">{a}</li>)}
        </ul>
      )}
    </div>
  )
}

// Same collapsible node-card shape as devices.jsx/nodes.jsx: header carries
// name + a compact status word, the body carries the enabled toggle, full
// status line, Edit/Test/Delete, and (once run) the last Test result.
function RuleCard({ r, open, onToggle, onToggleEnabled, enabling, onEdit, onDelete, deleting,
                    onTest, testing, testResult, actionError }) {
  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{r.name}</span>
        </span>
        <span class={`node-card-age hint${r.enabled && !r.ready ? ' error' : ''}`}>
          {r.enabled ? (r.ready ? 'ready' : 'not ready') : 'disabled'}
        </span>
      </button>
      <div class="node-card-body">
        <div class="node-card-row">
          <label>
            <input type="checkbox" checked={r.enabled} disabled={enabling}
                   onChange={() => onToggleEnabled(r)} />
            {' '}Enabled
          </label>
        </div>
        <div class="node-card-row">
          <span class="hint">{ruleStatusText(r)}</span>
        </div>
        {actionError && <div class="node-card-row"><span class="error">{actionError}</span></div>}
        <div class="node-card-footer">
          <button type="button" onClick={() => onEdit(r.id)}>Edit</button>
          <button type="button" class="btn-primary" onClick={() => onTest(r.id)} disabled={testing}>
            {testing ? '…' : 'Test'}
          </button>
          <button type="button" class="btn-destructive" onClick={() => onDelete(r.id)} disabled={deleting}>
            {deleting ? '…' : 'Delete'}
          </button>
        </div>
        {testResult && <TestResultBlock result={testResult} />}
      </div>
    </div>
  )
}

// Live event feed: GET .../events?after=0 on mount (rendered newest-first),
// then SSE's "event"-typed messages (sse.c's sse_push_event -- a distinct
// message type from the sensor-update stream dashboard.jsx consumes, and
// carrying no seq of its own) trigger a follow-up GET .../events?after=<last>
// to pull the new rows WITH their seq, rather than trying to reconstruct one
// from the SSE payload. Also the sole writer of the "seen" localStorage key
// app.jsx's tab-badge poll reads -- writing it here (on every fetch, initial
// or follow-up) means simply having this tab mounted keeps the badge clear.
const EVENTS_SEEN_KEY = 'planthub_events_seen'

// events_json_get() (sse.c) caps every response at 50 rows -- a single
// after=0 fetch on a hub with more than 50 events total silently drops the
// gap, and jumping the cursor straight to the response's `last_seq` (rather
// than the newest seq actually fetched) would then hide that gap forever.
// EVENTS_MAX_PAGES bounds how far a catch-up walk goes per call (6 * 50 =
// 300 events) so a very busy hub can't turn a tab mount, or one SSE nudge,
// into an unbounded fetch loop; EVENTS_KEEP caps how many rows this tab
// actually keeps in the DOM regardless of how much backlog existed.
const EVENTS_MAX_PAGES = 6
const EVENTS_KEEP = 100

// Walks forward from `afterSeq` in EVENTS_MAX_PAGES batches of up to 50,
// stopping once a page comes back short (fewer than 50 -- nothing left) or
// its newest row's seq has already reached `last_seq` (caught up). Returns
// every row collected (ascending by seq, oldest first) plus the newest seq
// ACTUALLY fetched -- which is what callers must advance their cursor to,
// not the possibly-still-ahead `last_seq`, if the page bound was hit first.
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

function EventFeed() {
  const [events, setEvents] = useState(null)
  const [error, setError] = useState(false)
  const lastSeqRef = useRef(0)
  // In-flight guard for the SSE-triggered catch-up poll: two nudges close
  // together must not fire two concurrent fetches against the same stale
  // cursor (which would both prepend the same rows). While a poll is
  // running, a fresh nudge just sets `dirty` and returns -- the running
  // poll re-runs once more after it finishes instead of a second one
  // starting in parallel.
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
            break   // network hiccup/abort -- the next nudge or reconnect retries
          }
          if (!alive) return
          const { events: fresh, maxSeq, lastSeq } = caught
          if (fresh.length > 0) {
            setEvents((prev) => {
              const existing = prev || []
              const known = new Set(existing.map((e) => e.seq))
              // Dedup belt-and-braces: `before` already excludes anything
              // this cursor has seen, `known` also excludes anything a
              // still-in-flight earlier poll already merged in.
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

    // Same reconnect discipline as dashboard.jsx's EventSource: a non-200
    // (e.g. the hub's 2-client SSE cap) permanently closes the browser's
    // EventSource with no auto-retry, so onerror rebuilds it from scratch.
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
        // If the page bound cut the walk short, the cursor lands on the
        // newest row actually fetched, not the (still-ahead) last_seq --
        // the SSE-triggered pollNew() picks up the remaining gap from there.
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

  return (
    <div class="panel">
      <h2>Events</h2>
      {error && <p class="error">Hub not reachable.</p>}
      {!error && events === null && <p class="placeholder">Loading…</p>}
      {!error && events && events.length === 0 && (
        <p class="placeholder">No events yet — rule fires (log/notify) appear here as they happen.</p>
      )}
      {!error && events && events.length > 0 && (
        <div class="table-scroll">
          <table class="devices">
            <thead><tr><th>When</th><th>Level</th><th>Message</th></tr></thead>
            <tbody>
              {events.map((e) => (
                <tr key={e.seq}>
                  <td class="hint">{fmtEventTs(e.ts)}</td>
                  <td><span class={`level-badge level-${e.level}`}>{e.level}</span></td>
                  <td>{e.msg}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}

export function RulesTab() {
  const [rules, setRules] = useState(null)
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})
  const [testResults, setTestResults] = useState({})
  const [testingId, setTestingId] = useState(null)
  const [enablingId, setEnablingId] = useState(null)
  const [deletingId, setDeletingId] = useState(null)
  const [actionErrors, setActionErrors] = useState({})

  const [editingId, setEditingId] = useState(null)   // null = new rule
  // The enabled state Save sends for the rule currently in the editor --
  // captured off the rule's own GET response when Edit loads it in (so a
  // Save preserves whatever the operator last set via the list's toggle,
  // controller ruling overriding the brief's literal "enabled: true").
  // Brand-new rules (editingId == null) always save enabled, matching the
  // brief as-is there -- there's no "current state" to preserve yet.
  const [editingEnabled, setEditingEnabled] = useState(true)
  const [source, setSource] = useState(TEMPLATE)
  const [compileResult, setCompileResult] = useState(null)
  const [saveState, setSaveState] = useState('idle')  // idle | saving | saved | error | unauth
  const [saveMsg, setSaveMsg] = useState('')

  function refreshRules(signal) {
    return fetch('/api/v1/rules', { signal })
      .then((r) => r.json())
      .then((d) => setRules(d.rules || []))
  }

  useEffect(() => {
    const controller = new AbortController()
    refreshRules(controller.signal).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Background keep-fresh poll, same 10s cadence and discipline as
  // nodes.jsx's own -- ready/fire_count/last_fire_age_s all change purely
  // from engine activity the operator didn't initiate here.
  useEffect(() => {
    const controller = new AbortController()
    const id = setInterval(() => refreshRules(controller.signal).catch(() => {}), 10000)
    return () => { clearInterval(id); controller.abort() }
  }, [])

  function toggleRule(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  function setActionError(id, msg) {
    setActionErrors((prev) => ({ ...prev, [id]: msg }))
  }

  async function onToggleEnabled(r) {
    setEnablingId(r.id)
    setActionError(r.id, '')
    try {
      const res = await fetch(`/api/v1/rules/${r.id}/enable`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ enabled: !r.enabled }),
      })
      if (res.ok) {
        setRules((prev) => prev.map((x) => (x.id === r.id ? { ...x, enabled: !r.enabled } : x)))
        // Keep the editor's own "current enabled" in sync if this is the
        // rule it has loaded -- otherwise a Save right after toggling from
        // the list would still send the pre-toggle value.
        if (editingId === r.id) setEditingEnabled(!r.enabled)
      } else {
        setActionError(r.id, res.status === 401 ? 'unauthorized — set the hub key in Config' : 'failed')
      }
    } catch {
      setActionError(r.id, 'hub not reachable')
    }
    setEnablingId(null)
  }

  async function onDelete(id) {
    const r = rules.find((x) => x.id === id)
    if (!confirm(`Delete rule "${r ? r.name : id}"? This cannot be undone.`)) return
    setDeletingId(id)
    setActionError(id, '')
    try {
      const res = await fetch(`/api/v1/rules/${id}`, { method: 'DELETE', headers: authHeaders() })
      if (res.ok) {
        setRules((prev) => prev.filter((x) => x.id !== id))
        if (editingId === id) startNewRule()
      } else {
        setActionError(id, res.status === 401 ? 'unauthorized — set the hub key in Config' : 'delete failed')
      }
    } catch {
      setActionError(id, 'hub not reachable')
    }
    setDeletingId(null)
  }

  async function onTest(id) {
    setTestingId(id)
    setActionError(id, '')
    try {
      const res = await fetch(`/api/v1/rules/${id}/test`, { method: 'POST', headers: authHeaders() })
      if (res.ok) {
        const d = await res.json()
        setTestResults((prev) => ({ ...prev, [id]: d }))
        setOpenMap((prev) => ({ ...prev, [id]: true }))
      } else {
        setActionError(id, res.status === 401 ? 'unauthorized — set the hub key in Config' : 'test failed')
      }
    } catch {
      setActionError(id, 'hub not reachable')
    }
    setTestingId(null)
  }

  async function onEdit(id) {
    try {
      const res = await fetch(`/api/v1/rules/${id}`)
      if (!res.ok) { setActionError(id, 'failed to load'); return }
      const d = await res.json()
      setEditingId(id)
      setEditingEnabled(!!d.enabled)
      setSource(d.source || '')
      setCompileResult(null)
      setSaveState('idle')
      setSaveMsg('')
    } catch {
      setActionError(id, 'hub not reachable')
    }
  }

  function startNewRule() {
    setEditingId(null)
    setEditingEnabled(true)
    setSource(TEMPLATE)
    setCompileResult(null)
    setSaveState('idle')
    setSaveMsg('')
  }

  function onSourceChange(v) {
    setSource(v)
    // Any edit invalidates the last Compile -- Save stays gated on a clean
    // compile of the CURRENT text, never a stale one.
    setCompileResult(null)
    setSaveState('idle')
    setSaveMsg('')
  }

  function onCompile() {
    setCompileResult(compile(source))
  }

  async function onSave() {
    if (!compileResult || !compileResult.ok) return
    setSaveState('saving')
    setSaveMsg('')
    const body = {
      name: compileResult.name,
      source,
      bytecode_b64: btoa(String.fromCharCode(...compileResult.bytecode)),
      enabled: editingId != null ? editingEnabled : true,
      mode: compileResult.mode === 'level' ? 1 : 0,
      cooldown_s: compileResult.cooldown_s,
      every_s: compileResult.every_s,
    }
    try {
      const res = await fetch(editingId ? `/api/v1/rules/${editingId}` : '/api/v1/rules', {
        method: editingId ? 'PUT' : 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify(body),
      })
      if (res.ok) {
        const d = await res.json().catch(() => ({}))
        if (!editingId && d.id) setEditingId(d.id)
        setSaveState('saved')
        refreshRules().catch(() => {})
      } else if (res.status === 401) {
        setSaveState('unauth')
      } else {
        const d = await res.json().catch(() => ({}))
        setSaveState('error')
        setSaveMsg(d.error || 'save failed')
      }
    } catch {
      setSaveState('error')
      setSaveMsg('hub not reachable')
    }
  }

  return (
    <div>
      <div class="panel">
        <h2>Rules</h2>
        {error && <p class="error">Hub not reachable.</p>}
        {!error && !rules && <p class="placeholder">Loading…</p>}
        {!error && rules && rules.length === 0 && (
          <p class="placeholder">No rules yet — write one below and Save.</p>
        )}
        {!error && rules && rules.length > 0 && (
          <div class="node-cards">
            {rules.map((r) => (
              <RuleCard key={r.id} r={r} open={!!openMap[r.id]} onToggle={() => toggleRule(r.id)}
                        onToggleEnabled={onToggleEnabled} enabling={enablingId === r.id}
                        onEdit={onEdit} onDelete={onDelete} deleting={deletingId === r.id}
                        onTest={onTest} testing={testingId === r.id} testResult={testResults[r.id]}
                        actionError={actionErrors[r.id]} />
            ))}
          </div>
        )}
      </div>

      <div class="panel">
        <h2>{editingId != null ? `Edit rule #${editingId}` : 'New rule'}</h2>
        <textarea class="rule-source mono" rows={12} value={source} spellcheck={false}
                  onInput={(e) => onSourceChange(e.currentTarget.value)} />
        <p>
          <button type="button" class="btn-primary" onClick={onCompile}>Compile</button>
          {' '}
          <button type="button" class="btn-primary" onClick={onSave}
                  disabled={!compileResult || !compileResult.ok || saveState === 'saving'}>
            {saveState === 'saving' ? 'Saving…' : 'Save'}
          </button>
          {' '}
          {editingId != null && <button type="button" onClick={startNewRule}>New rule</button>}
          {saveState === 'saved' && <span class="hint"> Saved.</span>}
          {saveState === 'unauth' && <span class="error"> unauthorized — set the hub key in Config</span>}
          {saveState === 'error' && <span class="error"> {saveMsg}</span>}
        </p>
        {compileResult && !compileResult.ok && (
          <ul class="error">
            {compileResult.errors.map((e, i) => <li key={i}>{e.line}:{e.col} {e.message}</li>)}
          </ul>
        )}
        {compileResult && compileResult.ok && (
          <div class="infobox">
            <p>
              mode {compileResult.mode} · cooldown {compileResult.cooldown_s}s
              {compileResult.every_s ? ` · every ${compileResult.every_s}s` : ' · no periodic evaluation'}
            </p>
            <details>
              <summary>Disassembly</summary>
              <pre class="mono">{disassemble(compileResult.bytecode)}</pre>
            </details>
          </div>
        )}
      </div>

      <EventFeed />
    </div>
  )
}
