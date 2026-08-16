import { useEffect, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'
import { loadCaps, capLabel, fmtCap } from '../lib/caps.js'

function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

function plantLabel(p) {
  return p.name || `Plant ${p.id}`
}

function deviceLabel(d) {
  return d.name ? `${d.name} (${d.id})` : d.id
}

// One capability's binding row inside a plant's card: current device (if
// any), its live value/age, a dropdown of devices currently reporting this
// exact capability (plus "clear"), matching devices.jsx/nodes.jsx's
// "current state IS the control" pattern. POSTs straight to
// /api/v1/plants/{id}/bind -- spec §4/§7's per-capability binding route,
// replacing V1's single whole-probe "/probe" route entirely (that route is
// gone; see api_v1.c's plants_bind_post doc comment).
function CapBindRow({ plantId, capId, caps, binding, deviceIds, deviceLabelById, onBound }) {
  const [state, setState] = useState('idle') // idle | saving | error | unauth
  const current = binding ? binding.device : ''

  // Option pool: every device the hub currently sees reporting this
  // capability, plus the currently-bound device even if it isn't reporting
  // right now (a pre-bound replacement that hasn't transmitted yet -- the
  // same "keep the current selection visible" rule plants.jsx's old
  // ProbeSelect used for probes).
  const options = deviceIds.slice()
  if (current && !options.includes(current)) options.unshift(current)

  async function onChange(e) {
    const val = e.currentTarget.value
    if (val === current) return
    setState('saving')
    try {
      const res = await fetch(`/api/v1/plants/${plantId}/bind`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ cap: capId, device: val || null }),
      })
      if (res.ok) { setState('idle'); onBound() }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <div class="node-card-row">
      <span class="hint">{capLabel(caps, capId)}</span>
      <span class="assign-control">
        <select value={current} disabled={state === 'saving'} onChange={onChange}>
          <option value="">— not bound —</option>
          {options.map((id) => <option key={id} value={id}>{deviceLabelById.get(id) || id}</option>)}
        </select>
        {state === 'error' && <span class="error">failed</span>}
        {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
      </span>
      {binding && (
        <span class="hint">{fmtCap(caps, capId, binding.value)} · {fmtAge(binding.age_s)}</span>
      )}
    </div>
  )
}

// spec §4's one-click "bind whole device" flow: POST {"cap":null,"device":
// "<id>"} binds every capability that device currently reports
// (plants_bind_device()) -- reproduces the V1 "assign this probe"
// experience in one action; individual capabilities can then be
// re-pointed or cleared via the rows above.
function BindDeviceControl({ plantId, devices, onBound }) {
  const [devId, setDevId] = useState('')
  const [state, setState] = useState('idle') // idle | saving | error | unauth

  async function bind() {
    if (!devId) return
    setState('saving')
    try {
      const res = await fetch(`/api/v1/plants/${plantId}/bind`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ cap: null, device: devId }),
      })
      if (res.ok) { setState('idle'); setDevId(''); onBound() }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <div class="node-card-row">
      <select value={devId} disabled={state === 'saving'} onChange={(e) => setDevId(e.currentTarget.value)}>
        <option value="">— choose a device —</option>
        {devices.map((d) => <option key={d.id} value={d.id}>{deviceLabel(d)}</option>)}
      </select>
      <button type="button" class="btn-primary" onClick={bind} disabled={!devId || state === 'saving'}>
        {state === 'saving' ? '…' : 'Bind whole device'}
      </button>
      {state === 'error' && <span class="error">failed</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </div>
  )
}

