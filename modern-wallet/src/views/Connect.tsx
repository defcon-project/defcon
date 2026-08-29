import { Plug, Power } from 'lucide-react'
import { useState } from 'react'
import { connect, setChain, startNode } from '../api'
import { BrandGlyph } from '../components'
import type { ConnectionInfo } from '../types'

type ChainChoice = 'main' | 'testnet' | 'regtest' | 'devnet'

/**
 * The gate in front of the wallet: pick the network, then either attach to a
 * node that is already running or start one. Every failure — including the
 * datadir being locked by the classic wallet — lands here in plain words.
 */
export function ConnectScreen({
  connection,
  onConnected,
}: {
  connection: ConnectionInfo
  onConnected: (info: ConnectionInfo) => void
}) {
  const [chain, setChainChoice] = useState<ChainChoice>('main')
  const [devnetName, setDevnetName] = useState('')
  const [datadir, setDatadir] = useState('')
  const [busy, setBusy] = useState<'attach' | 'start' | null>(null)
  const [problem, setProblem] = useState<string | null>(null)

  const applyChain = async () => {
    await setChain(chain, chain === 'devnet' ? devnetName.trim() : undefined, datadir.trim() || undefined)
  }

  const attach = async () => {
    setBusy('attach')
    setProblem(null)
    try {
      await applyChain()
      onConnected(await connect())
    } catch (e) {
      setProblem(e instanceof Error ? e.message : String(e))
    } finally {
      setBusy(null)
    }
  }

  const boot = async () => {
    setBusy('start')
    setProblem(null)
    try {
      await applyChain()
      onConnected(await startNode())
    } catch (e) {
      setProblem(e instanceof Error ? e.message : String(e))
    } finally {
      setBusy(null)
    }
  }

  return (
    <div className="connect-stage">
      <section className="panel connect-card">
        <div className="connect-brand"><BrandGlyph /><div><strong>DeFCoN Wallet</strong><span>Modern interface over DeFCoN Core</span></div></div>

        <label className="field"><span>Network</span>
          <select value={chain} onChange={(event) => setChainChoice(event.target.value as ChainChoice)}>
            <option value="main">Mainnet</option>
            <option value="testnet">Testnet</option>
            <option value="devnet">Devnet</option>
            <option value="regtest">Regtest</option>
          </select>
        </label>
        {chain === 'devnet' ? (
          <label className="field"><span>Devnet name</span><div className="input-shell"><input value={devnetName} onChange={(event) => setDevnetName(event.target.value)} placeholder="e.g. defcon-q60" spellCheck={false} /></div></label>
        ) : null}
        <label className="field"><span>Datadir (empty for the platform default)</span><div className="input-shell"><input value={datadir} onChange={(event) => setDatadir(event.target.value)} placeholder="default" spellCheck={false} /></div></label>

        {problem ? (
          <div className="error-note" role="alert">
            <strong>Not connected</strong>
            <span>{problem}</span>
            <span className="table-note">If the classic Qt wallet is open on this datadir, close it first — the two wallets share data but cannot hold it at once.</span>
          </div>
        ) : null}

        <div className="form-actions">
          <button className="button secondary" disabled={busy !== null || (chain === 'devnet' && !devnetName.trim())} onClick={() => void attach()}>
            <Plug size={16} /> {busy === 'attach' ? 'Connecting…' : 'Connect to running node'}
          </button>
          <button className="button primary" disabled={busy !== null || (chain === 'devnet' && !devnetName.trim())} onClick={() => void boot()}>
            <Power size={16} /> {busy === 'start' ? 'Starting node…' : 'Start node and connect'}
          </button>
        </div>
        <p className="table-note">Last state: {connection.connected ? 'connected' : 'not connected'} · datadir {connection.datadir}</p>
      </section>
    </div>
  )
}
