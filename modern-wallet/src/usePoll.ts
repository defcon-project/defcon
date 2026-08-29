import { useCallback, useEffect, useRef, useState } from 'react'

export interface Polled<T> {
  data: T | null
  error: string | null
  loading: boolean
  refresh: () => void
}

/**
 * Polls an async source. One in-flight request at a time; the interval starts
 * counting after a response, so a slow node is never stampeded. Errors replace
 * the error, never the last good data — a wallet that flickers between data
 * and blank on every hiccup is unusable.
 */
export function usePoll<T>(fn: () => Promise<T>, intervalMs: number, deps: unknown[] = []): Polled<T> {
  const [data, setData] = useState<T | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [tick, setTick] = useState(0)
  const alive = useRef(true)

  const refresh = useCallback(() => setTick((t) => t + 1), [])

  useEffect(() => {
    alive.current = true
    let timer: number | undefined
    const run = async () => {
      try {
        const result = await fn()
        if (!alive.current) return
        setData(result)
        setError(null)
      } catch (e) {
        if (!alive.current) return
        setError(e instanceof Error ? e.message : String(e))
      } finally {
        if (alive.current) {
          setLoading(false)
          timer = window.setTimeout(run, intervalMs)
        }
      }
    }
    setLoading(true)
    void run()
    return () => {
      alive.current = false
      if (timer !== undefined) window.clearTimeout(timer)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [intervalMs, tick, ...deps])

  return { data, error, loading, refresh }
}
