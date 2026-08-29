// Number and time presentation, shared by every view.

export const formatDfcn = (value: number) =>
  new Intl.NumberFormat('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 }).format(value)

export const formatCompact = (value: number) =>
  new Intl.NumberFormat('en-US', { notation: 'compact', maximumFractionDigits: 2 }).format(value)

export const formatTimeAgo = (unixSeconds: number): string => {
  if (!unixSeconds) return '—'
  const delta = Math.max(0, Math.floor(Date.now() / 1000) - unixSeconds)
  if (delta < 60) return `${delta}s ago`
  if (delta < 3600) return `${Math.floor(delta / 60)}m ago`
  if (delta < 86400) return `${Math.floor(delta / 3600)}h ${Math.floor((delta % 3600) / 60)}m ago`
  return `${Math.floor(delta / 86400)}d ago`
}

export const formatDuration = (seconds: number | null): string => {
  if (seconds === null || !Number.isFinite(seconds)) return '—'
  if (seconds < 60) return `${Math.round(seconds)}s`
  if (seconds < 3600) return `${Math.round(seconds / 60)}m`
  return `${Math.floor(seconds / 3600)}h ${Math.round((seconds % 3600) / 60)}m`
}

export const shortHash = (hash: string, keep = 10) =>
  hash.length > keep * 2 ? `${hash.slice(0, keep)}…${hash.slice(-keep)}` : hash

/** Decimal-string check for amounts the node will parse (up to 8 places). */
export const isValidAmount = (value: string): boolean =>
  /^\d+(\.\d{1,8})?$/.test(value.trim()) && Number(value) > 0
