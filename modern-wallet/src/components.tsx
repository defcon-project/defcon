import type { LucideIcon } from 'lucide-react'
import { ArrowDownLeft, ArrowUpRight, Coins, Zap } from 'lucide-react'
import type { ReactNode } from 'react'
import { formatDfcn, formatTimeAgo } from './format'
import type { TxItem } from './types'

import defconLogo from './assets/defcon-logo.png'

export function BrandGlyph({ compact = false }: { compact?: boolean }) {
  // The project's real logo, straight from the Core Qt resources
  // (src/qt/res/icons), so the two wallets share one identity.
  return (
    <div className={`brand-glyph ${compact ? 'compact' : ''}`} aria-hidden="true">
      <img src={defconLogo} alt="" draggable={false} />
    </div>
  )
}

export function StatusPill({ children, tone = 'online' }: { children: ReactNode; tone?: 'online' | 'busy' | 'warning' | 'offline' }) {
  return <span className={`status status-${tone}`}>{children}</span>
}

export function MetricCard({ icon: Icon, label, value, hint, tone }: { icon: LucideIcon; label: string; value: string; hint: string; tone: 'green' | 'blue' | 'amber' | 'red' }) {
  return (
    <article className="kpi-card">
      <span className={`kpi-icon ${tone}`}><Icon size={19} /></span>
      <div className="kpi-copy">
        <span>{label}</span>
        <strong>{value}</strong>
        <small>{hint}</small>
      </div>
    </article>
  )
}

export function TransactionRow({ transaction, detailed = false }: { transaction: TxItem; detailed?: boolean }) {
  const Icon = transaction.kind === 'sent' ? ArrowUpRight : transaction.kind === 'staking' ? Zap : transaction.kind === 'other' ? Coins : ArrowDownLeft
  const title =
    transaction.label ||
    (transaction.kind === 'sent' ? 'Sent' : transaction.kind === 'staking' ? 'Stake reward' : transaction.kind === 'received' ? 'Received' : transaction.categoryRaw || 'Transaction')
  return (
    <article className={`transaction-row ${detailed ? 'detailed' : ''}`}>
      <span className={`transaction-icon tx-${transaction.kind === 'other' ? 'masternode' : transaction.kind}`}><Icon size={17} /></span>
      <div className="transaction-main">
        <strong>{title}</strong>
        <span>{transaction.address || transaction.txid.slice(0, 20) + '…'}</span>
      </div>
      {detailed ? <span className="transaction-confirmations">{transaction.confirmations.toLocaleString()} confirmations</span> : null}
      <div className="transaction-side">
        <strong className={transaction.amount >= 0 ? 'amount-positive' : 'amount-negative'}>
          {transaction.amount >= 0 ? '+' : '−'}{formatDfcn(Math.abs(transaction.amount))} DFCN
        </strong>
        <span>{formatTimeAgo(transaction.time)}</span>
      </div>
    </article>
  )
}

/** A panel-level load failure: what failed and a retry, nothing hidden. */
export function ErrorNote({ message, onRetry }: { message: string; onRetry?: () => void }) {
  return (
    <div className="error-note" role="alert">
      <strong>Could not load</strong>
      <span>{message}</span>
      {onRetry ? <button className="button secondary" onClick={onRetry}>Retry</button> : null}
    </div>
  )
}

export function LoadingNote({ label = 'Loading…' }: { label?: string }) {
  return <div className="loading-note">{label}</div>
}
