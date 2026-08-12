import { useEffect, useState } from 'preact/hooks'
import { ConfigTab } from './tabs/config.jsx'
import { DashboardTab } from './tabs/dashboard.jsx'
import { DevicesTab } from './tabs/devices.jsx'
import { HistoryTab } from './tabs/history.jsx'
import { NetworkTab } from './tabs/network.jsx'
import { NodesTab } from './tabs/nodes.jsx'
import { PlantsTab } from './tabs/plants.jsx'
import { RoleTab } from './tabs/role.jsx'
import { getStoredTheme, setStoredTheme, systemPrefersDark } from './lib/theme.js'

// Post-M8 split: Dashboard is the live plant cards (DashboardTab),
// Plants is plant management -- create/rename/delete plus per-plant probe
// assignment (plants.jsx), and Probes is just the probe pool
// (devices.jsx, which keeps its own sensor-side assign control).
const ALL_TABS = ['Dashboard', 'Plants', 'Probes', 'History', 'Nodes', 'Config', 'Network']

function Placeholder({ name }) {
  return <p class="placeholder">{name} — coming in a later milestone.</p>
}

// Minimal inline sun/moon glyphs for the header theme toggle -- avoids
// pulling in an icon font/library for two shapes, and renders identically
// across platforms (unlike emoji, whose glyph varies by OS).
function SunIcon() {
  return (
    <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2"
         stroke-linecap="round" aria-hidden="true">
      <circle cx="12" cy="12" r="4.5" />
      <path d="M12 2v2.5M12 19.5V22M4.2 4.2l1.8 1.8M18 18l1.8 1.8M2 12h2.5M19.5 12H22M4.2 19.8l1.8-1.8M18 6l1.8-1.8" />
    </svg>
  )
}

function MoonIcon() {
  return (
    <svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" aria-hidden="true">
      <path d="M20.6 15.1A8.7 8.7 0 1 1 8.9 3.4a7 7 0 0 0 11.7 11.7Z" />
    </svg>
  )
}

// Rendered at the bottom of every screen, including the pre-role RoleTab
// screen -- see both `return`s in App() below.
function Footer() {
  return (
    <footer class="site-footer">
      <a href="https://plaiiin-planhub.com" target="_blank" rel="noreferrer">@plaiiin-planhub.com</a>
    </footer>
  )
}

export function App() {
  const [tab, setTab] = useState('Dashboard')
  // Optimistically 'main' so an existing hub (the overwhelmingly common
  // case) renders its tabs immediately instead of flashing a loading state
  // -- only a device that has never chosen a role (fresh out of the box)
  // ever flips this to 'unset' once /api/v1/status answers.
  const [role, setRole] = useState('main')

  // Day/night state, purely presentational (drives the toggle icon and the
  // theme state -- the actual palette is CSS, keyed off the same
  // data-theme attribute this mirrors). No stored preference means "follow
  // the OS", same rule index.html's inline anti-FOUC script and style.css's
  // prefers-color-scheme block both use; this only tracks it in JS so the
  // toggle icon can react without a page reload.
  const [theme, setTheme] = useState(() => getStoredTheme() || (systemPrefersDark() ? 'dark' : 'light'))

  useEffect(() => {
    if (getStoredTheme()) return   // explicit override in effect: OS changes don't apply
    const mq = window.matchMedia('(prefers-color-scheme: dark)')
    const onChange = (e) => setTheme(e.matches ? 'dark' : 'light')
    mq.addEventListener('change', onChange)
    return () => mq.removeEventListener('change', onChange)
  }, [])

  function toggleTheme() {
    const next = theme === 'dark' ? 'light' : 'dark'
    setStoredTheme(next)
    setTheme(next)
  }

  function refreshRole() {
    fetch('/api/v1/status')
      .then((r) => r.json())
      .then((st) => {
        const r = st.role || 'main'   // pre-M5a hubs have no "role" field
        setRole(r)
        // An unpaired node reaching the webui at all means it's sitting in
        // its portal after a failed pairing attempt (a paired node runs no
        // web server, and a searching one hasn't set up webserver either)
        // -- Plants/Probes/History are all empty and pointless there,
        // so land the user straight on the Config tab, where the retry /
        // switch-back-to-main controls live.
        if (r === 'node' && !st.paired) setTab('Config')
        if (!st.time_synced) {
          fetch('/api/v1/time', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ epoch_s: Math.floor(Date.now() / 1000) }),
          })
        }
      })
      .catch(() => {})
  }

  useEffect(() => { refreshRole() }, [])

  if (role === 'unset') {
    return (
      <>
        <RoleTab
          onMainChosen={() => {
            setTab('Network')
            refreshRole()
          }}
        />
        <Footer />
      </>
    )
  }

  // Nodes only means anything on a hub (unset/main); a node device runs no
  // node-management surface of its own, so hide the tab there rather than
  // rendering an empty/confusing list.
  const TABS = role === 'node' ? ALL_TABS.filter((t) => t !== 'Nodes') : ALL_TABS

  return (
    <div class="app">
      <header>
        <div class="brand">
          <h1>PlantHub</h1>
        </div>
        <nav>
          {TABS.map((t) => (
            <button key={t} class={'tab-btn' + (t === tab ? ' active' : '')} onClick={() => setTab(t)}>
              {t}
            </button>
          ))}
        </nav>
        <button type="button" class="theme-toggle" onClick={toggleTheme}
                aria-label={theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'}>
          {theme === 'dark' ? <SunIcon /> : <MoonIcon />}
        </button>
      </header>
      <main>
        {tab === 'Dashboard' ? <DashboardTab /> :
         tab === 'Plants' ? <PlantsTab /> :
         tab === 'Probes' ? <DevicesTab /> :
         tab === 'History' ? <HistoryTab /> :
         tab === 'Nodes' ? <NodesTab /> :
         tab === 'Config' ? <ConfigTab /> :
         tab === 'Network' ? <NetworkTab /> :
         <Placeholder name={tab} />}
      </main>
      <Footer />
    </div>
  )
}