function PlantCard({ p, caps, devices, deviceLabelById, capsByDevice, open, onToggle,
                     onRenamed, onDeleted, onBound }) {
  const [name, setName] = useState(p.name || '')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth
  const [deleting, setDeleting] = useState(false)

  async function save(e) {
    e.preventDefault()
    setState('saving')
    try {
      const res = await fetch(`/api/v1/plants/${p.id}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ name }),
      })
      if (res.ok) { setState('saved'); onRenamed(p.id, name) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  async function del() {
    if (!confirm(
      `Delete ${plantLabel(p)}? This permanently removes its stored history — there is no undo. ` +
      `Its bound devices become unbound and can be reassigned to another plant afterward.`
    )) return
    setDeleting(true)
    try {
      const res = await fetch(`/api/v1/plants/${p.id}`, { method: 'DELETE', headers: authHeaders() })
      if (res.ok) { onDeleted(p.id); return }
      alert(res.status === 401 ? 'unauthorized — set the hub key in Config' : 'delete failed')
    } catch {
      alert('hub not reachable')
    }
    setDeleting(false)
  }

  const bindingByCap = new Map(p.bindings.map((b) => [b.cap, b]))
  const capIds = Array.from(caps.keys()).sort((a, b) => a - b)

  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{plantLabel(p)}</span>
          <span class="node-card-mac mono">#{p.id}</span>
        </span>
        <span class="node-card-age hint">{p.bindings.length} bound</span>
      </button>
      <div class="node-card-body">
        <form onSubmit={save} class="namef">
          <input value={name} maxlength={32} placeholder={`Plant ${p.id}`}
                 onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
          <button type="submit" class="btn-primary" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>

        <BindDeviceControl plantId={p.id} devices={devices} onBound={onBound} />

        {capIds.map((capId) => (
          <CapBindRow key={capId} plantId={p.id} capId={capId} caps={caps}
                      binding={bindingByCap.get(capId)} deviceIds={capsByDevice.get(capId) || []}
                      deviceLabelById={deviceLabelById} onBound={onBound} />
        ))}

        <div class="node-card-footer">
          <button class="btn-destructive" onClick={del} disabled={deleting}>{deleting ? '…' : 'Delete'}</button>
        </div>
      </div>
    </div>
  )
}

export function PlantsTab() {
  const [caps, setCaps] = useState(null)
  const [plants, setPlants] = useState(null)
  const [devices, setDevices] = useState(null)
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})
  const [creating, setCreating] = useState(false)

  function refresh(signal) {
    return Promise.all([
      fetch('/api/v1/plants', { signal }).then((r) => r.json()).then((d) => setPlants(d.plants)),
      fetch('/api/v1/devices', { signal }).then((r) => r.json()).then((d) => setDevices(d.devices)),
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

  function togglePlant(id) {
    setOpenMap((prev) => ({ ...prev, [id]: !prev[id] }))
  }

  function onRenamed(id, name) {
    setPlants((prev) => prev.map((p) => (p.id === id ? { ...p, name } : p)))
  }

  function onDeleted(id) {
    setPlants((prev) => prev.filter((p) => p.id !== id))
  }

  // A bind/unbind can move a device's capability from another plant onto
  // this one -- refetching both lists is simpler and more obviously correct
  // than patching per-capability state locally across every plant card.
  function onBound() {
    return refresh()
  }

  async function doCreate() {
    setCreating(true)
    try {
      const res = await fetch('/api/v1/plants', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: '{}',
      })
      const data = await res.json().catch(() => ({}))
      if (res.ok && data.id != null) {
        await refresh()
      } else if (res.status === 401) {
        alert('unauthorized — set the hub key in Config')
      } else if (res.status === 409) {
        alert('plant table is full (16 max) — delete one first')
      } else {
        alert('create failed')
      }
    } catch {
      alert('hub not reachable')
    }
    setCreating(false)
  }

  if (error) return <p class="error">Hub not reachable.</p>
  if (!plants || !devices || !caps) return <p class="placeholder">Loading…</p>

  const deviceLabelById = new Map(devices.map((d) => [d.id, deviceLabel(d)]))
  const capsByDevice = new Map()
  for (const d of devices) {
    for (const c of d.caps) {
      if (!capsByDevice.has(c.id)) capsByDevice.set(c.id, [])
      capsByDevice.get(c.id).push(d.id)
    }
  }

  return (
    <div>
      <div class="panel">
        <h2>Plants</h2>
        {plants.length === 0 ? (
          <p class="placeholder">No plants yet.</p>
        ) : (
          <div class="node-cards">
            {plants.map((p) => (
              <PlantCard key={p.id} p={p} caps={caps} devices={devices} deviceLabelById={deviceLabelById}
                         capsByDevice={capsByDevice} open={!!openMap[p.id]} onToggle={() => togglePlant(p.id)}
                         onRenamed={onRenamed} onDeleted={onDeleted} onBound={onBound} />
            ))}
          </div>
        )}
        <p>
          <button class="btn-primary" onClick={doCreate} disabled={creating}>{creating ? 'Creating…' : 'New plant'}</button>
        </p>
        <p class="infobox">
          A plant keeps its history even when its devices change — a capability's history
          follows the plant, not the device. Bind a whole device for the V1-style one-click
          setup, then re-point or clear individual capabilities below as needed.
        </p>
      </div>
    </div>
  )
}
