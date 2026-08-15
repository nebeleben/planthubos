import { useEffect, useState } from 'preact/hooks'
import { authHeaders } from '../lib/auth.js'

function fmtAge(ageS) {
  if (ageS == null) return 'never'
  if (ageS < 90) return `${ageS}s ago`
  if (ageS < 5400) return `${Math.round(ageS / 60)}m ago`
  return `${Math.round(ageS / 3600)}h ago`
}

function plantLabel(p) {
  return p.name || `Plant ${p.id}`
}

// One <select> per probe doubles as both the "currently assigned plant"
// display (its selected value IS the current assignment -- an unassigned
// probe shows the "— unassign —" placeholder selected) and the
// reassignment control, the same "current state IS the control, no
// separate Save step" pattern nodes.jsx's PowerModeControl already uses.
function AssignControl({ s, plants, onAssigned }) {
  const [state, setState] = useState('idle') // idle | saving | error | unauth

  async function onChange(e) {
    const val = e.currentTarget.value
    setState('saving')
    try {
      let res
      if (val === '') {
        // Unassign: POST {"mac":null} to the plant that CURRENTLY owns
        // this probe -- plants_table_assign()'s unassign semantics only
        // apply through the owning plant's own /probe route, there's no
        // "detach this mac from whatever it's on" call. Nothing to do if
        // it's already unassigned (shouldn't normally fire -- the select
        // would already show this option selected -- but guard anyway).
        if (s.plant_id == null) { setState('idle'); return }
        res = await fetch(`/api/v1/plants/${s.plant_id}/probe`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json', ...authHeaders() },
          body: JSON.stringify({ mac: null }),
        })
      } else {
        // Assign or move: the TARGET plant's /probe route takes this
        // probe's mac. If it was assigned elsewhere, the hub moves it
        // there (plants_table_assign(), api_v1.c's plants_probe_post) --
        // no separate unassign call against the old plant is needed.
        res = await fetch(`/api/v1/plants/${val}/probe`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json', ...authHeaders() },
          body: JSON.stringify({ mac: s.mac }),
        })
      }
      if (res.ok) { setState('idle'); onAssigned() }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <span class="assign-control">
      <select value={s.plant_id != null ? String(s.plant_id) : ''} disabled={state === 'saving'} onChange={onChange}>
        <option value="">— unassign —</option>
        {plants.map((p) => <option key={p.id} value={p.id}>{plantLabel(p)}</option>)}
      </select>
      {state === 'error' && <span class="error">failed</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </span>
  )
}

// Same collapsible-card shape as nodes.jsx's NodeCard: name/mac + last
// seen while collapsed, details and controls in the body. The body stays
// mounted (visibility toggled by the shared .node-card.open CSS), matching
// the nodes tab's behavior.
function ProbeCard({ s, plants, open, onToggle, onAssigned, onRenamed }) {
  const [name, setName] = useState(s.name || '')
  const [state, setState] = useState('idle') // idle | saving | saved | error | unauth

  async function save(e) {
    e.preventDefault()
    setState('saving')
    try {
      const res = await fetch(`/api/v1/sensors/${s.mac.replaceAll(':', '')}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ name }),
      })
      if (res.ok) { setState('saved'); onRenamed(s.mac, name) }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <div class={`node-card${open ? ' open' : ''}`}>
      <button type="button" class="node-card-header" onClick={onToggle} aria-expanded={open}>
        <span class="node-card-chevron" aria-hidden="true">▸</span>
        <span class="node-card-title">
          <span class="node-card-name">{s.name || s.mac}</span>
          {s.name && <span class="node-card-mac mono">{s.mac}</span>}
        </span>
        <span class="node-card-age hint">{fmtAge(s.last_seen_s)}</span>
      </button>
      <div class="node-card-body">
        <form onSubmit={save} class="namef">
          <input value={name} maxlength={32} placeholder={s.mac}
                 onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
          <button type="submit" class="btn-primary" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>
        <div class="node-card-row">
          <span class="hint">{s.battery != null ? `battery ${s.battery}%` : 'battery –'}</span>
          <span class="hint">{s.rssi != null ? `${s.rssi} dBm` : '–'}</span>
          {/* Direct BLE reception (the hub hearing it on its own radio) is
              the strictly-better case and renders nothing; a non-null via
              means a node currently relays this probe and names which. */}
          {s.via && <span class="hint">via {s.via.name || s.via.mac} · {s.via.rssi} dBm</span>}
        </div>
        <div class="node-card-row">
          <span class="hint">Assigned to</span>
          <AssignControl s={s} plants={plants} onAssigned={onAssigned} />
        </div>
      </div>
    </div>
  )
}

export function DevicesTab() {
  const [sensors, setSensors] = useState(null)
  const [plants, setPlants] = useState(null)
  const [error, setError] = useState(false)
  const [openMap, setOpenMap] = useState({})

  function refresh(signal) {
    return Promise.all([
      fetch('/api/v1/sensors', { signal }).then((r) => r.json()).then((d) => setSensors(d.sensors)),
      fetch('/api/v1/plants', { signal }).then((r) => r.json()).then((d) => setPlants(d.plants)),
    ])
  }

  useEffect(() => {
    const controller = new AbortController()
    refresh(controller.signal).catch((err) => { if (err.name !== 'AbortError') setError(true) })
    return () => controller.abort()
  }, [])

  function toggleProbe(mac) {
    setOpenMap((prev) => ({ ...prev, [mac]: !prev[mac] }))
  }

  // Reassigning can touch TWO plants at once (the target gains a probe,
  // whichever plant previously held that mac loses it) plus the probe
  // pool's own plant_id/via -- a full refetch of both lists is simpler and
  // more obviously correct than threading the old owner through every call
  // site to patch state locally. (Plant create/rename/delete live on the
  // Plants tab -- plants are fetched here only to feed the assign select.)
  function onAssigned() {
    return refresh()
  }

  function onRenamed(mac, name) {
    setSensors((prev) => prev.map((s) => (s.mac === mac ? { ...s, name } : s)))
  }

  if (error) return <p class="error">Hub not reachable.</p>
  if (!sensors || !plants) return <p class="placeholder">Loading…</p>

  return (
    <div>
      <div class="panel">
        <h2>Probes</h2>
        {sensors.length === 0 ? (
          <p class="placeholder">No sensors discovered yet. MiFlora devices are discovered automatically — bring one in range.</p>
        ) : (
          <div class="node-cards">
            {sensors.map((s) => (
              <ProbeCard key={s.mac} s={s} plants={plants} open={!!openMap[s.mac]}
                         onToggle={() => toggleProbe(s.mac)} onAssigned={onAssigned} onRenamed={onRenamed} />
            ))}
          </div>
        )}
      </div>
    </div>
  )
}
