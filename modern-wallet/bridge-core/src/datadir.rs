// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! Datadir and network resolution, mirroring what the node itself does.
//!
//! Every constant here was read from the C++ source rather than assumed:
//! `GetDefaultDataDir` (util/system.cpp) gives `%APPDATA%\Defcon`,
//! `~/Library/Application Support/Defcon` and `~/.defcon`; the RPC ports come
//! from chainparamsbase.cpp (main 9996 P2P/8193... see below); and the devnet
//! subdirectory is `devnet-<name>`, the same string GetDevNetName builds.
//!
//! RPC ports by chain (chainparamsbase.cpp):
//! mainnet 8193, testnet 19998, devnet 19798, regtest 19898.

use std::env;
use std::path::PathBuf;

/// The chain the wallet talks to. Devnet carries its name, because both the
/// subdirectory and the `-devnet=` argument need it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Chain {
    Main,
    Testnet,
    Devnet(String),
    Regtest,
}

impl Chain {
    /// The default RPC port for the chain, from chainparamsbase.cpp.
    pub fn rpc_port(&self) -> u16 {
        match self {
            Chain::Main => 8193,
            Chain::Testnet => 19998,
            Chain::Devnet(_) => 19798,
            Chain::Regtest => 19898,
        }
    }

    /// The network subdirectory under the datadir root; empty for mainnet.
    /// Matches CBaseChainParams: "testnet3", "regtest", and for devnets the
    /// GetDevNetName convention `devnet-<name>`.
    pub fn subdir(&self) -> Option<String> {
        match self {
            Chain::Main => None,
            Chain::Testnet => Some("testnet3".to_owned()),
            Chain::Devnet(name) => Some(format!("devnet-{name}")),
            Chain::Regtest => Some("regtest".to_owned()),
        }
    }

    /// The daemon argument that selects this chain, if any.
    pub fn defcond_arg(&self) -> Option<String> {
        match self {
            Chain::Main => None,
            Chain::Testnet => Some("-testnet".to_owned()),
            Chain::Devnet(name) => Some(format!("-devnet={name}")),
            Chain::Regtest => Some("-regtest".to_owned()),
        }
    }
}

/// A resolved datadir: the root the user chose (or the platform default) plus
/// the chain, from which every derived path follows.
#[derive(Clone, Debug)]
pub struct Datadir {
    pub root: PathBuf,
    pub chain: Chain,
}

impl Datadir {
    pub fn new(root: PathBuf, chain: Chain) -> Datadir {
        Datadir { root, chain }
    }

    /// The platform default datadir root, exactly as GetDefaultDataDir builds
    /// it. Environment lookups are injected for testability.
    pub fn default_root() -> PathBuf {
        default_root_from_env(&|name| env::var(name).ok())
    }

    /// The directory the running network writes into (cookie, wallets, logs).
    pub fn network_dir(&self) -> PathBuf {
        match self.chain.subdir() {
            Some(sub) => self.root.join(sub),
            None => self.root.clone(),
        }
    }

    /// Where the node drops its RPC auth cookie.
    pub fn cookie_path(&self) -> PathBuf {
        self.network_dir().join(".cookie")
    }

    /// Where gui.conf lives: the datadir root, shared by every network, since
    /// which GUI to open is not a per-chain question.
    pub fn gui_conf_path(&self) -> PathBuf {
        self.root.join(crate::guiconf::GUI_CONF_FILENAME)
    }
}

fn default_root_from_env(get: &dyn Fn(&str) -> Option<String>) -> PathBuf {
    #[cfg(target_os = "windows")]
    {
        if let Some(appdata) = get("APPDATA") {
            return PathBuf::from(appdata).join("Defcon");
        }
        PathBuf::from("C:\\").join("Defcon")
    }
    #[cfg(target_os = "macos")]
    {
        let home = get("HOME").unwrap_or_else(|| "/".to_owned());
        PathBuf::from(home).join("Library/Application Support/Defcon")
    }
    #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
    {
        let home = get("HOME").unwrap_or_else(|| "/".to_owned());
        PathBuf::from(home).join(".defcon")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rpc_ports_match_chainparamsbase() {
        assert_eq!(Chain::Main.rpc_port(), 8193);
        assert_eq!(Chain::Testnet.rpc_port(), 19998);
        assert_eq!(Chain::Devnet("defcon-q60".into()).rpc_port(), 19798);
        assert_eq!(Chain::Regtest.rpc_port(), 19898);
    }

    #[test]
    fn network_subdirs_match_the_node() {
        assert_eq!(Chain::Main.subdir(), None);
        assert_eq!(Chain::Testnet.subdir().as_deref(), Some("testnet3"));
        assert_eq!(
            Chain::Devnet("defcon-q60".into()).subdir().as_deref(),
            Some("devnet-defcon-q60")
        );
        assert_eq!(Chain::Regtest.subdir().as_deref(), Some("regtest"));
    }

    #[test]
    fn derived_paths_follow_the_chain() {
        let d = Datadir::new(PathBuf::from("/tmp/defcon-test"), Chain::Regtest);
        assert_eq!(d.network_dir(), PathBuf::from("/tmp/defcon-test/regtest"));
        assert_eq!(d.cookie_path(), PathBuf::from("/tmp/defcon-test/regtest/.cookie"));
        assert_eq!(d.gui_conf_path(), PathBuf::from("/tmp/defcon-test/gui.conf"));

        let m = Datadir::new(PathBuf::from("/tmp/defcon-test"), Chain::Main);
        assert_eq!(m.network_dir(), PathBuf::from("/tmp/defcon-test"));
        assert_eq!(m.cookie_path(), PathBuf::from("/tmp/defcon-test/.cookie"));
    }

    #[test]
    fn default_root_uses_the_platform_convention() {
        let get = |name: &str| -> Option<String> {
            match name {
                "APPDATA" => Some("C:\\Users\\t\\AppData\\Roaming".to_owned()),
                "HOME" => Some("/home/t".to_owned()),
                _ => None,
            }
        };
        let root = default_root_from_env(&get);
        #[cfg(target_os = "windows")]
        assert!(root.ends_with("Defcon"));
        #[cfg(target_os = "macos")]
        assert_eq!(root, PathBuf::from("/home/t/Library/Application Support/Defcon"));
        #[cfg(all(not(target_os = "windows"), not(target_os = "macos")))]
        assert_eq!(root, PathBuf::from("/home/t/.defcon"));
    }
}
