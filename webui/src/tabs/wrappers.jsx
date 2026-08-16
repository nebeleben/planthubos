import { useEffect, useRef, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'
import { compileWrapper, disassemble } from '../lib/psc/index.js'
import { getAiSettings, hasAiKey } from '../lib/ai/settings.js'
import { aiComplete, AiError } from '../lib/ai/provider.js'
import { WRAPPER_TEMPLATE } from '../lib/ai/prompts/wrapper.js'
import { extractSource, provenance } from '../lib/ai/extract.js'
import { sliceWrapperPayload } from '../lib/ai/payload.js'

// Same "duplicated locally, not shared" idiom every tab's own fmtAge copy
// follows (rules.jsx/devices.jsx/nodes.jsx) -- last_seen_s/last_fire_age_s
// style fields all come back as SECONDS AGO already.
function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

// GET /api/v1/wrappers's match.kind is a STRING (api_v1.c's wmatch_kind_str)
// -- unlike compileWrapper()'s NUMERIC match.kind (wmatch_kind_t). Never mix
// the two: this file only ever reads the string form off list/get responses
// and only ever reads the numeric form off a fresh compileWrapper() result.
const MATCH_KEY_WIDTH = { service: 4, manufacturer: 4, mac_prefix: 6 }

function fmtMatchKey(match) {
  const width = MATCH_KEY_WIDTH[match.kind] || 4
  return `0x${Number(match.key).toString(16).toUpperCase().padStart(width, '0')}`
}

// Spec §3's Ruuvi example (also the wrapper.test.mjs golden), commented out
// so Compile isn't the first thing a blank editor demands -- same reasoning
// and shape as rules.jsx's own TEMPLATE.
const TEMPLATE = `# wrapper "ruuvi" match manufacturer 0x0499
# decode
#   payload starts after the match's own header (manufacturer: past the
#   2-byte company id; service: past the 2-byte UUID; mac_prefix: raw advert)
#   emit air.temperature   i16_be(payload, 1) * 0.005
#   emit air.humidity      u16_be(payload, 3) * 0.0025
`

// POST /wrappers/<id>/test's response (spec §6, ruling: render exactly what
// the endpoint returns -- cap name/unit are already joined server-side, no
// second /api/v1/capabilities round trip here).
function WrapperTestResultBlock({ result }) {
  return (
    <div class="infobox">
      <p>{result.ok ? 'ok' : `error — ${result.error || 'dry-run failed'}`}</p>
      {result.emits && result.emits.length > 0 && (
        <div class="table-scroll">
          <table class="devices">
            <thead><tr><th>Capability</th><th>Value</th><th>Unit</th></tr></thead>
            <tbody>
              {result.emits.map((e, i) => (
                <tr key={i}>
                  <td>{e.name}</td>
                  <td>{e.value}</td>
                  <td>{e.unit}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {result.emits && result.emits.length === 0 && <p class="hint">No capabilities emitted.</p>}
    </div>
  )
}

// Same collapsible node-card shape as rules.jsx's RuleCard: header carries
// name + match + a compact status word, body carries the enabled toggle,
// match_count/last_error, Edit/Test/Delete, an optional hex override for the
// dry-run, and (once run) the last Test result.
function WrapperCard({ w, open, onToggle, onToggleEnabled, enabling, onEdit, onDelete, deleting,
                       onTest, testing, testHex, onTestHexChange, testResult, actionError }) {
  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{w.name}</span>
          <span class="node-card-mac mono">{w.match.kind} {fmtMatchKey(w.match)}</span>
        </span>
        <span class={`node-card-age hint${w.enabled && w.last_error ? ' error' : ''}`}>
          {w.enabled ? (w.last_error ? 'error' : 'ok') : 'disabled'}
        </span>
      </button>
      <div class="node-card-body">
        <div class="node-card-row">
          <label>
            <input type="checkbox" checked={w.enabled} disabled={enabling}
                   onChange={() => onToggleEnabled(w)} />
            {' '}Enabled
          </label>
        </div>
        <div class="node-card-row">
          <span class="hint">
            matched {w.match_count}×{w.last_error ? ` · ${w.last_error}` : ''}
          </span>
        </div>
        {actionError && <div class="node-card-row"><span class="error">{actionError}</span></div>}
        <div class="node-card-row">
          <input class="mono" value={testHex} onInput={(e) => onTestHexChange(w.id, e.currentTarget.value)}
                 placeholder="hex payload override (optional — defaults to a captured sample)" />
        </div>
        <div class="node-card-footer">
          <button type="button" onClick={() => onEdit(w.id)}>Edit</button>
          <button type="button" class="btn-primary" onClick={() => onTest(w.id)} disabled={testing}>
            {testing ? '…' : 'Test'}
          </button>
          <button type="button" class="btn-destructive" onClick={() => onDelete(w.id)} disabled={deleting}>
            {deleting ? '…' : 'Delete'}
          </button>
        </div>
        {testResult && <WrapperTestResultBlock result={testResult} />}
      </div>
    </div>
  )
}

// prefill/onPrefillConsumed: cross-tab "Add wrapper" / "Generate wrapper
// with AI" hop from the Devices tab's Unknown devices section (app.jsx owns
// the value, per the controller ruling -- localStorage is rules.jsx's own
// unrelated unseen-events-badge mechanism, not a pattern to copy here).
// {hex, device}: hex prefills the editor exactly as M3 Task 8 always did,
// device is the whole raw GET /api/v1/unknown entry M4 Task 7 adds, held
// onto as `capturedDevice` for the Generate button (WRAPPER_TEMPLATE.build
// needs every sample, not just one) and for Step 4's auto-test (needs a
// captured sample to slice and send). Consumed once on mount/change, then
// cleared via onPrefillConsumed() so a later plain visit to this tab
// doesn't re-apply a stale prefill.
export function WrappersTab({ prefill, onPrefillConsumed }) {
  const [wrappers, setWrappers] = useState(null)
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})
  const [testResults, setTestResults] = useState({})
  const [testHexMap, setTestHexMap] = useState({})
  const [testingId, setTestingId] = useState(null)
  const [enablingId, setEnablingId] = useState(null)
  const [deletingId, setDeletingId] = useState(null)
  const [actionErrors, setActionErrors] = useState({})

  const [editingId, setEditingId] = useState(null)   // null = new wrapper
  // Same "preserve whatever the list's toggle last set" reasoning as
  // rules.jsx's editingEnabled: captured off the wrapper's own GET when Edit
  // loads it in, so a Save right after toggling from the list doesn't revert
  // it. Brand-new wrappers (editingId == null) always save enabled.
  const [editingEnabled, setEditingEnabled] = useState(true)
  const [source, setSource] = useState(TEMPLATE)
  const [compileResult, setCompileResult] = useState(null)
  const [saveState, setSaveState] = useState('idle')  // idle | saving | saved | error | unauth
  const [saveMsg, setSaveMsg] = useState('')

  // M4 Task 7: the device this editor session was prefilled from (or null
  // for a from-scratch/Edit-an-existing-wrapper session). Drives both the
  // Generate button (needs every captured sample to build a prompt) and
  // Step 4's auto-test-on-clean-compile (needs a captured sample to slice
  // and POST to /api/v1/wrappers/test). Cleared on New/Edit so an unrelated
  // wrapper never gets auto-tested against a stale device's bytes.
  const [capturedDevice, setCapturedDevice] = useState(null)
  const [aiBusy, setAiBusy] = useState(false)
  const [aiError, setAiError] = useState(null)          // {message, detail} | null
  const [aiRawResponse, setAiRawResponse] = useState(null)  // set only when extractSource() found no code block
  const [autoTestResult, setAutoTestResult] = useState(null)
  const [autoTestBusy, setAutoTestBusy] = useState(false)
  const [autoTestError, setAutoTestError] = useState('')
  const abortRef = useRef(null)   // current in-flight aiComplete()'s AbortController, for the Cancel button

  function refreshWrappers(signal) {
    return fetch('/api/v1/wrappers', { signal })
      .then((r) => r.json())
      .then((d) => setWrappers(d.wrappers || []))
  }

  useEffect(() => {
    const controller = new AbortController()
    refreshWrappers(controller.signal).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Background keep-fresh poll, same 10s cadence/discipline as rules.jsx's
  // own -- match_count/last_error change purely from decoder activity the
  // operator didn't initiate here.
  useEffect(() => {
    const controller = new AbortController()
    const id = setInterval(() => refreshWrappers(controller.signal).catch(() => {}), 10000)
    return () => { clearInterval(id); controller.abort() }
  }, [])

  // Clears every piece of AI-generation state (Generate result, any error,
  // the auto-test) -- shared by startNewWrapper/onEdit/the prefill effect
  // below, all of which start a session an old generation/test no longer
  // applies to. Aborts a still-in-flight aiComplete() first: a response
  // that lands after the editor has moved on to a different wrapper must
  // never write into it.
  function resetAiState() {
    if (abortRef.current) abortRef.current.abort()
    // Nulled (not just aborted) so a since-superseded onGenerate's own
    // catch handler -- which checks `abortRef.current === <its own
    // controller>` before touching state -- recognises this session as
    // stale and stays silent, instead of painting a "Generation cancelled"
    // message into whatever New-wrapper/Edit/prefill session replaced it.
    abortRef.current = null
    setAiBusy(false)
    setAiError(null)
    setAiRawResponse(null)
    setAutoTestResult(null)
    setAutoTestBusy(false)
    setAutoTestError('')
  }

  function startNewWrapper() {
    setEditingId(null)
    setEditingEnabled(true)
    setSource(TEMPLATE)
    setCompileResult(null)
    setSaveState('idle')
    setSaveMsg('')
    setCapturedDevice(null)
    resetAiState()
  }

  // Consumes app.jsx's prefill exactly once: drops the captured hex in as a
  // leading comment above the usual blank-editor template, then hands the
  // value back so a later plain tab switch doesn't re-apply it. Also keeps
  // the whole device around (capturedDevice) for the Generate button and
  // Step 4's auto-test, both M4 Task 7 additions -- prefillHex alone (the
  // M3 shape) was never enough to build a prompt from every sample.
  //
  // prefill.autoGenerate (fix round 1): the Devices tab's "Generate wrapper
  // with AI" button, unlike its "Add wrapper" sibling, sets this so
  // generation starts the moment this session lands here -- the operator
  // already clicked a control that says Generate, so that click is the
  // confirmation, not this effect. onGenerate(prefill.device) is called
  // with the device directly rather than relying on setCapturedDevice(...)
  // above having taken effect (it hasn't, in this same tick).
  useEffect(() => {
    if (prefill == null) return
    setEditingId(null)
    setEditingEnabled(true)
    setSource(`# captured payload: ${prefill.hex}\n${TEMPLATE}`)
    setCompileResult(null)
    setSaveState('idle')
    setSaveMsg('')
    setCapturedDevice(prefill.device || null)
    resetAiState()
    onPrefillConsumed()
    if (prefill.autoGenerate && prefill.device && hasAiKey()) {
      onGenerate(prefill.device)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [prefill])

  function toggleWrapper(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  function setActionError(id, msg) {
    setActionErrors((prev) => ({ ...prev, [id]: msg }))
  }

  // No dedicated /enable route exists for wrappers (unlike rules) -- PUT
  // carries the full {name, source, bytecode_b64, enabled} body, so toggling
  // from the list has to fetch the stored source, recompile it client-side
  // (it compiled fine when it was last saved), and PUT the result back with
  // only `enabled` flipped.
  async function onToggleEnabled(w) {
    setEnablingId(w.id)
    setActionError(w.id, '')
    try {
      const getRes = await fetch(`/api/v1/wrappers/${w.id}`)
      if (!getRes.ok) { setActionError(w.id, 'failed to load'); setEnablingId(null); return }
      const full = await getRes.json()
      const compiled = compileWrapper(full.source || '')
      if (!compiled.ok) {
        setActionError(w.id, 'stored source no longer compiles — open Edit to fix it')
        setEnablingId(null)
        return
      }
      const res = await fetch(`/api/v1/wrappers/${w.id}`, {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({
          name: compiled.name,
          source: full.source,
          bytecode_b64: btoa(String.fromCharCode(...compiled.bytecode)),
          enabled: !w.enabled,
        }),
      })
      if (res.ok) {
        setWrappers((prev) => prev.map((x) => (x.id === w.id ? { ...x, enabled: !w.enabled } : x)))
        if (editingId === w.id) setEditingEnabled(!w.enabled)
      } else {
        setActionError(w.id, res.status === 401 ? 'unauthorized — set the hub key in Config' : 'failed')
      }
    } catch {
      setActionError(w.id, 'hub not reachable')
    }
    setEnablingId(null)
  }

  async function onDelete(id) {
    const w = wrappers.find((x) => x.id === id)
    if (!confirm(`Delete wrapper "${w ? w.name : id}"? This cannot be undone.`)) return
    setDeletingId(id)
    setActionError(id, '')
    try {
      const res = await fetch(`/api/v1/wrappers/${id}`, { method: 'DELETE', headers: authHeaders() })
      if (res.ok) {
        setWrappers((prev) => prev.filter((x) => x.id !== id))
        if (editingId === id) startNewWrapper()
      } else {
        setActionError(id, res.status === 401 ? 'unauthorized — set the hub key in Config' : 'delete failed')
      }
    } catch {
      setActionError(id, 'hub not reachable')
    }
    setDeletingId(null)
  }

  function onTestHexChange(id, v) {
    setTestHexMap((prev) => ({ ...prev, [id]: v }))
  }

  async function onTest(id) {
    setTestingId(id)
    setActionError(id, '')
    try {
      const hex = (testHexMap[id] || '').trim()
      const res = await fetch(`/api/v1/wrappers/${id}/test`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify(hex ? { hex } : {}),
      })
      const d = await res.json().catch(() => ({}))
      if (res.ok) {
        setTestResults((prev) => ({ ...prev, [id]: d }))
        setOpenMap((prev) => ({ ...prev, [id]: true }))
      } else {
        setActionError(id, res.status === 401 ? 'unauthorized — set the hub key in Config' : (d.error || 'test failed'))
      }
    } catch {
      setActionError(id, 'hub not reachable')
    }
    setTestingId(null)
  }

  async function onEdit(id) {
    try {
      const res = await fetch(`/api/v1/wrappers/${id}`)
      if (!res.ok) { setActionError(id, 'failed to load'); return }
      const d = await res.json()
      setEditingId(id)
      setEditingEnabled(!!d.enabled)
      setSource(d.source || '')
      setCompileResult(null)
      setSaveState('idle')
      setSaveMsg('')
      // Editing an existing wrapper by id has no associated captured device
      // -- carrying one forward from a previous prefill would auto-test
      // this unrelated wrapper against a stale sample (see resetAiState's
      // own doc comment).
      setCapturedDevice(null)
      resetAiState()
    } catch {
      setActionError(id, 'hub not reachable')
    }
  }

  function onSourceChange(v) {
    setSource(v)
    // Any edit invalidates the last Compile -- Save stays gated on a clean
    // compile of the CURRENT text, never a stale one. Same reasoning
    // extends to a stale Generate error/result and a stale auto-test: none
    // of them describe the text now in the editor.
    setCompileResult(null)
    setSaveState('idle')
    setSaveMsg('')
    setAiError(null)
    setAiRawResponse(null)
    setAutoTestResult(null)
    setAutoTestError('')
  }

  // Compiles `src` and, if that succeeds and a captured device is on hand,
  // auto-tests it (Step 4, spec §6/M4 task-7-brief: "the milestone's proof
  // path") -- shared by the Compile button and Generate's own "triggers the
  // existing Compile path" step, so both a hand-written and an AI-written
  // wrapper get the identical verification once they compile clean.
  // `device` defaults to the render-time capturedDevice (right for a direct
  // button click) but Generate passes its own device explicitly: it may
  // resolve after an await, by which point the render that created THIS
  // closure is stale and its captured `capturedDevice` may not be -- a
  // plain function parameter can't go stale the way a closed-over state
  // variable can.
  function runCompile(src, device = capturedDevice) {
    const result = compileWrapper(src)
    setCompileResult(result)
    setAutoTestResult(null)
    setAutoTestError('')
    const samples = device?.samples
    if (result.ok && samples && samples.length > 0) {
      runAutoTest(result, samples[samples.length - 1].hex)
    }
    return result
  }

  function onCompile() {
    runCompile(source)
  }

  // Step 4: proves a clean compile against the real bytes it was written
  // for, using sliceWrapperPayload() (lib/ai/payload.js's own doc comment)
  // so the wrapper sees exactly what the firmware would hand it. Writes
  // nothing and installs nothing (POST /wrappers/test, not
  // /wrappers) -- this is a dry run the operator reviews, same as the
  // per-wrapper Test button below already does for saved wrappers.
  async function runAutoTest(compiled, capturedHex) {
    const hex = sliceWrapperPayload(capturedHex, compiled.match.kind)
    setAutoTestBusy(true)
    setAutoTestError('')
    try {
      const res = await fetch('/api/v1/wrappers/test', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ bytecode_b64: btoa(String.fromCharCode(...compiled.bytecode)), hex }),
      })
      const d = await res.json().catch(() => ({}))
      if (res.ok) {
        setAutoTestResult(d)
      } else {
        setAutoTestError(res.status === 401 ? 'unauthorized — set the hub key in Config' : (d.error || 'test failed'))
      }
    } catch {
      setAutoTestError('hub not reachable')
    }
    setAutoTestBusy(false)
  }

  // Step 3: builds the prompt from the whole captured device (redact.js
  // does the actual redaction, inside WRAPPER_TEMPLATE.build -- this
  // function never touches device fields itself), calls the provider with
  // an AbortController so the Cancel button below can interrupt a
  // tens-of-seconds request, and on success runs the identical path a
  // human pasting code in would: extractSource -> prepend provenance ->
  // into the editor -> runCompile(). No code path here reaches
  // POST /api/v1/wrappers -- onSave (untouched) is still the only one.
  //
  // `device` defaults to capturedDevice for the button's own onClick, but
  // the Devices-tab "Generate wrapper with AI" hop calls this directly from
  // the prefill-consuming effect with the device it already has in hand
  // (fix round 1: that button used to just prefill and switch tabs like
  // "Add wrapper" does, identically -- the click IS the confirmation to
  // spend an API call, so it now starts one). Passing it explicitly avoids
  // the effect's setCapturedDevice(...) call above racing this function's
  // own capturedDevice closure, which wouldn't have re-rendered yet.
  // Guards on abortRef.current, not the aiBusy render-state closure, for
  // the same reason: aiBusy read through this function's own closure can
  // be stale in exactly the same window.
  async function onGenerate(device) {
    const dev = device || capturedDevice
    if (!dev || abortRef.current) return
    setAiError(null)
    setAiRawResponse(null)
    setAiBusy(true)
    const controller = new AbortController()
    abortRef.current = controller
    const settings = getAiSettings()
    let text
    try {
      const { system, user } = WRAPPER_TEMPLATE.build({ device: dev })
      text = await aiComplete({ system, user, signal: controller.signal, settings })
    } catch (err) {
      // Superseded by a New wrapper/Edit/prefill mid-flight (resetAiState
      // nulls abortRef.current, see its own doc comment) -- that session
      // already painted over this one, so stay silent rather than clobber
      // it with a stale "cancelled" message.
      if (abortRef.current === controller) {
        if (err instanceof AiError) setAiError({ message: err.message, detail: err.detail })
        setAiBusy(false)
        abortRef.current = null
      }
      return
    }
    if (abortRef.current !== controller) return   // same supersede check, success side
    const extracted = extractSource(text)
    if (extracted == null) {
      setAiRawResponse(text)
    } else {
      const generated = `${provenance('wrapper', WRAPPER_TEMPLATE.version, settings.model)}\n${extracted}`
      setSource(generated)
      setSaveState('idle')
      setSaveMsg('')
      runCompile(generated, dev)
    }
    setAiBusy(false)
    abortRef.current = null
  }

  function onCancelGenerate() {
    if (abortRef.current) abortRef.current.abort()
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
    }
    try {
      const res = await fetch(editingId ? `/api/v1/wrappers/${editingId}` : '/api/v1/wrappers', {
        method: editingId ? 'PUT' : 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify(body),
      })
      if (res.ok) {
        const d = await res.json().catch(() => ({}))
        if (!editingId && d.id) setEditingId(d.id)
        setSaveState('saved')
        refreshWrappers().catch(() => {})
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
        <h2>Wrappers</h2>
        {error && <p class="error">Hub not reachable.</p>}
        {!error && !wrappers && <p class="placeholder">Loading…</p>}
        {!error && wrappers && wrappers.length === 0 && (
          <p class="placeholder">No wrappers yet — write one below and Save.</p>
        )}
        {!error && wrappers && wrappers.length > 0 && (
          <div class="node-cards">
            {wrappers.map((w) => (
              <WrapperCard key={w.id} w={w} open={!!openMap[w.id]} onToggle={() => toggleWrapper(w.id)}
                           onToggleEnabled={onToggleEnabled} enabling={enablingId === w.id}
                           onEdit={onEdit} onDelete={onDelete} deleting={deletingId === w.id}
                           onTest={onTest} testing={testingId === w.id}
                           testHex={testHexMap[w.id] || ''} onTestHexChange={onTestHexChange}
                           testResult={testResults[w.id]} actionError={actionErrors[w.id]} />
            ))}
          </div>
        )}
      </div>

      <div class="panel">
        <h2>{editingId != null ? `Edit wrapper #${editingId}` : 'New wrapper'}</h2>
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
          {editingId != null && <button type="button" onClick={startNewWrapper}>New wrapper</button>}
          {' '}
          {/* Generate only appears once a captured device is on hand (the
              Devices tab hop) -- WRAPPER_TEMPLATE.build({device}) has
              nothing to redact/prompt from otherwise. hasAiKey() gate and
              title mirror the Devices tab's own "Generate wrapper with AI"
              button so a user hits the same explanation wherever they meet
              the feature disabled. */}
          {capturedDevice && (
            aiBusy ? (
              <button type="button" onClick={onCancelGenerate}>Cancel</button>
            ) : (
              <button type="button" onClick={() => onGenerate()} disabled={!hasAiKey()}
                      title={hasAiKey() ? undefined : 'Set an API key in Config to use AI generation'}>
                Generate with AI
              </button>
            )
          )}
          {aiBusy && <span class="hint"> Generating… this can take up to a minute.</span>}
          {saveState === 'saved' && <span class="hint"> Saved.</span>}
          {saveState === 'unauth' && <span class="error"> unauthorized — set the hub key in Config</span>}
          {saveState === 'error' && <span class="error"> {saveMsg}</span>}
        </p>
        {aiError && (
          <div class="infobox">
            <p class="error">{aiError.message}</p>
            {aiError.detail && (
              <details><summary>Details</summary><pre class="mono">{aiError.detail}</pre></details>
            )}
          </div>
        )}
        {aiRawResponse != null && (
          <div class="infobox">
            <p class="error">The model did not return a code block.</p>
            <details><summary>Raw response</summary><pre class="mono">{aiRawResponse}</pre></details>
          </div>
        )}
        {compileResult && !compileResult.ok && (
          <ul class="error">
            {compileResult.errors.map((e, i) => <li key={i}>{e.line}:{e.col} {e.message}</li>)}
          </ul>
        )}
        {compileResult && compileResult.ok && (
          <div class="infobox">
            <p>
              match {compileResult.match.kind === 0 ? 'service' : compileResult.match.kind === 1 ? 'manufacturer' : 'mac_prefix'}{' '}
              0x{compileResult.match.key.toString(16).toUpperCase()} · uses {compileResult.capsUsed.length} capabilit{compileResult.capsUsed.length === 1 ? 'y' : 'ies'}
            </p>
            <details>
              <summary>Disassembly</summary>
              <pre class="mono">{disassemble(compileResult.bytecode)}</pre>
            </details>
          </div>
        )}
        {/* Step 4: proof-path auto-test against the captured sample this
            editor session was prefilled from -- see runCompile()'s own doc
            comment for why this fires from both Compile and Generate. */}
        {autoTestBusy && <p class="hint">Testing against captured sample…</p>}
        {autoTestError && <p class="error">{autoTestError}</p>}
        {autoTestResult && <WrapperTestResultBlock result={autoTestResult} />}
      </div>
    </div>
  )
}
