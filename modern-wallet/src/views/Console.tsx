import { Terminal, X } from 'lucide-react'
import { useEffect, useMemo, useRef, useState } from 'react'
import { consoleExecute } from '../api'

interface ConsoleEntry {
  command: string
  output: string
  isError: boolean
}

/** Command names out of the node's own `help` text: one command per line,
 * first token; the `== Category ==` headers are skipped. The node is the
 * authority on what exists, so the completion list is never hardcoded. */
function parseHelp(help: unknown): string[] {
  if (typeof help !== 'string') return []
  const names: string[] = []
  for (const line of help.split('\n')) {
    const trimmed = line.trim()
    if (!trimmed || trimmed.startsWith('==')) continue
    const name = trimmed.split(/\s/)[0]
    if (name && /^[a-z][a-z0-9_]*$/.test(name) && !names.includes(name)) names.push(name)
  }
  return names
}

/**
 * The debug console — the Qt wallet's console in the modern shell, including
 * its completion: typing filters the node's command list, matches appear
 * above the input line, a click inserts one, Tab takes the first.
 */
export function ConsoleOverlay({ open, onClose }: { open: boolean; onClose: () => void }) {
  const [entries, setEntries] = useState<ConsoleEntry[]>([])
  const [input, setInput] = useState('')
  const [busy, setBusy] = useState(false)
  const [historyIndex, setHistoryIndex] = useState<number | null>(null)
  const [commands, setCommands] = useState<string[]>([])
  const scrollRef = useRef<HTMLDivElement>(null)
  const inputRef = useRef<HTMLInputElement>(null)

  useEffect(() => {
    if (open) inputRef.current?.focus()
  }, [open])

  useEffect(() => {
    if (!open || commands.length > 0) return
    void consoleExecute('help')
      .then((help) => setCommands(parseHelp(help)))
      .catch(() => setCommands([])) // no completion without a node; the console still works
  }, [open, commands.length])

  useEffect(() => {
    scrollRef.current?.scrollTo({ top: scrollRef.current.scrollHeight })
  }, [entries])

  // Complete only the command word itself: once a space is typed, the
  // arguments are the user's business.
  const suggestions = useMemo(() => {
    const word = input.trimStart()
    if (!word || word.includes(' ')) return []
    const matches = commands.filter((name) => name.startsWith(word.toLowerCase()))
    return matches[0] === word && matches.length === 1 ? [] : matches.slice(0, 18)
  }, [input, commands])

  if (!open) return null

  const run = async () => {
    const command = input.trim()
    if (!command || busy) return
    setBusy(true)
    setInput('')
    setHistoryIndex(null)
    try {
      const result = await consoleExecute(command)
      const output = typeof result === 'string' ? result : JSON.stringify(result, null, 2) ?? 'null'
      setEntries((list) => [...list, { command, output, isError: false }])
    } catch (e) {
      setEntries((list) => [...list, { command, output: e instanceof Error ? e.message : String(e), isError: true }])
    } finally {
      setBusy(false)
      inputRef.current?.focus()
    }
  }

  const recall = (direction: -1 | 1) => {
    const history = entries.map((entry) => entry.command)
    if (history.length === 0) return
    const next =
      historyIndex === null
        ? direction === -1
          ? history.length - 1
          : null
        : Math.max(0, Math.min(history.length - 1, historyIndex + (direction === -1 ? -1 : 1)))
    if (next === null || (historyIndex !== null && direction === 1 && historyIndex === history.length - 1)) {
      setHistoryIndex(null)
      setInput('')
      return
    }
    setHistoryIndex(next)
    setInput(history[next])
  }

  const complete = (name: string) => {
    setInput(name + ' ')
    inputRef.current?.focus()
  }

  return (
    <div className="command-backdrop" onMouseDown={onClose}>
      <section className="command-palette console-panel" onMouseDown={(event) => event.stopPropagation()}>
        <div className="console-head">
          <span className="command-icon"><Terminal size={17} /></span>
          <div><strong>RPC console</strong><small>runs against the connected Core node — a mistyped spending command spends real coins</small></div>
          <button className="icon-button" onClick={onClose} aria-label="Close console"><X size={16} /></button>
        </div>
        <div className="console-scroll" ref={scrollRef}>
          {entries.length === 0 ? (
            <p className="table-note">Type an RPC command — for example <code>getblockchaininfo</code>, <code>getstakinginfo</code>, <code>help</code>. Typing suggests commands; Tab completes the first.</p>
          ) : null}
          {entries.map((entry, index) => (
            <div className="console-entry" key={index}>
              <div className="console-cmd">&gt; {entry.command}</div>
              <pre className={entry.isError ? 'console-err' : ''}>{entry.output}</pre>
            </div>
          ))}
        </div>
        {suggestions.length > 0 ? (
          <div className="console-suggest" role="listbox">
            {suggestions.map((name) => (
              <button key={name} onClick={() => complete(name)}>{name}</button>
            ))}
          </div>
        ) : null}
        <div className="console-input">
          <span>&gt;</span>
          <input
            ref={inputRef}
            value={input}
            disabled={busy}
            spellCheck={false}
            placeholder={busy ? 'running…' : 'RPC command'}
            onChange={(event) => setInput(event.target.value)}
            onKeyDown={(event) => {
              if (event.key === 'Enter') void run()
              if (event.key === 'ArrowUp') { event.preventDefault(); recall(-1) }
              if (event.key === 'ArrowDown') { event.preventDefault(); recall(1) }
              if (event.key === 'Tab' && suggestions.length > 0) {
                event.preventDefault()
                complete(suggestions[0])
              }
            }}
          />
          <button className="button secondary console-clear" onClick={() => { setEntries([]); inputRef.current?.focus() }} disabled={entries.length === 0}>Clear</button>
          <button className="button primary console-run" onClick={() => void run()} disabled={busy || input.trim().length === 0}>Enter</button>
        </div>
      </section>
    </div>
  )
}
