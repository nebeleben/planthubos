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

// One <select> per sensor row doubles as both the "currently assigned
// plant" display (its selected value IS the current assignment -- an
// unassigned sensor shows the "— unassign —" placeholder selected) and the
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
        // this sensor -- plants_table_assign()'s unassign semantics only
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
        // sensor's mac. If it was assigned elsewhere, the hub moves it
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

function SensorRow({ s, plants, onAssigned }) {
  return (
    <tr>
      <td class="mono">{s.mac}</td>
      <td>{s.battery != null ? `${s.battery}%` : '–'}</td>
      <td>{s.rssi != null ? `${s.rssi} dBm` : '–'}</td>
      {/* Direct BLE reception (the hub hearing it on its own radio) is the
          strictly-better case and renders nothing here; a non-null via
          means a node currently relays this sensor (strongest RSSI wins --
          see registry.h) and names which one. */}
      <td class="via">{s.via ? `via ${s.via.name || s.via.mac} · ${s.via.rssi} dBm` : ''}</td>
      <td>{fmtAge(s.last_seen_s)}</td>
      <td><AssignControl s={s} plants={plants} onAssigned={onAssigned} /></td>
    </tr>
  )
}

function PlantRow({ p, onRenamed, onDeleted }) {
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
      `Its probe, if any, becomes unassigned and can be reassigned to another plant afterward.`
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

  return (
    <tr>
      <td>
        <form onSubmit={save} class="namef">
          <input value={name} maxlength={32} placeholder={`Plant ${p.id}`}
                 onInput={(e) => { setName(e.currentTarget.value); setState('idle') }} />
          <button type="submit" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>
      </td>
      <td class="mono">{p.id}</td>
      <td>
        <button onClick={del} disabled={deleting}>{deleting ? '…' : 'Delete'}</button>
      </td>
    </tr>
  )
}

export function DevicesTab() {
  const [sensors, setSensors] = useState(null)
  const [plants, setPlants] = useState(null)
  const [error, setError] = useState(false)
  const [creating, setCreating] = useState(false)

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

  function onRenamed(id, name) {
    setPlants((prev) => prev.map((p) => (p.id === id ? { ...p, name } : p)))
  }

  function onDeleted(id) {
    setPlants((prev) => prev.filter((p) => p.id !== id))
    // The hub unassigns that plant's probe (if any) as part of delete --
    // reflect that locally rather than leaving a stale plant_id pointing
    // at a plant that no longer exists until the next refetch.
    setSensors((prev) => prev.map((s) => (s.plant_id === id ? { ...s, plant_id: null } : s)))
  }

  // Reassigning can touch TWO plants at once (the target gains a probe,
  // whichever plant previously held that mac loses it) plus the sensor
  // pool's own plant_id/via -- a full refetch of both lists is simpler and
  // more obviously correct than threading the old owner through every call
  // site to patch state locally.
  function onAssigned() {
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
      if (res.ok && data.ok) {
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
  if (!sensors || !plants) return <p class="placeholder">Loading…</p>

  return (
    <div>
      <h2>Probes</h2>
      {sensors.length === 0 ? (
        <p class="placeholder">No sensors discovered yet. MiFlora devices are discovered automatically — bring one in range.</p>
      ) : (
        <table class="devices">
          <thead><tr><th>MAC</th><th>Battery</th><th>RSSI</th><th>Via</th><th>Last seen</th><th>Assigned to</th></tr></thead>
          <tbody>{sensors.map((s) => <SensorRow key={s.mac} s={s} plants={plants} onAssigned={onAssigned} />)}</tbody>
        </table>
      )}

      <h2>Plants</h2>
      {plants.length === 0 ? (
        <p class="placeholder">No plants yet.</p>
      ) : (
        <table class="devices">
          <thead><tr><th>Name</th><th>ID</th><th></th></tr></thead>
          <tbody>{plants.map((p) => <PlantRow key={p.id} p={p} onRenamed={onRenamed} onDeleted={onDeleted} />)}</tbody>
        </table>
      )}
      <p>
        <button onClick={doCreate} disabled={creating}>{creating ? 'Creating…' : 'New plant'}</button>
      </p>
    </div>
  )
}
