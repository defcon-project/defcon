# src/node/

The [`src/node/`](./) directory contains code that needs to access node state
(state in `CChain`, `CBlockIndex`, `CCoinsView`, `CTxMemPool`, and similar
classes).

Code in [`src/node/`](./) is meant to be segregated from code in
[`src/wallet/`](../wallet/) and [`src/qt/`](../qt/), to ensure wallet and GUI
code changes don't interfere with node operation, to allow wallet and GUI code
to run in separate processes, and to perhaps eventually allow wallet and GUI
code to be maintained in separate source repositories.

As a rule of thumb, code in one of the [`src/node/`](./),
[`src/wallet/`](../wallet/), or [`src/qt/`](../qt/) directories should avoid
calling code in the other directories directly, and only invoke it indirectly
through the more limited [`src/interfaces/`](../interfaces/) classes.

Examples include [block storage](blockstorage.cpp) and [block assembly](miner.cpp).
