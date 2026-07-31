import { useEffect, useState } from 'preact/hooks'
import { DashboardTab } from './tabs/dashboard.jsx'
import { DevicesTab } from './tabs/devices.jsx'
import { NetworkTab } from './tabs/network.jsx'

const TABS = ['Dashboard', 'Devices', 'History', 'Config', 'Network']

function Placeholder({ name }) {
  return <p class="placeholder">{name} — coming in a later milestone.</p>
}

export function App() {
  const [tab, setTab] = useState('Dashboard')

  useEffect(() => {
    fetch('/api/v1/status')
      .then((r) => r.json())
      .then((st) => {
        if (!st.time_synced) {
          fetch('/api/v1/time', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ epoch_s: Math.floor(Date.now() / 1000) }),
          })
        }
      })
      .catch(() => {})
  }, [])

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
         tab === 'Network' ? <NetworkTab /> :
         <Placeholder name={tab} />}
      </main>
    </div>
  )
}
