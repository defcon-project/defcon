import { Blocks, CheckCircle2, ChevronRight, LockKeyhole, Menu, Search, ShieldCheck, Terminal, UnlockKeyhole, WalletCards, Wifi, X } from 'lucide-react'
import { useEffect, useMemo, useState } from 'react'
import {
  getCoinJoinInfo,
  getConnection,
  getMasternodes,
  getNodeStatus,
  getProposals,
  getTransactions,
  getWalletSummary,
  isTauri,
  walletLock,
  walletUnlock,
} from './api'
import { BrandGlyph } from './components'
import { navigation, viewCopy, type View } from './nav'
import type { ConnectionInfo } from './types'
import { usePoll } from './usePoll'
import { ConnectScreen } from './views/Connect'
import { ConsoleOverlay } from './views/Console'
import { CoinJoinView, GovernanceView, MasternodesView } from './views/Extras'
import { Overview } from './views/Overview'
import { ReceiveView } from './views/Receive'
import { SendView } from './views/Send'
import { SettingsView } from './views/Settings'
import { TransactionsView } from './views/Transactions'

function CommandPalette({ open, onClose, onNavigate }: { open: boolean; onClose: () => void; onNavigate: (view: View) => void }) {
  const [query, setQuery] = useState('')
  if (!open) return null
  const items = navigation.filter((item) => item.label.toLowerCase().includes(query.toLowerCase()))
  return (
    <div className="command-backdrop" onMouseDown={onClose}>
      <section className="command-palette" onMouseDown={(event) => event.stopPropagation()}>
        <div className="command-search"><Search size={19} /><input autoFocus placeholder="Go to a wallet view…" value={query} onChange={(event) => setQuery(event.target.value)} /><kbd>ESC</kbd></div>
        <div className="command-results">{items.map((item) => { const Icon = item.icon; return <button key={item.view} onClick={() => { onNavigate(item.view); onClose() }}><span className="command-icon"><Icon size={17} /></span><span><strong>{item.label}</strong><small>Open {item.label.toLowerCase()} view</small></span><ChevronRight size={16} /></button> })}</div>
      </section>
    </div>
  )
}

function UnlockDialog({ open, onClose, notify }: { open: boolean; onClose: () => void; notify: (m: string) => void }) {
  const [passphrase, setPassphrase] = useState('')
  const [busy, setBusy] = useState(false)
  if (!open) return null
  const unlock = async () => {
    setBusy(true)
    try {
      await walletUnlock(passphrase, 300)
      notify('Wallet unlocked for 5 minutes.')
      onClose()
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e))
    } finally {
      setBusy(false)
      setPassphrase('')
    }
  }
  return (
    <div className="command-backdrop" onMouseDown={onClose}>
      <section className="command-palette confirm-dialog unlock-dialog" onMouseDown={(event) => event.stopPropagation()}>
        <h2>Unlock wallet</h2>
        <label className="field"><span>Passphrase</span><div className="input-shell"><input type="password" autoFocus value={passphrase} onChange={(event) => setPassphrase(event.target.value)} onKeyDown={(event) => { if (event.key === 'Enter') void unlock() }} /></div></label>
        <p className="table-note">Sent once to the Core wallet over the local bridge; never stored.</p>
        <div className="form-actions">
          <button className="button secondary" onClick={onClose}>Cancel</button>
          <button className="button primary" disabled={busy || !passphrase} onClick={() => void unlock()}>{busy ? 'Unlocking…' : 'Unlock'}</button>
        </div>
      </section>
    </div>
  )
}

