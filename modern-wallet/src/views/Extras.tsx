// The three read-only network views: CoinJoin, Masternodes, Governance.
// Each renders live node data; mutating actions (start mixing, cast votes)
// are deliberately absent until they can be built with the same confirm-first
// discipline as Send.

import { Blocks, CheckCircle2, Clock3, Coins, Network, RefreshCw, ShieldCheck } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { ErrorNote, LoadingNote, MetricCard, StatusPill } from '../components'
import { formatCompact, shortHash } from '../format'
import type { CoinJoinInfo, MasternodeEntry, Proposal } from '../types'

export function CoinJoinView({ info, error, onRetry }: { info: CoinJoinInfo | null; error: string | null; onRetry: () => void }) {
  return (
    <div className="view-stack">
      <section className="feature-hero coinjoin-hero">
        <div>
          <span className="feature-icon"><ShieldCheck size={26} /></span>
          <p className="section-eyebrow">Privacy pool</p>
          <h2>CoinJoin status</h2>
          <p>Live state of the Core mixing engine. Starting and stopping sessions from here is a planned addition; until then use the RPC console or the classic wallet.</p>
        </div>
        <div className="privacy-score"><span>Mixing engine</span><strong>{info ? (info.running ? 'ON' : 'OFF') : '…'}</strong><small>{info?.enabled ? 'enabled' : 'disabled'}</small></div>
      </section>
      {error ? <ErrorNote message={error} onRetry={onRetry} /> : null}
      <section className="kpi-grid three">
        <MetricCard icon={Coins} label="Target amount" value={info ? formatCompact(info.amount) : '…'} hint="DFCN configured" tone="blue" />
        <MetricCard icon={Blocks} label="Rounds" value={info ? String(info.rounds) : '…'} hint={`keys left ${info?.keysLeft ?? '…'}`} tone="amber" />
        <MetricCard icon={Clock3} label="Sessions" value={info ? String(info.sessions) : '…'} hint={info?.running ? 'mixing now' : 'no active mixing round'} tone={info?.running ? 'green' : 'blue'} />
      </section>
    </div>
  )
}

export function MasternodesView({ list, error, onRetry }: { list: MasternodeEntry[] | null; error: string | null; onRetry: () => void }) {
  const [scope, setScope] = useState<'mine' | 'all'>('all')
  const mineCount = list?.filter((mn) => mn.mine).length ?? 0
  // On the first load, open on "My nodes" when the wallet actually owns some;
  // otherwise stay on "All". Only auto-selects once, so a later manual switch
  // is never overridden.
  const autoPicked = useRef(false)
  useEffect(() => {
    if (autoPicked.current || list === null) return
    autoPicked.current = true
    if (mineCount > 0) setScope('mine')
  }, [list, mineCount])
  const shown = scope === 'mine' ? (list ?? []).filter((mn) => mn.mine) : (list ?? [])
  const enabled = shown.filter((mn) => mn.status === 'ENABLED').length
  const banned = shown.filter((mn) => mn.status === 'POSE_BANNED').length
  return (
    <div className="view-stack">
      <section className="kpi-grid three">
        <MetricCard icon={Network} label={scope === 'mine' ? 'My masternodes' : 'Registered'} value={list ? String(shown.length) : '…'} hint={scope === 'mine' ? 'owned by this wallet' : 'deterministic list entries'} tone="blue" />
        <MetricCard icon={CheckCircle2} label="Enabled" value={list ? String(enabled) : '…'} hint="in the active set" tone="green" />
        <MetricCard icon={ShieldCheck} label="PoSe banned" value={list ? String(banned) : '…'} hint="penalty reached the ban score" tone={banned ? 'red' : 'green'} />
      </section>
      <section className="panel">
        <div className="panel-heading">
          <div><p className="section-eyebrow">Network list</p><h2>Masternodes</h2></div>
          <div className="mn-toolbar">
            <div className="segmented">
              <button className={scope === 'mine' ? 'active' : ''} onClick={() => setScope('mine')}>My nodes{list ? ` (${mineCount})` : ''}</button>
              <button className={scope === 'all' ? 'active' : ''} onClick={() => setScope('all')}>All{list ? ` (${list.length})` : ''}</button>
            </div>
            <button className="button secondary" onClick={onRetry}><RefreshCw size={16} /> Refresh</button>
          </div>
        </div>
        {error ? <ErrorNote message={error} onRetry={onRetry} /> : null}
        {list === null && !error ? <LoadingNote /> : null}
        <div className="data-table mn-table">
          <div className="data-head"><span>ProTx</span><span>Service</span><span>Status</span><span>PoSe</span><span>Last paid</span></div>
          {shown.slice(0, 200).map((mn) => (
            <div className={`data-row ${mn.mine ? 'is-mine' : ''}`} key={mn.proTxHash}>
              <code>{shortHash(mn.proTxHash, 8)}{mn.mine ? <span className="mine-tag">mine</span> : null}</code>
              <code>{mn.address}</code>
              <StatusPill tone={mn.status === 'ENABLED' ? 'online' : mn.status === 'POSE_BANNED' ? 'offline' : 'warning'}>{mn.status}</StatusPill>
              <span>{mn.posePenalty}</span>
              <span>{mn.lastPaidHeight ? `#${mn.lastPaidHeight.toLocaleString()}` : '—'}</span>
            </div>
          ))}
          {list !== null && shown.length === 0 ? <LoadingNote label={scope === 'mine' ? 'This wallet owns no masternodes.' : 'No masternodes.'} /> : null}
        </div>
        {shown.length > 200 ? <p className="table-note">Showing the first 200 of {shown.length}.</p> : null}
      </section>
    </div>
  )
}

export function GovernanceView({ proposals, error, onRetry }: { proposals: Proposal[] | null; error: string | null; onRetry: () => void }) {
  return (
    <div className="view-stack">
      <section className="panel">
        <div className="panel-heading"><div><p className="section-eyebrow">Active cycle</p><h2>Governance proposals</h2></div><StatusPill tone="busy">{proposals ? `${proposals.length} active` : '…'}</StatusPill></div>
        {error ? <ErrorNote message={error} onRetry={onRetry} /> : null}
        {proposals === null && !error ? <LoadingNote /> : null}
        <div className="proposal-list">
          {(proposals ?? []).map((proposal) => {
            const total = proposal.yes + proposal.no
            const percent = total > 0 ? Math.round((proposal.yes / total) * 100) : 0
            return (
              <article className="proposal-row" key={proposal.hash}>
                <div>
                  <strong>{proposal.name}</strong>
                  <span>{proposal.paymentAmount ? `requests ${proposal.paymentAmount} DFCN` : shortHash(proposal.hash, 8)}</span>
                </div>
                <div className="vote-meter">
                  <div className="meter"><div className="meter-fill green" style={{ width: `${percent}%` }} /></div>
                  <span>{proposal.yes} yes · {proposal.no} no · {proposal.abstain} abstain</span>
                </div>
                <div className="vote-actions"><span className="table-note">voting from the wallet is planned</span></div>
              </article>
            )
          })}
          {proposals !== null && proposals.length === 0 ? <LoadingNote label="No active proposals." /> : null}
        </div>
      </section>
    </div>
  )
}
