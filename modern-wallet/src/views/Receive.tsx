import { Copy, RefreshCw } from 'lucide-react'
import { useCallback, useEffect, useState } from 'react'
import { copyText, getNewAddress, getReceiveAddresses } from '../api'
import { ErrorNote, LoadingNote, StatusPill } from '../components'
import { formatDfcn } from '../format'
import type { ReceiveAddress } from '../types'

/** A deterministic visual fingerprint of the address — not a scannable QR
 * (that needs a QR library), but stable per address so two addresses never
 * look alike. Marked clearly as a fingerprint in the UI. */
function AddressFingerprint({ address }: { address: string }) {
  const cells = Array.from({ length: 169 }, (_, index) => {
    let h = 2166136261
    const key = `${address}:${index}`
    for (let i = 0; i < key.length; i++) {
      h ^= key.charCodeAt(i)
      h = Math.imul(h, 16777619)
    }
    const row = Math.floor(index / 13)
    const col = index % 13
    const finder = (row < 4 && col < 4) || (row < 4 && col > 8) || (row > 8 && col < 4)
    return <i className={finder || (h >>> 0) % 5 < 2 ? 'filled' : ''} key={index} />
  })
  return <div className="mock-qr" aria-label="Address visual fingerprint">{cells}</div>
}

export function ReceiveView({ notify }: { notify: (message: string) => void }) {
  const [current, setCurrent] = useState<string | null>(null)
  const [known, setKnown] = useState<ReceiveAddress[] | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [label, setLabel] = useState('')
  const [busy, setBusy] = useState(false)

  const load = useCallback(async () => {
    try {
      const list = await getReceiveAddresses()
      setKnown(list)
      setError(null)
      setCurrent((existing) => existing ?? list[0]?.address ?? null)
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const fresh = async () => {
    setBusy(true)
    try {
      const address = await getNewAddress(label)
      setCurrent(address)
      notify('New address created by the Core wallet.')
      await load()
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="receive-layout">
      <section className="panel receive-card">
        <div className="panel-heading"><div><p className="section-eyebrow">Current receiving address</p><h2>Share to receive DFCN</h2></div><StatusPill tone={current ? 'online' : 'warning'}>{current ? 'Ready' : 'No address yet'}</StatusPill></div>
        {current ? (
          <>
            <div className="qr-stage"><AddressFingerprint address={current} /><div className="qr-halo" /></div>
            <div className="address-box"><span>{current}</span><button className="icon-button" onClick={() => { void copyText(current).then(() => notify('Address copied to clipboard.')) }} aria-label="Copy address"><Copy size={17} /></button></div>
          </>
        ) : (
          <LoadingNote label={error ?? 'Create the first address below.'} />
        )}
        <label className="field"><span>Label for the next address</span><div className="input-shell"><input value={label} onChange={(event) => setLabel(event.target.value)} placeholder="Invoice or contact name" /></div></label>
        <div className="button-row"><button className="button primary" disabled={busy} onClick={() => void fresh()}><RefreshCw size={16} /> {busy ? 'Creating…' : 'New address'}</button></div>
      </section>

      <section className="panel request-panel">
        <div className="panel-heading"><div><p className="section-eyebrow">Wallet addresses</p><h2>Known receiving addresses</h2></div></div>
        {error && known === null ? <ErrorNote message={error} onRetry={() => void load()} /> : null}
        {known === null && !error ? <LoadingNote /> : null}
        <div className="transaction-list">
          {(known ?? []).map((entry) => (
            <article className="transaction-row" key={entry.address}>
              <div className="transaction-main">
                <strong>{entry.label || 'no label'}</strong>
                <span>{entry.address}</span>
              </div>
              <div className="transaction-side">
                <strong>{formatDfcn(entry.amount)} DFCN</strong>
                <span>received in total</span>
              </div>
              <button className="icon-button" onClick={() => { setCurrent(entry.address) }} aria-label="Show this address">→</button>
            </article>
          ))}
          {known !== null && known.length === 0 ? <LoadingNote label="No addresses with history yet." /> : null}
        </div>
      </section>
    </div>
  )
}