function App() {
  const [connection, setConnection] = useState<ConnectionInfo | null>(null)
  const [activeView, setActiveView] = useState<View>('overview')
  const [sidebarOpen, setSidebarOpen] = useState(false)
  const [commandOpen, setCommandOpen] = useState(false)
  const [consoleOpen, setConsoleOpen] = useState(false)
  const [unlockOpen, setUnlockOpen] = useState(false)
  const [notice, setNotice] = useState<string | null>(null)

  useEffect(() => {
    void getConnection().then(setConnection).catch(() => setConnection(null))
  }, [])

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
        event.preventDefault()
        setCommandOpen(true)
      }
      if (event.key === 'Escape') setCommandOpen(false)
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [])

  useEffect(() => {
    if (!notice) return
    const timeout = window.setTimeout(() => setNotice(null), 4200)
    return () => window.clearTimeout(timeout)
  }, [notice])

  const connected = connection?.connected ?? false

  // Live data. Everything degrades to its own error note, never to a blank app.
  const status = usePoll(getNodeStatus, 5000, [connected])
  const summary = usePoll(getWalletSummary, 10000, [connected, connection?.wallet])
  const transactions = usePoll(() => getTransactions(200, 0), 15000, [connected, connection?.wallet])
  const masternodes = usePoll(getMasternodes, 60000, [connected])
  const proposals = usePoll(getProposals, 120000, [connected])
  const coinjoin = usePoll(getCoinJoinInfo, 30000, [connected, connection?.wallet])

  const copy = viewCopy[activeView]
  const activeLabel = useMemo(() => navigation.find((item) => item.view === activeView)?.label ?? 'Overview', [activeView])
  const navigate = (view: View) => { setActiveView(view); setSidebarOpen(false) }
  const refreshWalletData = () => { summary.refresh(); transactions.refresh() }

  const locked = summary.data?.encrypted === true && summary.data.unlockedUntil === 0

  if (connection === null) {
    return <main className="app-shell"><div className="loading-note" style={{ margin: 'auto' }}>Starting…</div></main>
  }
  if (!connected) {
    return (
      <main className="app-shell">
        <ConnectScreen connection={connection} onConnected={setConnection} />
      </main>
    )
  }

  const content = (() => {
    if (activeView === 'overview') return <Overview status={status.data} summary={summary.data} transactions={transactions.data} txError={transactions.error} onNavigate={navigate} />
    if (activeView === 'send') return <SendView summary={summary.data} notify={setNotice} refreshAll={refreshWalletData} />
    if (activeView === 'receive') return <ReceiveView notify={setNotice} />
    if (activeView === 'transactions') return <TransactionsView transactions={transactions.data} error={transactions.error} onRetry={transactions.refresh} />
    if (activeView === 'coinjoin') return <CoinJoinView info={coinjoin.data} error={coinjoin.error} onRetry={coinjoin.refresh} />
    if (activeView === 'masternodes') return <MasternodesView list={masternodes.data} error={masternodes.error} onRetry={masternodes.refresh} />
    if (activeView === 'governance') return <GovernanceView proposals={proposals.data} error={proposals.error} onRetry={proposals.refresh} />
    return <SettingsView connection={connection} notify={setNotice} onReconnect={() => void getConnection().then(setConnection)} />
  })()

  return (
    <main className="app-shell">
      <CommandPalette open={commandOpen} onClose={() => setCommandOpen(false)} onNavigate={navigate} />
      <ConsoleOverlay open={consoleOpen} onClose={() => setConsoleOpen(false)} />
      <UnlockDialog open={unlockOpen} onClose={() => { setUnlockOpen(false); summary.refresh() }} notify={setNotice} />
      {notice ? <div className="toast"><CheckCircle2 size={18} /><span>{notice}</span><button onClick={() => setNotice(null)}><X size={15} /></button></div> : null}

      <aside className={`sidebar ${sidebarOpen ? 'open' : ''}`} aria-label="Primary navigation">
        <div className="brand"><BrandGlyph compact /><div><strong>DeFCoN</strong><span>Modern Wallet</span></div></div>
        <div className="wallet-switch"><span>ACTIVE WALLET</span><button onClick={() => navigate('settings')}><WalletCards size={16} /><span><strong>{connection.wallet ?? '(none)'}</strong><small>{summary.data?.encrypted ? 'Encrypted · local' : 'Local'}</small></span><ChevronRight size={15} /></button></div>
        <nav className="nav-list">{navigation.map((item) => { const Icon = item.icon; return <button className={`nav-item ${activeView === item.view ? 'active' : ''}`} key={item.view} onClick={() => navigate(item.view)}><Icon size={18} /><span>{item.label}</span>{activeView === item.view ? <i /> : null}</button> })}</nav>
        <div className="node-chip"><span className="node-status-dot" /><div><strong>Core connected</strong><span>{status.data?.chainlock ? `ChainLocked #${status.data.chainlock.height.toLocaleString()}` : `height ${status.data?.blockchain.blocks.toLocaleString() ?? '…'}`}</span></div></div>
        <div className="sidebar-footer"><span>{status.data?.network.subversion.replaceAll('/', '') || 'DeFCoN Core'}</span>{isTauri ? null : <span className="mock-label">DEMO</span>}</div>
      </aside>

      <section className="main-panel">
        <header className="topbar">
          <button className="mobile-menu" onClick={() => setSidebarOpen((value) => !value)} aria-label="Open navigation"><Menu size={20} /></button>
          <div className="page-title"><p className="eyebrow">{copy.eyebrow}</p><h1>{copy.title}</h1><p>{copy.caption}</p></div>
          <div className="top-actions">
            <span className="api-pill"><span /> {status.error ? 'Core unreachable' : 'Core connected'}</span>
            <button className="button secondary command-button" onClick={() => setConsoleOpen(true)}><Terminal size={17} /><span>Console</span></button>
            {summary.data?.encrypted ? (
              locked ? (
                <button className="button primary" onClick={() => setUnlockOpen(true)}><UnlockKeyhole size={17} /> Unlock</button>
              ) : (
                <button className="button secondary" onClick={() => { void walletLock().then(() => { setNotice('Wallet locked.'); summary.refresh() }) }}><LockKeyhole size={17} /> Lock</button>
              )
            ) : null}
          </div>
        </header>

        {status.error ? <div className="context-bar error"><div><span className="context-icon"><Wifi size={16} /></span><span><strong>Connection problem:</strong> {status.error}</span></div></div> : null}
        {!isTauri ? <div className="context-bar"><div><span className="context-icon"><Wifi size={16} /></span><span><strong>{activeLabel}</strong> shows browser demo data — run inside Tauri for a live Core connection.</span></div></div> : null}
        {content}

        <footer className="statusbar">
          <span><span className="node-status-dot" /> {status.data?.network.connections ?? 0} active connections</span>
          <span><ShieldCheck size={14} /> {status.data?.chainlock ? 'ChainLock verified' : 'no ChainLock'}</span>
          <span><Blocks size={14} /> {status.data?.blockchain.blocks.toLocaleString() ?? '…'} blocks</span>
          <span className="statusbar-spacer" />
          <span>{connection.chain}</span>
          <strong>{connection.wallet ?? ''}</strong>
        </footer>
      </section>
    </main>
  )
}

export default App
