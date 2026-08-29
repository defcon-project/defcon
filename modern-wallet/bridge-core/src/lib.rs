// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! The platform-independent core of the DeFCoN modern wallet.
//!
//! Everything the Tauri shell and the launcher share lives here, with no GUI
//! toolkit dependency of any kind: the `gui.conf` launcher contract, default
//! datadir resolution, cookie authentication, a minimal JSON-RPC client, and
//! the defcond process lifecycle. The React layer sits on the far side of the
//! Tauri commands and never sees anything in this crate directly -- in
//! particular it never receives RPC credentials or key material.

pub mod datadir;
pub mod guiconf;
pub mod node;
pub mod rpc;

pub use datadir::{Chain, Datadir};
pub use guiconf::{GuiConf, GuiMode};
pub use node::{build_defcond_args, NodeHandle};
pub use rpc::{RpcClient, RpcError};
