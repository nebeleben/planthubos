import { useEffect, useRef, useState } from 'preact/hooks'
import { getKey, setKey, authHeaders } from '../lib/auth.js'
import { getAiSettings, setAiSettings, normEndpoint, AI_DEFAULTS } from '../lib/ai/settings.js'
import { aiComplete, AiError } from '../lib/ai/provider.js'

function fmtBytes(n) {
  if (n == null) return '–'
  if (n > 1048576) return `${(n / 1048576).toFixed(1)} MB`
  if (n > 1024) return `${(n / 1024).toFixed(0)} KB`
  return `${n} B`
}

function fmtUptime(s) {
  if (s == null) return '–'
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60)
  return d > 0 ? `${d}d ${h}h` : h > 0 ? `${h}h ${m}m` : `${m}m`
}

export function ConfigTab() {
  const [st, setSt] = useState(null)
  const [error, setError] = useState(false)
  const [secret, setSecret] = useState(null)     // freshly generated, shown once
  const [keyInput, setKeyInput] = useState(getKey())
  const [busy, setBusy] = useState('')           // '' | claim | unclaim | ota | pair | retry | switch | config
  const [otaMsg, setOtaMsg] = useState('')
  const [otaPct, setOtaPct] = useState(null)
  const xhrRef = useRef(null)

  const [cfg, setCfg] = useState(null)
  const [cfgLoaded, setCfgLoaded] = useState(false)   // only true after a successful GET — guards Save
  const [cfgLoadError, setCfgLoadError] = useState(false)
  const [mqtt, setMqtt] = useState({ enabled: false, uri: '', user: '', pass: '' })
  const [influx, setInflux] = useState({ enabled: false, url: '', org: '', bucket: '', token: '' })
  const [cfgMsg, setCfgMsg] = useState('')
  const [region, setRegion] = useState('')           // '' = build default (GET's null)
  const [regionMsg, setRegionMsg] = useState('')
  const [hubName, setHubName] = useState('')
  const [nameMsg, setNameMsg] = useState('')
  const [fresetMsg, setFresetMsg] = useState('')

  // AI assist (M4): browser-local only -- read once from localStorage via
  // settings.js, written back the same way. Never touches the hub.
  const [ai, setAi] = useState(() => getAiSettings())
  const [aiMsg, setAiMsg] = useState('')
  const [aiTestMsg, setAiTestMsg] = useState('')
  const [aiTestDetail, setAiTestDetail] = useState('')

  // Every successful config save reboots the hub (the settings only apply
  // at boot). One shared countdown: tell the user, then pull the page back
  // up once the hub should be reachable again.
  function rebootCountdown(setMsg, extra) {
    setMsg(`Saved — hub is rebooting to apply.${extra ? ` ${extra}` : ''} This page reloads in ~15 s.`)
    setTimeout(() => location.reload(), 15000)
  }

  function refresh() {
    fetch('/api/v1/status')
      .then((r) => r.json())
      .then((s) => { setSt(s); setError(false) })
      .catch(() => setError(true))
  }
  useEffect(() => { refresh() }, [])

  function refreshConfig() {
    setCfgLoaded(false); setCfgLoadError(false)
    fetch('/api/v1/config')
      .then((r) => r.json())
      .then((c) => {
        setCfg(c)
        setMqtt({ enabled: c.mqtt.enabled, uri: c.mqtt.uri, user: c.mqtt.user, pass: '' })
        setInflux({ enabled: c.influx.enabled, url: c.influx.url, org: c.influx.org, bucket: c.influx.bucket, token: '' })
        setRegion(c.region ?? '')   // null (build default) -> the select's "" option
        setHubName(c.name ?? '')
        setCfgLoaded(true)
      })
      // Leave cfgLoaded false on failure: the form would otherwise render
      // blank defaults, and a wholesale-replace Save would wipe the real
      // stored config with zeros. Save stays disabled until a load succeeds.
      .catch(() => setCfgLoadError(true))
  }
  useEffect(() => { refreshConfig() }, [])

  // The hub's role_change_ok() gate requires either AP mode (this
  // device's own setup network) or a valid claim key. A pair-failed node
  // that still holds WiFi credentials rejoins as a normal STA, so it's
  // never in AP mode -- and if it's also unclaimed (the common case for a
  // node), no key typed here can ever satisfy that gate over the network;
  // recovery needs physical access instead. `claimed` distinguishes what
  // we can: a genuinely claimed hub really does just need the right key.
  function role401Message(claimed) {
    return claimed
      ? 'Unauthorized — wrong key.'
      : "Can't do this remotely — this device already rejoined your WiFi, and changing its role now requires physical access. Hold the BOOT button for 10 seconds to factory-reset it back to the setup portal."
  }

  async function doRetryPairing() {
    setBusy('retry')
    try {
      const res = await fetch('/api/v1/pair/retry', { method: 'POST', headers: authHeaders() })
      if (res.ok) {
        alert('Retrying — this device will restart and search for your hub again.')
      } else {
        alert(res.status === 401 ? role401Message(st.claimed) : 'retry failed')
      }
    } catch { alert('hub not reachable') }
    setBusy('')
  }

  async function doSwitchToMain() {
    if (!confirm('Switch this device back to a main hub? It will restart.')) return
    setBusy('switch')
    try {
      const res = await fetch('/api/v1/role', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ role: 'main' }),
      })
      if (res.ok) {
        alert('Switching to main hub — this device will restart.')
      } else {
        alert(res.status === 401 ? role401Message(st.claimed) : 'switch failed')
      }
    } catch { alert('hub not reachable') }
    setBusy('')
  }

  async function doClaim() {
    setBusy('claim')
    try {
      const res = await fetch('/api/v1/claim', { method: 'POST' })
      const data = await res.json()
      if (res.ok && data.secret) {
        setSecret(data.secret)
        setKey(data.secret)          // this browser becomes the owner
        setKeyInput(data.secret)
        refresh()
      } else {
        alert(data.error || 'claim failed')
      }
    } catch { alert('hub not reachable') }
    setBusy('')
  }

  async function doUnclaim() {
    if (!confirm('Unclaim this hub? Releases your ownership — the hub stays reachable, and anyone on your network can claim it.')) return
    setBusy('unclaim')
    try {
      const res = await fetch('/api/v1/unclaim', { method: 'POST', headers: authHeaders() })
      if (res.ok) { setKey(''); setKeyInput(''); setSecret(null); refresh() }
      else alert(res.status === 401 ? 'unauthorized — wrong key' : 'unclaim failed')
    } catch { alert('hub not reachable') }
    setBusy('')
  }

  function doOta(e) {
    const file = e.currentTarget.files[0]
    e.currentTarget.value = ''                // allow re-selecting the same file after a failure
    if (!file) return
    setBusy('ota'); setOtaMsg(''); setOtaPct(0)
    const xhr = new XMLHttpRequest()          // XHR for upload progress
    xhrRef.current = xhr
    xhr.open('POST', '/api/v1/ota')
    xhr.timeout = 120000
    const key = getKey()
    if (key) xhr.setRequestHeader('Authorization', `Bearer ${key}`)
    xhr.upload.onprogress = (ev) => ev.lengthComputable && setOtaPct(Math.round(100 * ev.loaded / ev.total))
    xhr.onload = () => {
      xhrRef.current = null
      setBusy('')
      if (xhr.status === 200) setOtaMsg('Update accepted — hub is rebooting. Reload this page in ~20 s.')
      else if (xhr.status === 401) setOtaMsg('Unauthorized — set the hub key above.')
      else setOtaMsg(`Update failed (${xhr.status}).`)
    }
    xhr.onerror = () => { xhrRef.current = null; setBusy(''); setOtaMsg('Upload failed — hub not reachable.') }
    xhr.ontimeout = () => { xhrRef.current = null; setBusy(''); setOtaMsg('Upload timed out.') }
    xhr.onabort = () => { xhrRef.current = null; setBusy(''); setOtaMsg('Upload cancelled.') }
    xhr.send(file)
  }

  function cancelOta() {
    xhrRef.current?.abort()
  }

  async function doSaveIntegrations(e) {
    e.preventDefault()
    if (!cfgLoaded) return   // never wholesale-replace over an unloaded/failed config fetch
    setBusy('config'); setCfgMsg('')
    // The hub wholesale-replaces a present section's non-secret fields, so
    // both sections must always be sent complete -- omitting the secret
    // field entirely (never blank) is the only way to "leave it unchanged".
    const body = {
      mqtt: { enabled: mqtt.enabled, uri: mqtt.uri, user: mqtt.user },
      influx: { enabled: influx.enabled, url: influx.url, org: influx.org, bucket: influx.bucket },
    }
    if (mqtt.pass) body.mqtt.pass = mqtt.pass
    if (influx.token) body.influx.token = influx.token
    try {
      const res = await fetch('/api/v1/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify(body),
      })
      if (res.ok) {
        setMqtt((m) => ({ ...m, pass: '' }))
        setInflux((i) => ({ ...i, token: '' }))
        rebootCountdown(setCfgMsg)
      } else if (res.status === 401) {
        setCfgMsg('Unauthorized — set the hub key above.')
      } else {
        // Transport-level 400s may not be JSON, so fall back to a generic message.
        let msg = 'Save failed.'
        try { const d = await res.json(); if (d.error) msg = d.error } catch { /* non-JSON body */ }
        setCfgMsg(msg)
      }
    } catch { setCfgMsg('hub not reachable') }
    setBusy('')
  }

  async function doSaveRegion() {
    setBusy('region'); setRegionMsg('')
    try {
      const res = await fetch('/api/v1/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ region: region || null }),
      })
      if (res.ok) {
        rebootCountdown(setRegionMsg)
      } else if (res.status === 401) {
        setRegionMsg('Unauthorized — set the hub key below.')
      } else {
        let msg = 'Save failed.'
        try { const d = await res.json(); if (d.error) msg = d.error } catch { /* non-JSON body */ }
        setRegionMsg(msg)
      }
    } catch { setRegionMsg('hub not reachable') }
    setBusy('')
  }

  async function doSaveName() {
    setBusy('name'); setNameMsg('')
    try {
      const res = await fetch('/api/v1/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ name: hubName }),
      })
      if (res.ok) {
        rebootCountdown(setNameMsg)
      } else if (res.status === 401) {
        setNameMsg('Unauthorized — set the hub key below.')
      } else {
        let msg = 'Save failed.'
        try { const d = await res.json(); if (d.error) msg = d.error } catch { /* non-JSON body */ }
        setNameMsg(msg)
      }
    } catch { setNameMsg('hub not reachable') }
    setBusy('')
  }

  async function doFactoryReset(wipe) {
    const q = wipe
      ? 'Factory reset AND ERASE ALL DATA? Plants, history and every setting are wiped — the hub returns to a fresh install and starts its setup WiFi.'
      : 'Factory reset? Claim, WiFi and node pairings are cleared (plants and history survive) — the hub starts its setup WiFi.'
    if (!confirm(q)) return
    setBusy('freset'); setFresetMsg('')
    try {
      const res = await fetch('/api/v1/factory_reset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ wipe_data: wipe }),
      })
      if (res.ok) {
        setFresetMsg('Resetting — the hub is leaving this network. Connect to its setup WiFi to start over.')
        return    // deliberately stay "busy": the hub is gone, the buttons must not re-arm
      } else if (res.status === 401) {
        setFresetMsg('Unauthorized — set the hub key above.')
      } else {
        let msg = 'Reset failed.'
        try { const d = await res.json(); if (d.error) msg = d.error } catch { /* non-JSON body */ }
        setFresetMsg(msg)
      }
    } catch { setFresetMsg('hub not reachable') }
    setBusy('')
  }

  function doSaveAi() {
    setBusy('ai'); setAiMsg('')
    // An empty key field means "clear it" -- setAiSettings treats null as
    // remove, not the literal string "null" (that was a review-fixed bug).
    setAiSettings({ kind: ai.kind, endpoint: ai.endpoint, model: ai.model, key: ai.key || null })
    setAiMsg('Saved.')
    setBusy('')
  }

  function doClearAiKey() {
    setAiSettings({ key: null })
    setAi((a) => ({ ...a, key: '' }))
    setAiMsg('Key cleared.')
  }

  async function doTestAi() {
    setBusy('ai-test'); setAiTestMsg(''); setAiTestDetail('')
    try {
      // Test whatever is currently typed, not the last-saved value -- the
      // point of this button is to check a change before committing it.
      await aiComplete({
        system: 'You are a test.',
        user: 'Reply with the single word: ok',
        settings: {
          kind: ai.kind,
          // Same normalisation Save routes through on write (settings.js),
          // so Test checks the exact value that would actually be stored --
          // not a doubled-slash URL that fails for a reason Save wouldn't hit.
          endpoint: normEndpoint(ai.endpoint) || AI_DEFAULTS.endpoint,
          model: ai.model || AI_DEFAULTS.model,
          key: ai.key,
        },
      })
      setAiTestMsg('Connection OK.')
    } catch (err) {
      if (err instanceof AiError) {
        setAiTestMsg(err.message)
        setAiTestDetail(err.detail || '')
      } else {
        setAiTestMsg('Unexpected error testing the connection.')
      }
    }
    setBusy('')
  }

  if (error) return <p class="error">Hub not reachable.</p>
  if (!st) return <p class="placeholder">Loading…</p>

  return (
    <div class="config">
      <div class="panel">
        <h2>Hub</h2>
        <table class="kv">
          <tbody>
            <tr><td>Firmware</td><td>{st.version}</td></tr>
            <tr><td>Uptime</td><td>{fmtUptime(st.uptime_s)}</td></tr>
            <tr><td>Clock</td><td>{st.time_synced ? 'synced' : 'not synced'}</td></tr>
            <tr>
              <td>Storage</td>
              <td>
                {fmtBytes(st.fs_used)} / {fmtBytes(st.fs_total)}
                {st.fs_total > 0 && ` (${Math.round((st.fs_used / st.fs_total) * 100)}%)`}
              </td>
            </tr>
            <tr><td>Free heap</td><td>{fmtBytes(st.heap_free)}</td></tr>
            <tr><td>Claim state</td><td>{st.claimed ? 'claimed' : 'unclaimed'}</td></tr>
          </tbody>
        </table>
        {st.fs_warn && (
          // Storage-pressure ruling (task-6 finding, surfaced by Task 8):
          // neither chip's history partition can hold full retention for
          // all 16 plants, and a full-storage append fails silently
          // (storage_append(), oldest-first ring eviction). st.fs_warn
          // (api_v1.c's status_get) latches at 85% used -- calm and
          // factual, not an alarm: nothing is broken, the ring buffer is
          // doing exactly what it's designed to do.
          <p class="infobox">
            Storage is over 85% full. History is a ring buffer, so this is expected under
            heavy use (many plants logging at once) — oldest samples are dropped first as
            new ones arrive, nothing is lost that isn't meant to be. No action needed unless
            you want more retention headroom.
          </p>
        )}
        <p>
          <label>
            Name
            <input value={hubName} maxLength={15} placeholder={st.name}
                   onInput={(e) => setHubName(e.currentTarget.value)} />
          </label>
          {' '}
          <button class="btn-primary" onClick={doSaveName}
                  disabled={busy === 'name' || !cfgLoaded || hubName === (cfg?.name ?? '')}>
            {busy === 'name' ? 'Saving…' : 'Rename'}
          </button>
        </p>
        <p class="infobox">
          Letters, digits, <code>-</code> and <code>_</code> only (max 15). The name is also
          the setup WiFi name and the MQTT topic prefix — renaming reboots the hub, and
          Home Assistant will rediscover the plants under the new topics.
        </p>
        {nameMsg && <p class="hint">{nameMsg}</p>}
      </div>

      <div class="panel">
        <h2>WiFi region</h2>
        <p>
          <label>
            Region
            <select value={region} onChange={(e) => setRegion(e.currentTarget.value)}>
              <option value="">Build default</option>
              <option value="CH">Europe (channels 1–13)</option>
              <option value="US">United States (1–11)</option>
              <option value="JP">Japan (1–14)</option>
              <option value="01">World-safe (1–11)</option>
            </select>
          </label>
          {' '}
          <button class="btn-primary" onClick={doSaveRegion} disabled={busy === 'region'}>
            {busy === 'region' ? 'Saving…' : 'Save region'}
          </button>
        </p>
        <p class="infobox">
          Saving reboots the hub and applies the region to it. Already-paired nodes do{' '}
          <strong>not</strong> pick up the change automatically — a node inherits the region
          once, when it pairs. To move existing nodes to the new region, forget and re-pair
          them. If pairing fails on channels 12–13, check the region.
        </p>
        {regionMsg && <p class="hint">{regionMsg}</p>}
      </div>

      <div class="panel">
        <h2>Claim</h2>
        {secret && (
          <p class="secretbox">
            Hub claimed. Your key (shown once, also saved in this browser):
            <code>{secret}</code>
          </p>
        )}
        {!st.claimed && !secret && (
          <p>
            <button class="btn-primary" onClick={doClaim} disabled={busy === 'claim'}>
              {busy === 'claim' ? 'Claiming…' : 'Claim this hub'}
            </button>
            <span class="hint"> Locks renaming, WiFi changes and updates behind a key.</span>
          </p>
        )}
        {st.claimed && (
          <div>
            <label class="keyrow">
              Hub key
              <input type="password" value={keyInput}
                     onInput={(e) => { setKeyInput(e.currentTarget.value); setKey(e.currentTarget.value) }}
                     placeholder="paste the 64-char key" />
            </label>
            <button class="btn-destructive" onClick={doUnclaim} disabled={busy === 'unclaim'}>
              {busy === 'unclaim' ? 'Unclaiming…' : 'Unclaim hub'}
            </button>
          </div>
        )}
      </div>

      <div class="panel">
        <h2>Firmware update</h2>
        <p>
          <label class="filebtn btn-primary">
            {busy === 'ota' ? 'Uploading…' : 'Choose firmware…'}
            <input type="file" accept=".bin" onChange={doOta} disabled={busy === 'ota'} />
          </label>
        </p>
        {otaPct != null && busy === 'ota' && (
          <p>
            <progress max="100" value={otaPct} />
            <button class="btn-destructive" onClick={cancelOta}>Cancel</button>
          </p>
        )}
        <p class="infobox">
          Firmware comes in regional builds (<code>eu</code>/<code>us</code>/<code>jp</code>)
          that differ in allowed WiFi channels. Updating with the wrong region's image can
          strand the hub if your router uses channels the image forbids (12–13 on{' '}
          <code>us</code>) — pick the build for your region. A WiFi region set above
          overrides the image's default.
        </p>
        {otaMsg && <p class="hint">{otaMsg}</p>}
      </div>

      {st.role !== 'node' && (
        <div class="panel">
          <h2>Integrations</h2>
          {cfgLoadError && (
            <p class="error">
              Couldn't load current settings — retry.{' '}
              <button type="button" onClick={refreshConfig}>Retry</button>
            </p>
          )}
          <form onSubmit={doSaveIntegrations}>
            <fieldset>
              <legend>MQTT</legend>
              <label>
                <input type="checkbox" checked={mqtt.enabled}
                       onChange={(e) => setMqtt((m) => ({ ...m, enabled: e.currentTarget.checked }))} />
                {' '}Enabled
              </label>
              <label>
                Broker URI
                <input value={mqtt.uri} placeholder="mqtt://host:1883"
                       onInput={(e) => setMqtt((m) => ({ ...m, uri: e.currentTarget.value }))} />
              </label>
              <label>
                Username
                <input value={mqtt.user}
                       onInput={(e) => setMqtt((m) => ({ ...m, user: e.currentTarget.value }))} />
              </label>
              <label>
                Password
                <input type="password" value={mqtt.pass}
                       placeholder={cfg?.mqtt?.pass_set ? 'saved' : ''}
                       onInput={(e) => setMqtt((m) => ({ ...m, pass: e.currentTarget.value }))} />
              </label>
              <p class="infobox">
                Each plant publishes JSON to{' '}
                <code>planthub/{st.name}/plant/&lt;id&gt;/state</code>, availability to{' '}
                <code>planthub/{st.name}/status</code>. Home Assistant finds them automatically
                via MQTT discovery.
              </p>
            </fieldset>

            <fieldset>
              <legend>InfluxDB</legend>
              <label>
                <input type="checkbox" checked={influx.enabled}
                       onChange={(e) => setInflux((i) => ({ ...i, enabled: e.currentTarget.checked }))} />
                {' '}Enabled
              </label>
              <label>
                URL
                <input value={influx.url} placeholder="http://host:8086"
                       onInput={(e) => setInflux((i) => ({ ...i, url: e.currentTarget.value }))} />
              </label>
              <label>
                Org
                <input value={influx.org} placeholder="my-org"
                       onInput={(e) => setInflux((i) => ({ ...i, org: e.currentTarget.value }))} />
              </label>
              <label>
                Bucket
                <input value={influx.bucket} placeholder="my-bucket"
                       onInput={(e) => setInflux((i) => ({ ...i, bucket: e.currentTarget.value }))} />
              </label>
              <label>
                Token
                <input type="password" value={influx.token}
                       placeholder={cfg?.influx?.token_set ? 'saved' : ''}
                       onInput={(e) => setInflux((i) => ({ ...i, token: e.currentTarget.value }))} />
              </label>
              <p class="infobox">
                Points land in this bucket as measurement <code>plant</code>
                (tagged <code>plant=&lt;id&gt;</code>, one point per bound
                capability) and measurement <code>device</code> (tagged{' '}
                <code>device=&lt;id&gt;</code>, every known device whether
                bound to a plant or not), with fields <code>moisture</code>,{' '}
                <code>temp</code>, <code>lux</code>, <code>conductivity</code>,{' '}
                <code>battery</code>, <code>humidity</code>,{' '}
                <code>pressure</code> and <code>rssi</code> — whichever of
                these each point currently reports. There is no{' '}
                <code>name</code> field.
              </p>
            </fieldset>

            <p>
              <button type="submit" class="btn-primary" disabled={busy === 'config' || !cfgLoaded}>
                {busy === 'config' ? 'Saving…' : 'Save integrations'}
              </button>
            </p>
          </form>
          {cfgMsg && <p class="hint">{cfgMsg}</p>}
        </div>
      )}

      {st.role === 'node' && (
        <div class="panel">
          <h2>Node</h2>
          {st.pair_failed && (
            <p class="error">
              Pairing failed — make sure the main hub's pairing window is open, then Retry.
            </p>
          )}
          <p>
            {st.pair_failed && (
              <button class="btn-primary" onClick={doRetryPairing} disabled={busy === 'retry'}>
                {busy === 'retry' ? 'Retrying…' : 'Retry pairing'}
              </button>
            )}
            {' '}
            <button onClick={doSwitchToMain} disabled={busy === 'switch'}>
              {busy === 'switch' ? 'Switching…' : 'Switch back to main hub'}
            </button>
          </p>
        </div>
      )}

      <div class="panel">
        <h2>Factory reset</h2>
        {!st.claimed && (
          <p class="hint">Available on claimed hubs only — claim the hub above to enable.</p>
        )}
        <p>
          <button class="btn-destructive" onClick={() => doFactoryReset(false)}
                  disabled={busy === 'freset' || !st.claimed}>
            {busy === 'freset' ? 'Resetting…' : 'Factory reset'}
          </button>
          <span class="hint">
            {' '}Clears claim, WiFi and node pairings, then reboots into the setup WiFi —
            plants, their history and the integration settings all survive.
          </span>
        </p>
        <p>
          <button class="btn-destructive" onClick={() => doFactoryReset(true)}
                  disabled={busy === 'freset' || !st.claimed}>
            {busy === 'freset' ? 'Resetting…' : 'Hard factory reset'}
          </button>
          <span class="hint">
            {' '}Erases <strong>everything</strong> — plants, history and all settings —
            back to a fresh install, then reboots into the setup WiFi.
          </span>
        </p>
        {fresetMsg && <p class="hint">{fresetMsg}</p>}
      </div>

      <div class="panel">
        <h2>AI assist</h2>
        <p>
          <label>
            Provider
            <select value={ai.kind} onChange={(e) => setAi((a) => ({ ...a, kind: e.currentTarget.value }))}>
              <option value="anthropic">Anthropic (Claude)</option>
              <option value="openai">OpenAI-compatible / local</option>
            </select>
          </label>
        </p>
        <p>
          <label>
            Endpoint
            <input value={ai.endpoint} placeholder={AI_DEFAULTS.endpoint}
                   onInput={(e) => setAi((a) => ({ ...a, endpoint: e.currentTarget.value }))} />
          </label>
        </p>
        <p>
          <label>
            Model
            <input value={ai.model} placeholder={AI_DEFAULTS.model}
                   onInput={(e) => setAi((a) => ({ ...a, model: e.currentTarget.value }))} />
          </label>
        </p>
        <label class="keyrow">
          API key
          <input type="password" value={ai.key} placeholder="paste your API key"
                 onInput={(e) => setAi((a) => ({ ...a, key: e.currentTarget.value }))} />
        </label>
        <p class="infobox">Stored in this browser only. Never sent to the hub.</p>
        <p>
          <button class="btn-primary" onClick={doSaveAi} disabled={busy === 'ai'}>
            {busy === 'ai' ? 'Saving…' : 'Save AI settings'}
          </button>
          {' '}
          <button onClick={doClearAiKey} disabled={busy !== '' || !ai.key}>Clear key</button>
          {' '}
          <button onClick={doTestAi} disabled={busy === 'ai-test'}>
            {busy === 'ai-test' ? 'Testing…' : 'Test connection'}
          </button>
        </p>
        {aiMsg && <p class="hint">{aiMsg}</p>}
        {aiTestMsg && (
          <p class="hint">
            {aiTestMsg}
            {aiTestDetail && (
              <details><summary>Details</summary><pre>{aiTestDetail}</pre></details>
            )}
          </p>
        )}
      </div>
    </div>
  )
}
