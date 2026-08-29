# The modern wallet and the launcher

DeFCoN ships two interfaces over one Core engine:

```text
DeFCoN Launcher  (defcon-launcher)
      │  reads gui.conf from the datadir root
      ├── guimode=classic ──► defcon-qt       (the Qt wallet, unchanged)
      └── guimode=modern  ──► defcon-wallet   (React + Tauri)
```

- The preference file is `gui.conf` in the datadir root
  (`%APPDATA%\Defcon\gui.conf` on Windows, `~/.defcon/gui.conf` elsewhere),
  holding the readable line `guimode=classic` or `guimode=modern`. It is a
  separate file because defcond rejects unknown options in `defcon.conf`.
- A missing or invalid preference opens the classic wallet.
- `defcon-launcher --guimode=modern` overrides the file for one start.
- Both wallets use the same chain data and wallet files; defcond's datadir
  lock guarantees only one holds a datadir at a time.

Everything else — architecture, build steps, the security model, and the test
suite — is documented in [`modern-wallet/README.md`](../modern-wallet/README.md).
