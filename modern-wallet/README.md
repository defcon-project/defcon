# DeFCoN Modern Wallet

A React + Tauri wallet over the unchanged DeFCoN Core engine, living beside the
classic Qt wallet rather than replacing it.

```text
DeFCoN Launcher  (defcon-launcher)
      │  reads gui.conf in the datadir root
      │
      ├── guimode=classic ──► defcon-qt          (built-in Core + wallet, untouched)
      │
      └── guimode=modern ──► defcon-wallet      (this directory)
                               ├── React UI            src/
                               ├── Tauri Rust bridge   src-tauri/  +  bridge-core/
                               └── defcond + wallet    (spawned or attached to)
```

Both interfaces use the same Core implementation, the same chain data and the
same wallet files. They cannot hold the same datadir at once — defcond's own
datadir lock enforces that, and this wallet reports the conflict in plain
words instead of fighting it.

## Layout

| Path | What |
|---|---|
| `bridge-core/` | Pure Rust: `gui.conf` contract, datadir/chain resolution, cookie-auth JSON-RPC client, defcond lifecycle. No GUI dependency; tests run anywhere with plain `cargo test`. |
| `launcher/` | `defcon-launcher`: reads `gui.conf`, starts `defcon-qt` or `defcon-wallet`, forwards every other argument untouched. Missing or invalid preference means classic. |
| `src-tauri/` | The Tauri shell: every Core interaction as a typed command over `bridge-core`. The webview never sees RPC credentials, passphrases echoed back, or key material. |
| `src/` | The React interface. All outside contact goes through `src/api.ts`; in a plain browser (`npm run dev` without Tauri) it renders demo data and says so. |

## gui.conf

A separate file in the datadir root — `%APPDATA%\Defcon\gui.conf` on Windows,
`~/.defcon/gui.conf` elsewhere — deliberately not `defcon.conf`, which the
daemon would reject for carrying an unknown option:

```text
guimode=classic
guimode=modern
```

Values are the two readable words, never numbers. Unknown or missing values
fall back to classic. Everything else in the file survives a rewrite.
The launcher also accepts a `--guimode=` override on its command line, which
wins over the file and is not forwarded.

## Building

Rust side (any platform, no GUI toolchain needed):

```sh
cd modern-wallet
cargo test          # bridge-core + launcher (the Tauri shell is not a default member)
cargo build --release -p defcon-launcher
```

Frontend type-check and bundle:

```sh
npm install
npm run build       # tsc --noEmit && vite build
```

The desktop app (needs the Tauri prerequisites — WebView2 on Windows,
webkit2gtk on Linux):

```sh
npm run tauri dev     # development window
npm run tauri build   # installers; also builds defcond-wallet in release
```

Browser-only preview with demo data (no node, no funds):

```sh
npm run dev           # http://127.0.0.1:4180/ — shows a DEMO badge
```

## Security model

- The React layer talks only to the Tauri commands (`src/api.ts` is the single
  entry point) and receives business results: balances, lists, a txid.
- The Rust bridge reads the node's `.cookie` itself and keeps it; nothing
  secret crosses the webview boundary in either direction, except the unlock
  passphrase travelling inward, which is forwarded to the node and dropped.
- Signing happens in the Core wallet process, exactly as with the Qt wallet.
- Send is confirm-first: validate address, show an explicit confirmation with
  amount and destination, only then `sendtoaddress`.
- CSP allows only the app itself and the Tauri IPC origin.

## Testing

- `cargo test` — 25 unit tests: gui.conf parsing and rewrite, datadir/port
  constants checked against the C++ source, RPC client behaviour against a
  local mock HTTP server (auth header, error mapping, wallet routing), defcond
  argument building, launcher decision table.
- `npm run build` — strict TypeScript over the whole UI.
- Live smoke: start the app against a regtest or devnet node and walk the
  views; the connect screen reports a locked datadir (classic wallet open) as
  exactly that.
