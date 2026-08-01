import { useEffect, useRef, useState } from 'preact/hooks'
import { getKey, setKey, authHeaders } from '../lib/auth.js'

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
  const [busy, setBusy] = useState('')           // '' | claim | unclaim | ota
  const [otaMsg, setOtaMsg] = useState('')
  const [otaPct, setOtaPct] = useState(null)
  const xhrRef = useRef(null)

  function refresh() {
    fetch('/api/v1/status')
      .then((r) => r.json())
      .then((s) => { setSt(s); setError(false) })
      .catch(() => setError(true))
  }
  useEffect(() => { refresh() }, [])

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
    if (!confirm('Unclaim this hub? Mutating endpoints become open again.')) return
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

  if (error) return <p class="error">Hub not reachable.</p>
  if (!st) return <p class="placeholder">Loading…</p>

  return (
    <div class="config">
      <h2>Hub</h2>
      <table class="kv">
        <tbody>
          <tr><td>Firmware</td><td>{st.version}</td></tr>
          <tr><td>Uptime</td><td>{fmtUptime(st.uptime_s)}</td></tr>
          <tr><td>Clock</td><td>{st.time_synced ? 'synced' : 'not synced'}</td></tr>
          <tr><td>Storage</td><td>{fmtBytes(st.fs_used)} / {fmtBytes(st.fs_total)}</td></tr>
          <tr><td>Free heap</td><td>{fmtBytes(st.heap_free)}</td></tr>
          <tr><td>Claim state</td><td>{st.claimed ? 'claimed' : 'unclaimed'}</td></tr>
        </tbody>
      </table>

      <h2>Claim</h2>
      {secret && (
        <p class="secretbox">
          Hub claimed. Your key (shown once, also saved in this browser):
          <code>{secret}</code>
        </p>
      )}
      {!st.claimed && !secret && (
        <p>
          <button onClick={doClaim} disabled={busy === 'claim'}>
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
          <button onClick={doUnclaim} disabled={busy === 'unclaim'}>
            {busy === 'unclaim' ? 'Unclaiming…' : 'Unclaim hub'}
          </button>
        </div>
      )}

      <h2>Firmware update</h2>
      <p>
        <input type="file" accept=".bin" onChange={doOta} disabled={busy === 'ota'} />
      </p>
      {otaPct != null && busy === 'ota' && (
        <p>
          <progress max="100" value={otaPct} />
          <button onClick={cancelOta}>Cancel</button>
        </p>
      )}
      {otaMsg && <p class="hint">{otaMsg}</p>}
    </div>
  )
}
