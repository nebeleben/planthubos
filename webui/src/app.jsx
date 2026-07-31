import { useState } from 'preact/hooks'
import { DashboardTab } from './tabs/dashboard.jsx'
import { NetworkTab } from './tabs/network.jsx'

const TABS = ['Dashboard', 'Devices', 'History', 'Config', 'Network']

function Placeholder({ name }) {
  return <p class="placeholder">{name} — coming in a later milestone.</p>
}

export function App() {
  const [tab, setTab] = useState('Dashboard')
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
         tab === 'Network' ? <NetworkTab /> :
         <Placeholder name={tab} />}
      </main>
    </div>
  )
}
