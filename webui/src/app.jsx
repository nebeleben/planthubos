import { useEffect, useState } from 'preact/hooks'
import { ConfigTab } from './tabs/config.jsx'
import { DashboardTab } from './tabs/dashboard.jsx'
import { DevicesTab } from './tabs/devices.jsx'
import { HistoryTab } from './tabs/history.jsx'
import { NetworkTab } from './tabs/network.jsx'
import { NodesTab } from './tabs/nodes.jsx'
import { RoleTab } from './tabs/role.jsx'

const ALL_TABS = ['Dashboard', 'Devices', 'History', 'Nodes', 'Config', 'Network']

function Placeholder({ name }) {
  return <p class="placeholder">{name} — coming in a later milestone.</p>
}

export function App() {
  const [tab, setTab] = useState('Dashboard')
  // Optimistically 'main' so an existing hub (the overwhelmingly common
  // case) renders its tabs immediately instead of flashing a loading state
  // -- only a device that has never chosen a role (fresh out of the box)
  // ever flips this to 'unset' once /api/v1/status answers.
  const [role, setRole] = useState('main')

  function refreshRole() {
    fetch('/api/v1/status')
      .then((r) => r.json())
      .then((st) => {
        const r = st.role || 'main'   // pre-M5a hubs have no "role" field
        setRole(r)
        // An unpaired node reaching the webui at all means it's sitting in
        // its portal after a failed pairing attempt (a paired node runs no
        // web server, and a searching one hasn't set up webserver either)
        // -- Dashboard/Devices/History are all empty and pointless there,
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
      <RoleTab
        onMainChosen={() => {
          setTab('Network')
          refreshRole()
        }}
      />
    )
  }

  // Nodes only means anything on a hub (unset/main); a node device runs no
  // node-management surface of its own, so hide the tab there rather than
  // rendering an empty/confusing list.
  const TABS = role === 'node' ? ALL_TABS.filter((t) => t !== 'Nodes') : ALL_TABS

  return (
    <div class="app">
      <header>
        <h1>PlantHub</h1>
        <nav>
          {TABS.map((t) => (
            <button key={t} class={t === tab ? 'active' : ''} onClick={() => setTab(t)}>
              {t}
            </button>
          ))}
        </nav>
      </header>
      <main>
        {tab === 'Dashboard' ? <DashboardTab /> :
         tab === 'Devices' ? <DevicesTab /> :
         tab === 'History' ? <HistoryTab /> :
         tab === 'Nodes' ? <NodesTab /> :
         tab === 'Config' ? <ConfigTab /> :
         tab === 'Network' ? <NetworkTab /> :
         <Placeholder name={tab} />}
      </main>
    </div>
  )
}
