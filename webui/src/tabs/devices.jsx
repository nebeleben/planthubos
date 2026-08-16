import { useEffect, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'
import { loadCaps, capLabel, fmtCap } from '../lib/caps.js'

function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

const KIND_LABEL = { ble: 'Bluetooth', espnow: 'ESP-NOW', zb: 'Zigbee' }
// Fixed display order regardless of which kinds are actually present --
// stable groupings read better than "whatever order the registry happened
// to return them in".
const KIND_ORDER = ['ble', 'espnow', 'zb']

function plantLabel(p) {
  return p.name || `Plant ${p.id}`
}

// Bind-key material is WRITE-ONLY (spec §4: "Keys are never returned by any
// GET" -- bthome.h's bindkey_get()/bindkey_has() contract). This field only
// ever POSTs a key or a null clear to /api/v1/devices/{id}/key; it never
// tries to read one back, and `has_key` (already on every GET /api/v1/devices
// entry regardless of kind) is the only state it renders between edits.
// Only BLE devices can currently have a key set (api_v1.c's devices_json.c
// comment: bindkey_has() is checked for every kind, but POST .../key only
// ever matters for BTHome's AES-CCM payloads) -- devices.jsx's own isBle
// gate already limits this component's caller to BLE rows.
function BindKeyField({ deviceId, hasKey }) {
  const [key, setKey] = useState('')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth | invalid

  async function submit(newKey) {
    setState('saving')
    try {
      const res = await fetch(`/api/v1/devices/${deviceId}/key`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ key: newKey }),
      })
      if (res.ok) { setState('saved'); setKey('') }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  function onSet(e) {
    e.preventDefault()
    if (!/^[0-9a-fA-F]{32}$/.test(key)) { setState('invalid'); return }
    submit(key)
  }

  function onClear() {
    if (!confirm('Clear the bind key for this device?')) return
    submit(null)
  }

  return (
    <form onSubmit={onSet} class="namef">
      <span class="hint">{hasKey ? 'key set' : 'no key'}</span>
      <input value={key} maxlength={32} placeholder="32 hex chars"
             onInput={(e) => { setKey(e.currentTarget.value); setState('idle') }} />
      <button type="submit" class="btn-primary" disabled={state === 'saving'}>
        {state === 'saving' ? '…' : 'Set key'}
      </button>
      {hasKey && (
        <button type="button" class="btn-destructive" onClick={onClear} disabled={state === 'saving'}>
          Clear
        </button>
      )}
      {state === 'saved' && <span class="hint">saved</span>}
      {state === 'invalid' && <span class="error">key must be 32 hex chars</span>}
      {state === 'error' && <span class="error">failed</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </form>
  )
}

// GET /api/v1/devices' `id` is the canonical device-id string (spec §2,
// e.g. "ble:A4C138xxxxxx") -- the rename route below is still mac-keyed
// (POST /api/v1/sensors/{MAC12}, api_v1.c's sensors_rename_post) and only
// ever resolves a name for BLE-kind devices (devices_json.c's device_json:
// app_config_get_sensor_name() is only consulted when e->id.kind ==
// DEV_KIND_BLE). Strips the "ble:" prefix to recover the bare 12 hex chars
// that route expects.
function mac12FromBleId(id) {
  const i = id.indexOf(':')
  return i < 0 ? id : id.slice(i + 1)
}

// Same collapsible-card shape as nodes.jsx's NodeCard / rules.jsx's
// RuleCard: name/id + last-seen while collapsed, details in the body.
function DeviceCard({ d, caps, plantNameById, open, onToggle, onRenamed }) {
  const isBle = d.kind === 'ble'
  const [name, setName] = useState(d.name || '')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth

  async function save(e) {
    e.preventDefault()
    setState('saving')
    try {
      const res = await fetch(`/api/v1/sensors/${mac12FromBleId(d.id)}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ name }),
      })
      if (res.ok) { setState('saved'); onRenamed(d.id, name) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  const plantNames = d.plant_ids.map((id) => plantNameById.get(id) || `Plant ${id}`)

  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{d.name || d.id}</span>
          {d.name && <span class="node-card-mac mono">{d.id}</span>}
        </span>
        <span class="node-card-age hint">{fmtAge(d.last_seen_s)}</span>
      </button>
      <div class="node-card-body">
        {/* Only a BLE device's addr is mac-keyed, which is the only key the
            rename store understands (see mac12FromBleId's doc comment) --
            ESP-NOW/Zigbee devices have no display-name form yet. */}
        {isBle && (
          <form onSubmit={save} class="namef">
            <input value={name} maxlength={32} placeholder={d.id}
                   onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
            <button type="submit" class="btn-primary" disabled={state === 'saving'}>
              {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
            </button>
            {state === 'error' && <span class="error">failed</span>}
            {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
          </form>
        )}
        <div class="node-card-row">
          <span class="hint">{d.via ? `via ${d.via}` : 'direct'} · {d.rssi} dBm</span>
        </div>
        {isBle && (
          <div class="node-card-row">
            <BindKeyField deviceId={d.id} hasKey={d.has_key} />
          </div>
        )}
        {d.caps.length === 0 ? (
          <p class="hint">No live capabilities yet.</p>
        ) : (
          <div class="table-scroll">
            <table class="devices">
              <thead><tr><th>Capability</th><th>Value</th><th>Age</th></tr></thead>
              <tbody>
                {d.caps.map((c) => (
                  <tr key={c.id}>
                    <td>{capLabel(caps, c.id)}</td>
                    <td>{fmtCap(caps, c.id, c.value)}</td>
                    <td class="hint">{fmtAge(c.age_s)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
        <div class="node-card-row">
          <span class="hint">
            {plantNames.length > 0 ? `Bound to ${plantNames.join(', ')}` : 'Not bound to any plant'}
          </span>
        </div>
      </div>
    </div>
  )
}

// Renders a hex byte string spaced every 2 chars for readability -- same
// idea as the wrapper editor's disassembly <pre>, just for raw bytes.
function fmtHexBytes(hex) {
  return hex.match(/.{1,2}/g)?.join(' ') || hex
}

// Same collapsible node-card shape as DeviceCard: header carries the
// device-id + age, body carries RSSI, every captured sample's hex payload,
// and the Add-wrapper hop into WrappersTab (M3 Task 8, spec §5/§6's
// unknown-device discovery surface -- devices no wrapper currently claims,
// captured so an operator/M4's AI can write one). GET /api/v1/unknown's
// shape (id/rssi/last_seen_s/samples[{hex,len,ts}]) is M4's own input
// contract -- rendered here as-is, never reshaped.
function UnknownDeviceCard({ d, open, onToggle, onAddWrapper }) {
  const newest = d.samples[d.samples.length - 1]   // s[] is oldest-first, newest-last (api_v1.c's unknown_get)
  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name mono">{d.id}</span>
        </span>
        <span class="node-card-age hint">{fmtAge(d.last_seen_s)}</span>
      </button>
      <div class="node-card-body">
        <div class="node-card-row">
          <span class="hint">
            {d.rssi} dBm · {d.samples.length} sample{d.samples.length === 1 ? '' : 's'} captured
          </span>
        </div>
        <div class="table-scroll">
          <table class="devices">
            <thead><tr><th>Payload</th><th>Bytes</th></tr></thead>
            <tbody>
              {d.samples.map((s, i) => (
                <tr key={i}>
                  <td class="mono">{fmtHexBytes(s.hex)}</td>
                  <td class="hint">{s.len}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <div class="node-card-footer">
          <button type="button" class="btn-primary" onClick={() => onAddWrapper(newest.hex)}>
            Add wrapper
          </button>
        </div>
      </div>
    </div>
  )
}

function UnknownDevicesSection({ onAddWrapper }) {
  const [devices, setDevices] = useState(null)
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})

  function refresh(signal) {
    return fetch('/api/v1/unknown', { signal }).then((r) => r.json()).then((d) => setDevices(d.devices || []))
  }

  useEffect(() => {
    const controller = new AbortController()
    refresh(controller.signal).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Same 10s keep-fresh cadence as the rest of this tab -- new captures
  // arrive purely from radio activity the operator didn't initiate here.
  useEffect(() => {
    const controller = new AbortController()
    const id = setInterval(() => refresh(controller.signal).catch(() => {}), 10000)
    return () => { clearInterval(id); controller.abort() }
  }, [])

  function toggle(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  return (
    <div class="panel">
      <h2>Unknown devices</h2>
      {error && <p class="error">Hub not reachable.</p>}
      {!error && !devices && <p class="placeholder">Loading…</p>}
      {!error && devices && devices.length === 0 && (
        <p class="placeholder">No unclaimed BLE devices captured yet — devices no wrapper matches show up here.</p>
      )}
      {!error && devices && devices.length > 0 && (
        <div class="node-cards">
          {devices.map((d) => (
            <UnknownDeviceCard key={d.id} d={d} open={!!openMap[d.id]} onToggle={() => toggle(d.id)}
                                onAddWrapper={onAddWrapper} />
          ))}
        </div>
      )}
    </div>
  )
}

export function DevicesTab({ onAddWrapper }) {
  const [caps, setCaps] = useState(null)
  const [devices, setDevices] = useState(null)
  const [plants, setPlants] = useState(null)
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})

  function refresh(signal) {
    return Promise.all([
      fetch('/api/v1/devices', { signal }).then((r) => r.json()).then((d) => setDevices(d.devices)),
      fetch('/api/v1/plants', { signal }).then((r) => r.json()).then((d) => setPlants(d.plants)),
    ])
  }

  useEffect(() => {
    loadCaps().then(setCaps).catch(() => {})
  }, [])

  useEffect(() => {
    const controller = new AbortController()
    refresh(controller.signal).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  // Background keep-fresh poll -- same 10s cadence/discipline as
  // rules.jsx's own rule-list poll: live values and ages change purely from
  // radio activity the operator didn't initiate here.
  useEffect(() => {
    const controller = new AbortController()
    const id = setInterval(() => refresh(controller.signal).catch(() => {}), 10000)
    return () => { clearInterval(id); controller.abort() }
  }, [])

  function toggleDevice(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  function onRenamed(id, name) {
    setDevices((prev) => prev.map((d) => (d.id === id ? { ...d, name } : d)))
  }

  if (error) return <p class="error">Hub not reachable.</p>
  if (!devices || !plants || !caps) return <p class="placeholder">Loading…</p>

  const plantNameById = new Map(plants.map((p) => [p.id, plantLabel(p)]))
  const byKind = new Map()
  for (const d of devices) {
    if (!byKind.has(d.kind)) byKind.set(d.kind, [])
    byKind.get(d.kind).push(d)
  }

  return (
    <div>
      <div class="panel">
        <h2>Devices</h2>
        {devices.length === 0 ? (
          <p class="placeholder">No devices discovered yet. MiFlora devices are discovered automatically — bring one in range.</p>
        ) : (
          KIND_ORDER.filter((k) => byKind.has(k)).map((k) => (
            <div key={k}>
              <h3>{KIND_LABEL[k] || k}</h3>
              <div class="node-cards">
                {byKind.get(k).map((d) => (
                  <DeviceCard key={d.id} d={d} caps={caps} plantNameById={plantNameById}
                              open={!!openMap[d.id]} onToggle={() => toggleDevice(d.id)} onRenamed={onRenamed} />
                ))}
              </div>
            </div>
          ))
        )}
      </div>
      <UnknownDevicesSection onAddWrapper={onAddWrapper} />
    </div>
  )
}
