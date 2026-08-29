import { Copy, RefreshCw } from 'lucide-react'
import QRCode from 'qrcode'
import { useCallback, useEffect, useRef, useState } from 'react'
import { copyText, getNewAddress, getReceiveAddresses } from '../api'
import { ErrorNote, LoadingNote, StatusPill } from '../components'
import { formatDfcn } from '../format'
import type { ReceiveAddress } from '../types'

/** A real, scannable QR of the address, drawn locally to a canvas — no network,
 * so it stays within the app's content-security policy. */
function AddressQr({ address }: { address: string }) {
  const canvasRef = useRef<HTMLCanvasElement>(null)
  useEffect(() => {
    if (!canvasRef.current) return
    void QRCode.toCanvas(canvasRef.current, address, {
      width: 190,
      margin: 1,
      color: { dark: '#0a121c', light: '#f4f8ff' },
      errorCorrectionLevel: 'M',
    }).catch(() => {})
  }, [address])
  return <canvas ref={canvasRef} className="address-qr" width={190} height={190} aria-label="Receiving address QR code" />
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
      setLabel('')
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
        <div className="panel-heading">
          <div><p className="section-eyebrow">Current receiving address</p><h2>Share to receive DFCN</h2></div>
          <StatusPill tone={current ? 'online' : 'warning'}>{current ? 'Ready' : 'No address'}</StatusPill>
        </div>

        {current ? (
          <div className="receive-body">
            <div className="qr-frame"><AddressQr address={current} /></div>
            <button
              className="address-chip"
              onClick={() => { void copyText(current).then(() => notify('Address copied to clipboard.')) }}
              title="Copy address"
            >
              <code>{current}</code>
              <Copy size={16} />
            </button>
          </div>
        ) : (
          <LoadingNote label={error ?? 'Create the first address below.'} />
        )}

        <div className="receive-new">
          <label className="field">
            <span>Label for the next address</span>
            <div className="input-shell"><input value={label} onChange={(event) => setLabel(event.target.value)} placeholder="Invoice or contact name" /></div>
          </label>
          <button className="button primary receive-new-button" disabled={busy} onClick={() => void fresh()}>
            <RefreshCw size={16} /> {busy ? 'Creating…' : 'New address'}
          </button>
        </div>
      </section>

      <section className="panel request-panel">
        <div className="panel-heading"><div><p className="section-eyebrow">Wallet addresses</p><h2>Known receiving addresses</h2></div></div>
        {error && known === null ? <ErrorNote message={error} onRetry={() => void load()} /> : null}
        {known === null && !error ? <LoadingNote /> : null}
        <div className="transaction-list">
          {(known ?? []).map((entry) => (
            <article className="transaction-row receive-known" key={entry.address}>
              <div className="transaction-main">
                <strong>{entry.label || 'no label'}</strong>
                <span>{entry.address}</span>
              </div>
              <div className="transaction-side">
                <strong>{formatDfcn(entry.amount)} DFCN</strong>
                <span>received in total</span>
              </div>
              <button className="button secondary receive-use" onClick={() => setCurrent(entry.address)}>Show</button>
            </article>
          ))}
          {known !== null && known.length === 0 ? <LoadingNote label="No addresses with history yet." /> : null}
        </div>
      </section>
    </div>
  )
}
