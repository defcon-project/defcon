// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! defcond lifecycle: find it, start it, wait for RPC, stop it cleanly.
//!
//! One rule shapes everything here: the modern wallet and the Qt wallet share
//! one datadir but must never hold it at once. defcond enforces that itself
//! with the datadir lock, so this module does not second-guess it -- a failed
//! start with "cannot obtain a lock" is reported as exactly that, and the UI
//! tells the user the other wallet is still open.

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use crate::datadir::Datadir;
use crate::rpc::{RpcClient, RpcError};

/// Arguments for a wallet-managed defcond. Pure so it can be tested without
/// spawning anything: the daemon is started with `-server` (RPC on), no
/// `-daemon` (the child stays attached so we can supervise it), and the chain
/// selector plus datadir the resolved `Datadir` implies.
pub fn build_defcond_args(datadir: &Datadir, default_root: &Path) -> Vec<String> {
    let mut args = Vec::new();
    if datadir.root != default_root {
        args.push(format!("-datadir={}", datadir.root.display()));
    }
    if let Some(chain_arg) = datadir.chain.defcond_arg() {
        args.push(chain_arg);
    }
    args.push("-server=1".to_owned());
    // The wallet supervises the process; daemonizing would orphan it.
    args.push("-daemon=0".to_owned());
    args
}

/// A running, wallet-managed daemon.
pub struct NodeHandle {
    child: Child,
    pub started_with: Vec<String>,
}

impl NodeHandle {
    /// Spawns `defcond_path` for `datadir`. stdout/stderr go to null: the node
    /// writes its real log into debug.log in the datadir, and holding pipes
    /// we never drain would eventually block the child.
    pub fn spawn(defcond_path: &Path, datadir: &Datadir) -> std::io::Result<NodeHandle> {
        let args = build_defcond_args(datadir, &Datadir::default_root());
        let child = Command::new(defcond_path)
            .args(&args)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()?;
        Ok(NodeHandle { child, started_with: args })
    }

    /// True while the child has not exited.
    pub fn is_running(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }

    /// Asks the node to stop over RPC and waits for the process to exit.
    /// Falls back to kill only if the node ignores the request for `grace`.
    pub fn stop(&mut self, client: &RpcClient, grace: Duration) -> bool {
        let _ = client.call("stop", serde_json::json!([]));
        let deadline = Instant::now() + grace;
        while Instant::now() < deadline {
            if !self.is_running() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(200));
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
        false
    }
}

/// Where to look for a sibling Core binary: next to the current executable
/// first (the installed layout), then bare on PATH.
pub fn locate_binary(name: &str, current_exe: Option<&Path>) -> PathBuf {
    let filename = if cfg!(windows) { format!("{name}.exe") } else { name.to_owned() };
    if let Some(exe) = current_exe {
        if let Some(dir) = exe.parent() {
            let candidate = dir.join(&filename);
            if candidate.exists() {
                return candidate;
            }
        }
    }
    PathBuf::from(filename)
}

/// Polls the node until RPC answers or `timeout` passes. During warmup the
/// node answers -28 "Loading block index…": that is progress, not failure,
/// and the last such message is reported on timeout.
pub fn wait_for_rpc(
    make_client: &dyn Fn() -> Result<RpcClient, RpcError>,
    timeout: Duration,
) -> Result<RpcClient, String> {
    let deadline = Instant::now() + timeout;
    let mut last: String = "node did not answer".to_owned();
    loop {
        match make_client() {
            Ok(client) => match client.call("getblockchaininfo", serde_json::json!([])) {
                Ok(_) => return Ok(client),
                Err(RpcError::Node { code: -28, message }) => last = message, // warming up
                Err(e) => last = e.to_string(),
            },
            Err(e) => last = e.to_string(), // cookie not written yet
        }
        if Instant::now() >= deadline {
            return Err(last);
        }
        std::thread::sleep(Duration::from_millis(400));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::datadir::Chain;

    #[test]
    fn args_for_default_mainnet_are_minimal() {
        let root = Datadir::default_root();
        let d = Datadir::new(root.clone(), Chain::Main);
        assert_eq!(build_defcond_args(&d, &root), vec!["-server=1", "-daemon=0"]);
    }

    #[test]
    fn args_carry_custom_datadir_and_chain() {
        let root = PathBuf::from("/srv/defcon");
        let d = Datadir::new(root.clone(), Chain::Devnet("defcon-q60".into()));
        let args = build_defcond_args(&d, &PathBuf::from("/home/x/.defcon"));
        assert_eq!(
            args,
            vec![
                "-datadir=/srv/defcon",
                "-devnet=defcon-q60",
                "-server=1",
                "-daemon=0",
            ]
        );
    }

    #[test]
    fn regtest_arg_is_regtest() {
        let root = Datadir::default_root();
        let d = Datadir::new(root.clone(), Chain::Regtest);
        assert_eq!(
            build_defcond_args(&d, &root),
            vec!["-regtest", "-server=1", "-daemon=0"]
        );
    }

    #[test]
    fn locate_binary_prefers_sibling_then_path() {
        let dir = tempfile::tempdir().unwrap();
        let sibling = dir.path().join(if cfg!(windows) { "defcond.exe" } else { "defcond" });
        std::fs::write(&sibling, b"x").unwrap();
        let exe = dir.path().join("defcon-wallet");
        assert_eq!(locate_binary("defcond", Some(&exe)), sibling);

        // No sibling: bare name, resolved by the OS through PATH.
        let elsewhere = tempfile::tempdir().unwrap();
        let exe2 = elsewhere.path().join("defcon-wallet");
        let expected = if cfg!(windows) { "defcond.exe" } else { "defcond" };
        assert_eq!(locate_binary("defcond", Some(&exe2)), PathBuf::from(expected));
    }
}
