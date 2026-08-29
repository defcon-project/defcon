import { Check, Eye, EyeOff, History, Network, QrCode, Send, ShieldCheck, TrendingUp, WalletCards, Zap } from 'lucide-react'
import { useState } from 'react'
import { ErrorNote, LoadingNote, MetricCard, StatusPill, TransactionRow } from '../components'
import { formatCompact, formatDfcn, formatDuration } from '../format'
import type { NodeStatus, TxItem, WalletSummary } from '../types'
import type { View } from '../nav'

export function Overview({
  status,
  summary,
  transactions,
  txError,
  onNavigate,
}: {
  status: NodeStatus | null
  summary: WalletSummary | null
  transactions: TxItem[] | null
  txError: string | null
  onNavigate: (view: View) => void
}) {
  const [balanceVisible, setBalanceVisible] = useState(true)
  const balances = summary?.balances
  const total = balances ? balances.trusted + balances.pending + balances.immature + balances.watchonly : null
  const staking = summary?.staking ?? null
  // GuessVerificationProgress estimates from a per-chain tx-rate table this
  // fork inherited from its parent, so on a young chain it reads near zero at
  // the tip. Header distance is the signal the node itself acts on: caught up
  // means blocks have reached the best known header.
  const blocks = status?.blockchain.blocks ?? 0
  const headers = status?.blockchain.headers ?? 0
  const synced = status !== null && blocks > 0 && blocks >= headers
  const progress = status === null || headers === 0 ? 0 : Math.min(100, Math.floor((blocks / headers) * 100))

  return (
    <div className="view-stack">
      <section className="wallet-hero">
        <div className="hero-content">
          <div className="hero-kicker"><WalletCards size={15} /> Total wallet balance</div>
          <div className="hero-balance-line">
            <strong>{total === null ? '…' : balanceVisible ? formatDfcn(total) : '••••••••'}</strong>
            <span>DFCN</span>
            <button className="ghost-icon" onClick={() => setBalanceVisible((value) => !value)} aria-label="Toggle balance visibility">
              {balanceVisible ? <Eye size={30} /> : <EyeOff size={30} />}
            </button>
          </div>
          <div className="hero-metrics">
            <div><span>Spendable</span><strong>{balances ? (balanceVisible ? formatDfcn(balances.trusted) : '••••••') : '…'}</strong></div>
            <div><span>Watch-only</span><strong>{balances ? (balanceVisible ? formatDfcn(balances.watchonly) : '••••••') : '…'}</strong></div>
            <div><span>Pending + immature</span><strong>{balances ? formatDfcn(balances.pending + balances.immature) : '…'}</strong></div>
          </div>
          <div className="hero-actions">
            <button className="button primary" onClick={() => onNavigate('send')}><Send size={17} /> Send</button>
            <button className="button secondary" onClick={() => onNavigate('receive')}><QrCode size={17} /> Receive</button>
            <button className="button secondary" onClick={() => onNavigate('transactions')}><History size={17} /> History</button>
          </div>
        </div>
      </section>

      <section className="kpi-grid">
        <MetricCard icon={TrendingUp} label="Block height" value={status ? status.blockchain.blocks.toLocaleString() : '…'} hint={status ? `headers ${status.blockchain.headers.toLocaleString()}` : ''} tone="blue" />
        <MetricCard icon={Zap} label="Stake weight" value={staking ? formatCompact(staking.weight) : '—'} hint={staking?.expectedtime ? `expected reward in ~${formatDuration(staking.expectedtime)}` : 'staking idle'} tone={staking?.staking ? 'green' : 'amber'} />
        <MetricCard icon={Network} label="Connections" value={status ? String(status.network.connections) : '…'} hint={status?.network.subversion.replaceAll('/', '') ?? ''} tone="blue" />
        <MetricCard icon={ShieldCheck} label="ChainLock" value={status?.chainlock ? `#${status.chainlock.height.toLocaleString()}` : '—'} hint={status?.chainlock ? 'best chainlock' : 'no chainlock seen'} tone={status?.chainlock ? 'green' : 'amber'} />
      </section>

      <section className="lower-grid">
        <article className="panel">
          <div className="panel-heading">
            <div><p className="section-eyebrow">Latest activity</p><h2>Recent transactions</h2></div>
            <button className="text-button" onClick={() => onNavigate('transactions')}>View all</button>
          </div>
          {txError ? <ErrorNote message={txError} /> : null}
          {transactions === null && !txError ? <LoadingNote /> : null}
          <div className="transaction-list">
            {(transactions ?? []).slice(0, 5).map((transaction) => <TransactionRow key={transaction.txid + transaction.categoryRaw + transaction.address} transaction={transaction} />)}
            {transactions !== null && transactions.length === 0 ? <LoadingNote label="No transactions yet." /> : null}
          </div>
        </article>

        <article className="panel network-panel">
          <div className="panel-heading"><div><p className="section-eyebrow">Core status</p><h2>Network health</h2></div><StatusPill tone={synced ? 'online' : 'busy'}>{synced ? 'Synced' : 'Syncing'}</StatusPill></div>
          <div className="health-ring"><div><Check size={27} /><strong>{progress}%</strong><span>synchronized</span></div></div>
          <div className="health-list">
            <div><span>Chain</span><strong>{status?.blockchain.chain ?? '…'}</strong></div>
            <div><span>Peers</span><strong>{status?.network.connections ?? '…'}</strong></div>
            <div><span>Staking</span><strong>{staking ? (staking.staking ? 'active' : 'inactive') : '—'}</strong></div>
            <div><span>Net stake weight</span><strong>{staking ? formatCompact(staking.netstakeweight) : '—'}</strong></div>
          </div>
        </article>
      </section>
    </div>
  )
}
