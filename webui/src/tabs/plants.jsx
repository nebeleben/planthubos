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

// The plant-side counterpart of devices.jsx's AssignControl: same
// "current state IS the control" pattern, opposite direction -- here each
// PLANT row picks its probe. Selecting a probe that belongs to another
// plant MOVES it (the hub's plants_table_assign semantics); the empty
// option unassigns this plant's probe.
function ProbeSelect({ p, sensors, onAssigned }) {
  const [state, setState] = useState('idle') // idle | saving | error | unauth
  const current = p.probe?.mac ?? ''

  // Option pool: every probe the hub currently hears, plus this plant's
  // own probe even if it hasn't transmitted yet (a pre-assigned
  // replacement probe is in plants.bin but not in the registry snapshot).
  const macs = sensors.map((s) => s.mac)
  if (current && !macs.includes(current)) macs.unshift(current)

  function describe(mac) {
    const s = sensors.find((x) => x.mac === mac)
    if (!s) return `${mac} · not heard yet`
    const owner = s.plant_id != null && s.plant_id !== p.id ? ` · on ${s.owner_label}` : ''
    return `${mac} · ${fmtAge(s.last_seen_s)}${owner}`
  }

  async function onChange(e) {
    const val = e.currentTarget.value
    if (val === current) return
    setState('saving')
    try {
      const res = await fetch(`/api/v1/plants/${p.id}/probe`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', ...authHeaders() },
        body: JSON.stringify({ mac: val || null }),
      })
      if (res.ok) { setState('idle'); onAssigned() }
      else setState(res.status === 401 ? 'unauth' : 'error')
    } catch {
      setState('error')
    }
  }

  return (
    <span class="assign-control">
      <select value={current} disabled={state === 'saving'} onChange={onChange}>
        <option value="">— no probe —</option>
        {macs.map((m) => <option key={m} value={m}>{describe(m)}</option>)}
      </select>
      {state === 'error' && <span class="error">failed</span>}
      {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
    </span>
  )
}

function PlantRow({ p, sensors, onRenamed, onDeleted, onAssigned }) {
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
          <button type="submit" class="btn-primary" disabled={state === 'saving'}>
            {state === 'saving' ? '…' : state === 'saved' ? '✓' : 'Save'}
          </button>
          {state === 'error' && <span class="error">failed</span>}
          {state === 'unauth' && <span class="error">unauthorized — set the hub key in Config</span>}
        </form>
      </td>
      <td class="mono">{p.id}</td>
      <td><ProbeSelect p={p} sensors={sensors} onAssigned={onAssigned} /></td>
      <td>
        <button class="btn-destructive" onClick={del} disabled={deleting}>{deleting ? '…' : 'Delete'}</button>
      </td>
    </tr>
  )
}

export function PlantsTab() {
  const [plants, setPlants] = useState(null)
  const [sensors, setSensors] = useState(null)
  const [error, setError] = useState(false)
  const [creating, setCreating] = useState(false)

  function refresh(signal) {
    return Promise.all([
      fetch('/api/v1/plants', { signal }).then((r) => r.json()).then((d) => setPlants(d.plants)),
      fetch('/api/v1/sensors', { signal }).then((r) => r.json()).then((d) => setSensors(d.sensors)),
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
    setSensors((prev) => prev.map((s) => (s.plant_id === id ? { ...s, plant_id: null } : s)))
  }

  // Reassigning can touch TWO plants at once (the target gains a probe,
  // whichever plant previously held that mac loses it) -- a full refetch
  // of both lists is simpler and more obviously correct than patching
  // state locally.
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
      // Create's success body is {"id":N} with no "ok" field
      // (api_v1.c's plants_create_post).
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
  if (!plants || !sensors) return <p class="placeholder">Loading…</p>

  // The probe column's "on <other plant>" labels need id -> name here,
  // where both lists are in hand; precompute rather than passing plants
  // into every ProbeSelect.
  const label = new Map(plants.map((p) => [p.id, plantLabel(p)]))
  const sensorsL = sensors.map((s) => ({ ...s, owner_label: label.get(s.plant_id) ?? `Plant ${s.plant_id}` }))

  return (
    <div>
      <div class="panel">
        <h2>Plants</h2>
        {plants.length === 0 ? (
          <p class="placeholder">No plants yet.</p>
        ) : (
          <div class="table-scroll"><table class="devices">
            <thead><tr><th>Name</th><th>ID</th><th>Probe</th><th></th></tr></thead>
            <tbody>{plants.map((p) => (
              <PlantRow key={p.id} p={p} sensors={sensorsL}
                        onRenamed={onRenamed} onDeleted={onDeleted} onAssigned={onAssigned} />
            ))}</tbody>
          </table></div>
        )}
        <p>
          <button class="btn-primary" onClick={doCreate} disabled={creating}>{creating ? 'Creating…' : 'New plant'}</button>
        </p>
        <p class="infobox">
          A plant keeps its history even when its probe changes — swap a broken probe by
          assigning the new one here. Picking a probe that belongs to another plant moves it.
        </p>
      </div>
    </div>
  )
}
