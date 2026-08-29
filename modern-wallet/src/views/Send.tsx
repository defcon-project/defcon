import { Coins, FileText, Landmark, ShieldCheck } from 'lucide-react'
import { useState } from 'react'
import { sendToAddress, validateAddress } from '../api'
import { StatusPill } from '../components'
import { formatDfcn, isValidAmount } from '../format'
import type { WalletSummary } from '../types'

type Stage = 'edit' | 'confirm' | 'sending'

export function SendView({ summary, notify, refreshAll }: { summary: WalletSummary | null; notify: (message: string) => void; refreshAll: () => void }) {
  const [address, setAddress] = useState('')
  const [amount, setAmount] = useState('')
  const [label, setLabel] = useState('')
  const [stage, setStage] = useState<Stage>('edit')
  const [problem, setProblem] = useState<string | null>(null)
  const [sentTxid, setSentTxid] = useState<string | null>(null)

  const spendable = summary?.balances.trusted ?? 0

  const review = async () => {
    setProblem(null)
    setSentTxid(null)
    if (!isValidAmount(amount)) {
      setProblem('Enter a positive amount with at most 8 decimal places.')
      return
    }
    if (Number(amount) > spendable) {
      setProblem(`Amount exceeds the spendable balance (${formatDfcn(spendable)} DFCN).`)
      return
    }
    try {
      if (!(await validateAddress(address.trim()))) {
        setProblem('The node rejected this address as invalid.')
        return
      }
    } catch (e) {
      setProblem(e instanceof Error ? e.message : String(e))
      return
    }
    setStage('confirm')
  }

  const send = async () => {
    setStage('sending')
    try {
      const txid = await sendToAddress(address.trim(), amount, label || undefined)
      setSentTxid(txid)
      setStage('edit')
      setAddress('')
      setAmount('')
      setLabel('')
      notify('Transaction submitted to the network.')
      refreshAll()
    } catch (e) {
      setStage('edit')
      setProblem(e instanceof Error ? e.message : String(e))
    }
  }

  return (
    <div className="two-column-layout">
      <section className="panel form-panel">
        <div className="panel-heading"><div><p className="section-eyebrow">New payment</p><h2>Transaction details</h2></div><StatusPill tone={summary ? 'online' : 'warning'}>{summary ? summary.walletName || 'wallet' : 'no wallet'}</StatusPill></div>
        <label className="field"><span>Recipient address</span><div className="input-shell"><Landmark size={17} /><input value={address} onChange={(event) => setAddress(event.target.value)} placeholder="Enter a DeFCoN address" spellCheck={false} /></div></label>
        <div className="field-grid">
          <label className="field"><span>Amount</span><div className="input-shell"><Coins size={17} /><input value={amount} onChange={(event) => setAmount(event.target.value)} placeholder="0.00" inputMode="decimal" /><b>DFCN</b></div></label>
          <label className="field"><span>Fee</span><select disabled><option>Node default (per kB)</option></select></label>
        </div>
        <label className="field"><span>Label</span><div className="input-shell"><FileText size={17} /><input value={label} onChange={(event) => setLabel(event.target.value)} placeholder="Optional wallet label" /></div></label>
        {problem ? <div className="error-note" role="alert"><strong>Cannot send</strong><span>{problem}</span></div> : null}
        {sentTxid ? <div className="success-note">Broadcast — txid <code>{sentTxid}</code></div> : null}
        <div className="form-actions">
          <button className="button secondary" onClick={() => { setAddress(''); setAmount(''); setLabel(''); setProblem(null) }}>Clear</button>
          <button className="button primary" disabled={!address || !amount || stage !== 'edit'} onClick={() => void review()}><ShieldCheck size={17} /> Review transaction</button>
        </div>
      </section>

      <aside className="panel summary-panel">
        <div className="panel-heading"><div><p className="section-eyebrow">Before signing</p><h2>Payment summary</h2></div></div>
        <div className="summary-amount"><span>You send</span><strong>{amount || '0.00'} DFCN</strong><small>Fee decided by the node at signing</small></div>
        <div className="summary-list">
          <div><span>From wallet</span><strong>{summary?.walletName || '—'}</strong></div>
          <div><span>Spendable now</span><strong>{formatDfcn(spendable)} DFCN</strong></div>
          <div><span>After send</span><strong>{isValidAmount(amount) ? formatDfcn(Math.max(0, spendable - Number(amount))) : formatDfcn(spendable)} DFCN</strong></div>
          <div><span>Coin control</span><strong>Automatic selection</strong></div>
        </div>
        <div className="security-note"><ShieldCheck size={19} /><div><strong>Core signs locally</strong><span>This interface never receives private keys; the node validates and signs.</span></div></div>
      </aside>

      {stage !== 'edit' ? (
        <div className="command-backdrop" onMouseDown={() => stage === 'confirm' && setStage('edit')}>
          <section className="command-palette confirm-dialog" onMouseDown={(event) => event.stopPropagation()}>
            <h2>Confirm payment</h2>
            <div className="summary-list">
              <div><span>To</span><strong className="mono-break">{address.trim()}</strong></div>
              <div><span>Amount</span><strong>{amount} DFCN</strong></div>
              {label ? <div><span>Label</span><strong>{label}</strong></div> : null}
            </div>
            <p className="confirm-warning">This signs and broadcasts immediately. There is no undo.</p>
            <div className="form-actions">
              <button className="button secondary" disabled={stage === 'sending'} onClick={() => setStage('edit')}>Back</button>
              <button className="button primary" disabled={stage === 'sending'} onClick={() => void send()}>
                {stage === 'sending' ? 'Sending…' : `Send ${amount} DFCN`}
              </button>
            </div>
          </section>
        </div>
      ) : null}
    </div>
  )
}
