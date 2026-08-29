// Live smoke test against a real defcond, exercising the exact call path the
// wallet uses: spawn -> cookie -> RPC warmup -> wallet calls -> graceful stop.
//
//   cargo run --example smoke -- <path-to-defcond> <scratch-datadir>
//
// Runs on regtest in a throwaway datadir, so it touches no real chain and no
// real wallet. Exits non-zero on the first failed step.

use std::path::PathBuf;
use std::time::Duration;

use defcon_bridge_core::{build_defcond_args, node, Chain, Datadir, NodeHandle, RpcClient};
use serde_json::json;

fn main() {
    let mut args = std::env::args().skip(1);
    let defcond = PathBuf::from(args.next().expect("usage: smoke <defcond> <datadir>"));
    let root = PathBuf::from(args.next().expect("usage: smoke <defcond> <datadir>"));

    std::fs::create_dir_all(&root).expect("create scratch datadir");
    let datadir = Datadir::new(root, Chain::Regtest);
    println!("[1] spawning {} {:?}", defcond.display(), build_defcond_args(&datadir, &Datadir::default_root()));
    let mut handle = NodeHandle::spawn(&defcond, &datadir).expect("spawn defcond");

    let port = datadir.chain.rpc_port();
    let cookie = datadir.cookie_path();
    println!("[2] waiting for RPC on port {port} (cookie {})", cookie.display());
    let client = node::wait_for_rpc(&|| RpcClient::from_cookie(port, &cookie), Duration::from_secs(60))
        .expect("node did not become ready");

    let info = client.call("getblockchaininfo", json!([])).expect("getblockchaininfo");
    println!("[3] chain={} blocks={}", info["chain"], info["blocks"]);
    assert_eq!(info["chain"], "regtest");

    println!("[4] creating a descriptor wallet");
    match client.call("createwallet", json!(["smoke", false, false, "", false, true, true])) {
        Ok(_) => {}
        Err(e) => {
            // A rerun on the same scratch dir finds it on disk; loading is fine.
            println!("    createwallet said: {e}; trying loadwallet");
            let _ = client.call("loadwallet", json!(["smoke"]));
        }
    }

    let address = client
        .call_wallet(Some("smoke"), "getnewaddress", json!(["smoke-label"]))
        .expect("getnewaddress");
    println!("[5] new address: {address}");
    assert!(address.as_str().map(|s| !s.is_empty()).unwrap_or(false));

    let valid = client
        .call("validateaddress", json!([address]))
        .expect("validateaddress");
    assert_eq!(valid["isvalid"], true, "own address must validate");

    let balances = client
        .call_wallet(Some("smoke"), "getbalances", json!([]))
        .expect("getbalances");
    println!("[6] balances: trusted={}", balances["mine"]["trusted"]);

    let txs = client
        .call_wallet(Some("smoke"), "listtransactions", json!(["*", 10, 0]))
        .expect("listtransactions");
    assert!(txs.is_array());
    println!("[7] listtransactions: {} entries", txs.as_array().unwrap().len());

    println!("[8] graceful stop");
    let clean = handle.stop(&client, Duration::from_secs(30));
    assert!(clean, "node had to be killed instead of stopping");
    println!("SMOKE OK");
}
