import { RefreshCw } from 'lucide-react'
import { useEffect, useState } from 'react'
import {
  getGuiMode,
  listWallets,
  previewDefcondArgs,
  selectWallet,
  setGuiMode,
  stopNode,
} from '../api'
import { StatusPill } from '../components'
import type { ConnectionInfo, GuiMode } from '../types'

export function SettingsView({
  connection,
  notify,
  onReconnect,
}: {
  connection: ConnectionInfo
  notify: (message: string) => void
  onReconnect: () => void
}) {
  const [mode, setMode] = useState<GuiMode | null>(null)
  const [args, setArgs] = useState<string[]>([])
  const [wallets, setWallets] = useState<string[]>([])
  const [saving, setSaving] = useState(false)

  useEffect(() => {
    void getGuiMode().then(setMode).catch(() => setMode(null))
    void previewDefcondArgs().then(setArgs).catch(() => setArgs([]))
    void listWallets().then(setWallets).catch(() => setWallets([]))
  }, [])

  const saveMode = async (next: GuiMode) => {
    setSaving(true)
    try {
      const saved = await setGuiMode(next)
      setMode(saved)
      notify(`Saved: guimode=${saved}. The launcher opens the ${saved} wallet from the next start.`)
    } catch (e) {
      notify(e instanceof Error ? e.message : String(e))
    } finally {
      setSaving(false)
    }
  }

  return (
    <div className="settings-layout">
      <section className="panel settings-panel">
        <div className="panel-heading"><div><p className="section-eyebrow">Experience</p><h2>Wallet interface</h2></div></div>
        <div className="setting-row">
          <div><strong>GUI mode</strong><span>Which wallet the DeFCoN launcher opens. Written to gui.conf in the datadir root — never into defcon.conf.</span></div>
          <div className="segmented">
            <button className={mode === 'classic' ? 'active' : ''} disabled={saving || mode === null} onClick={() => void saveMode('classic')}>Classic Qt</button>
            <button className={mode === 'modern' ? 'active' : ''} disabled={saving || mode === null} onClick={() => void saveMode('modern')}>Modern</button>
          </div>
        </div>
        <div className="setting-row">
          <div><strong>Active wallet</strong><span>Multiwallet nodes route every wallet call to this wallet.</span></div>
          <select
            value={connection.wallet ?? ''}
            onChange={(event) => {
              const name = event.target.value
              void selectWallet(name)
                .then(() => { notify(`Wallet selected: ${name}`); onReconnect() })
                .catch((e) => notify(e instanceof Error ? e.message : String(e)))
            }}
          >
            {connection.wallet === null ? <option value="">(none)</option> : null}
            {wallets.map((w) => <option key={w} value={w}>{w || '(default)'}</option>)}
          </select>
        </div>
        <div className="restart-note"><RefreshCw size={18} /><div><strong>Two wallets, one datadir</strong><span>The classic and modern wallets share the same Core data and wallet files, but only one may hold the datadir at a time. Close one before opening the other.</span></div></div>
        {connection.managed ? (
          <button
            className="button secondary settings-save"
            onClick={() => { void stopNode(false).then(() => { notify('Node stopped.'); onReconnect() }) }}
          >
            Stop the wallet-managed node
          </button>
        ) : null}
      </section>

      <aside className="panel config-preview">
        <div className="panel-heading"><div><p className="section-eyebrow">Connection</p><h2>Core link</h2></div></div>
        <div className="summary-list">
          <div><span>State</span><StatusPill tone={connection.connected ? 'online' : 'offline'}>{connection.connected ? 'connected' : 'not connected'}</StatusPill></div>
          <div><span>Chain</span><strong>{connection.chain}</strong></div>
          <div><span>Datadir</span><strong className="mono-break">{connection.datadir}</strong></div>
          <div><span>Node ownership</span><strong>{connection.managed ? 'started by this wallet' : 'external / already running'}</strong></div>
          <div><span>RPC credentials</span><strong>cookie, read by the Rust bridge only</strong></div>
        </div>
        <p className="section-eyebrow" style={{ marginTop: 18 }}>Managed defcond command line</p>
        <code>defcond {args.join(' ')}</code>
      </aside>
    </div>
  )
}
