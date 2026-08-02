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
  const [busy, setBusy] = useState('')           // '' | claim | unclaim | ota | pair | retry | switch
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

      {st.role !== 'node' && (
        <div>
          <h2>Nodes</h2>
          <p class="hint">Rename, forget, or pair a node on the <strong>Nodes</strong> tab.</p>
        </div>
      )}

      {st.role === 'node' && (
        <div>
          <h2>Node</h2>
          {st.pair_failed && (
            <p class="error">
              Pairing failed — make sure the main hub's pairing window is open, then Retry.
            </p>
          )}
          <p>
            {st.pair_failed && (
              <button onClick={doRetryPairing} disabled={busy === 'retry'}>
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
    </div>
  )
}
