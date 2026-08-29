import type { LucideIcon } from 'lucide-react'
import { ArrowDownLeft, ArrowUpRight, Coins, Zap } from 'lucide-react'
import type { ReactNode } from 'react'
import { formatDfcn, formatTimeAgo } from './format'
import type { TxItem } from './types'

export function BrandGlyph({ compact = false }: { compact?: boolean }) {
  return (
    <div className={`brand-glyph ${compact ? 'compact' : ''}`} aria-hidden="true">
      <svg viewBox="0 0 64 64" role="img">
        <defs>
          <linearGradient id="defcon-gradient" x1="7" y1="5" x2="57" y2="59" gradientUnits="userSpaceOnUse">
            <stop stopColor="#2fb8ff" />
            <stop offset="1" stopColor="#37d69c" />
          </linearGradient>
        </defs>
        <path d="M14 10h19c14 0 24 9 24 22S47 54 33 54H14l9-10h10c8 0 13-5 13-12s-5-12-13-12H23L14 10Z" fill="url(#defcon-gradient)" />
        <path d="M8 14h13l9 10H18l-5 7h18l-9 10H8l11-14L8 14Z" fill="#f4f8ff" fillOpacity=".96" />
      </svg>
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
