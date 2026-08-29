import { Search } from 'lucide-react'
import { useMemo, useState } from 'react'
import { ErrorNote, LoadingNote, TransactionRow } from '../components'
import type { TxItem, TxKind } from '../types'

const filters = ['all', 'received', 'sent', 'staking'] as const

type Filter = (typeof filters)[number]

export function TransactionsView({ transactions, error, onRetry }: { transactions: TxItem[] | null; error: string | null; onRetry: () => void }) {
  const [filter, setFilter] = useState<Filter>('all')
  const [query, setQuery] = useState('')

  const filtered = useMemo(() => {
    let list = transactions ?? []
    if (filter !== 'all') list = list.filter((t) => t.kind === (filter as TxKind))
    const q = query.trim().toLowerCase()
    if (q) {
      list = list.filter(
        (t) =>
          t.address.toLowerCase().includes(q) ||
          t.label.toLowerCase().includes(q) ||
          t.txid.toLowerCase().includes(q) ||
          String(Math.abs(t.amount)).includes(q),
      )
    }
    return list
  }, [transactions, filter, query])

  return (
    <section className="panel transactions-panel">
      <div className="table-toolbar">
        <div className="search-shell"><Search size={17} /><input placeholder="Search address, label, txid or amount" value={query} onChange={(event) => setQuery(event.target.value)} /></div>
        <div className="segmented filter-segments">
          {filters.map((item) => <button className={filter === item ? 'active' : ''} key={item} onClick={() => setFilter(item)}>{item}</button>)}
        </div>
      </div>
      {error ? <ErrorNote message={error} onRetry={onRetry} /> : null}
      {transactions === null && !error ? <LoadingNote /> : null}
      <div className="transaction-list detailed-list">
        {filtered.map((transaction, index) => <TransactionRow detailed key={`${transaction.txid}-${transaction.categoryRaw}-${index}`} transaction={transaction} />)}
        {transactions !== null && filtered.length === 0 ? <LoadingNote label="Nothing matches." /> : null}
      </div>
    </section>
  )
}
