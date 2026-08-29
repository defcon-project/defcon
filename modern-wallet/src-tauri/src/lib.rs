// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! The Tauri shell: every Core interaction the React UI can perform, as typed
//! commands over `defcon-bridge-core`.
//!
//! The security boundary runs exactly here. The webview receives business
//! results -- balances, transaction lists, a txid -- and never the RPC cookie,
//! a passphrase echo, or key material. The one command that carries a secret
//! inward (`wallet_unlock`) forwards it straight to the node and drops it.

use std::path::PathBuf;
use std::sync::Mutex;
use std::time::Duration;

use serde::Serialize;
use serde_json::{json, Value};
use tauri::State;

use defcon_bridge_core::{
    build_defcond_args, guiconf, node, Chain, Datadir, GuiMode, NodeHandle, RpcClient, RpcError,
};

/// Everything mutable the commands share.
struct AppState {
    datadir: Datadir,
    client: Option<RpcClient>,
    /// Present only when this wallet started defcond itself; a node that was
    /// already running is never stopped by us.
    managed_node: Option<NodeHandle>,
    wallet_name: Option<String>,
}

impl AppState {
    fn client(&self) -> Result<&RpcClient, String> {
        self.client.as_ref().ok_or_else(|| "not connected to Core".to_owned())
    }
}

type Shared<'a> = State<'a, Mutex<AppState>>;

fn err<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

/// Wallet-endpoint call with the active wallet name.
fn wcall(state: &AppState, method: &str, params: Value) -> Result<Value, String> {
    state
        .client()?
        .call_wallet(state.wallet_name.as_deref(), method, params)
        .map_err(err)
}

fn ncall(state: &AppState, method: &str, params: Value) -> Result<Value, String> {
    state.client()?.call(method, params).map_err(err)
}

// ---------------------------------------------------------------- connection

#[derive(Serialize)]
struct ConnectionInfo {
    connected: bool,
    chain: String,
    datadir: String,
    managed: bool,
    wallet: Option<String>,
}

fn connection_info(state: &AppState) -> ConnectionInfo {
    ConnectionInfo {
        connected: state.client.is_some(),
        chain: match &state.datadir.chain {
            Chain::Main => "main".to_owned(),
            Chain::Testnet => "testnet".to_owned(),
            Chain::Devnet(name) => format!("devnet-{name}"),
            Chain::Regtest => "regtest".to_owned(),
        },
        datadir: state.datadir.root.display().to_string(),
        managed: state.managed_node.is_some(),
        wallet: state.wallet_name.clone(),
    }
}

#[tauri::command]
fn get_connection(state: Shared) -> ConnectionInfo {
    connection_info(&state.lock().unwrap())
}

/// Selects the chain (and datadir root) the wallet talks to. Disconnects; the
/// UI calls `connect` afterwards.
#[tauri::command]
fn set_chain(state: Shared, chain: String, devnet_name: Option<String>, datadir: Option<String>) -> Result<ConnectionInfo, String> {
    let chain = match chain.as_str() {
        "main" => Chain::Main,
        "testnet" => Chain::Testnet,
        "regtest" => Chain::Regtest,
        "devnet" => Chain::Devnet(devnet_name.ok_or("devnet needs a name")?),
        other => return Err(format!("unknown chain: {other}")),
    };
    let mut s = state.lock().unwrap();
    let root = match datadir {
        Some(d) if !d.trim().is_empty() => PathBuf::from(d),
        _ => Datadir::default_root(),
    };
    s.datadir = Datadir::new(root, chain);
    s.client = None;
    Ok(connection_info(&s))
}

/// Connects to a running node via its cookie. Never starts anything.
#[tauri::command]
fn connect(state: Shared) -> Result<ConnectionInfo, String> {
    let mut s = state.lock().unwrap();
    let client = RpcClient::from_cookie(s.datadir.chain.rpc_port(), &s.datadir.cookie_path())
        .map_err(err)?;
    client.call("getblockchaininfo", json!([])).map_err(err)?;
    s.client = Some(client);
    drop(s);
    pick_default_wallet(&state);
    Ok(connection_info(&state.lock().unwrap()))
}

/// Starts defcond for the selected datadir and waits for RPC. Refuses cleanly
/// when the datadir is already locked -- that is the "Qt wallet is open" case.
#[tauri::command]
fn start_node(state: Shared, defcond_path: Option<String>) -> Result<ConnectionInfo, String> {
    let mut s = state.lock().unwrap();
    if s.client.is_some() {
        return Ok(connection_info(&s));
    }
    let path = match defcond_path {
        Some(p) if !p.trim().is_empty() => PathBuf::from(p),
        _ => node::locate_binary("defcond", std::env::current_exe().ok().as_deref()),
    };
    let handle = NodeHandle::spawn(&path, &s.datadir).map_err(|e| {
        format!("could not start {} ({e}); is it installed next to the wallet?", path.display())
    })?;
    s.managed_node = Some(handle);

    let port = s.datadir.chain.rpc_port();
    let cookie = s.datadir.cookie_path();
    drop(s); // do not hold the lock across the warmup wait

    let client = node::wait_for_rpc(
        &|| RpcClient::from_cookie(port, &cookie),
        Duration::from_secs(90),
    )
    .map_err(|last| {
        format!("node did not become ready: {last} (if another wallet holds this datadir, close it first)")
    })?;

    let mut s = state.lock().unwrap();
    s.client = Some(client);
    drop(s);
    pick_default_wallet(&state);
    Ok(connection_info(&state.lock().unwrap()))
}

/// Stops a node this wallet started. A node we merely connected to is left
/// alone unless `force` says otherwise.
#[tauri::command]
fn stop_node(state: Shared, force: bool) -> Result<ConnectionInfo, String> {
    let mut s = state.lock().unwrap();
    match (s.managed_node.take(), s.client.take()) {
        (Some(mut handle), Some(client)) => {
            handle.stop(&client, Duration::from_secs(60));
        }
        (None, Some(client)) if force => {
            let _ = client.call("stop", json!([]));
        }
        _ => {}
    }
    Ok(connection_info(&s))
}

/// Loads the first wallet the node reports, so a single-wallet setup works
/// with zero configuration. Multiwallet users pick explicitly in Settings.
fn pick_default_wallet(state: &Shared) {
    let mut s = state.lock().unwrap();
    if s.wallet_name.is_some() {
        return;
    }
    if let Ok(Value::Array(list)) = ncall(&s, "listwallets", json!([])) {
        if let Some(Value::String(first)) = list.first() {
            s.wallet_name = Some(first.clone());
        }
    }
}

#[tauri::command]
fn list_wallets(state: Shared) -> Result<Value, String> {
    ncall(&state.lock().unwrap(), "listwallets", json!([]))
}

#[tauri::command]
fn select_wallet(state: Shared, name: String) -> Result<(), String> {
    let mut s = state.lock().unwrap();
    // Load it if the node has it on disk but not open; ignore "already loaded".
    match s.client()?.call("loadwallet", json!([name])) {
        Ok(_) => {}
        Err(RpcError::Node { code: -35, .. }) => {} // already loaded
        Err(RpcError::Node { code, message }) if code == -18 => {
            return Err(format!("wallet not found: {message}"));
        }
        Err(e) => return Err(e.to_string()),
    }
    s.wallet_name = Some(name);
    Ok(())
}

// ---------------------------------------------------------------- read views

#[tauri::command]
fn node_status(state: Shared) -> Result<Value, String> {
    let s = state.lock().unwrap();
    let blockchain = ncall(&s, "getblockchaininfo", json!([]))?;
    let network = ncall(&s, "getnetworkinfo", json!([]))?;
    // Best ChainLock is informative, not vital: absence is null, not an error.
    let chainlock = ncall(&s, "getbestchainlock", json!([])).unwrap_or(Value::Null);
    Ok(json!({ "blockchain": blockchain, "network": network, "chainlock": chainlock }))
}

#[tauri::command]
fn wallet_summary(state: Shared) -> Result<Value, String> {
    let s = state.lock().unwrap();
    let balances = wcall(&s, "getbalances", json!([]))?;
    let info = wcall(&s, "getwalletinfo", json!([]))?;
    let staking = ncall(&s, "getstakinginfo", json!([])).unwrap_or(Value::Null);
    Ok(json!({ "balances": balances, "info": info, "staking": staking }))
}

#[tauri::command]
fn list_transactions(state: Shared, count: u32, skip: u32) -> Result<Value, String> {
    let s = state.lock().unwrap();
    wcall(&s, "listtransactions", json!(["*", count, skip]))
}

#[tauri::command]
fn masternode_list(state: Shared) -> Result<Value, String> {
    let s = state.lock().unwrap();
    ncall(&s, "masternodelist", json!(["json"]))
}

#[tauri::command]
fn governance_list(state: Shared) -> Result<Value, String> {
    let s = state.lock().unwrap();
    ncall(&s, "gobject", json!(["list", "valid", "proposals"]))
}

#[tauri::command]
fn coinjoin_info(state: Shared) -> Result<Value, String> {
    let s = state.lock().unwrap();
    wcall(&s, "getcoinjoininfo", json!([]))
}

// ---------------------------------------------------------------- receive

#[tauri::command]
fn get_new_address(state: Shared, label: String) -> Result<Value, String> {
    let s = state.lock().unwrap();
    wcall(&s, "getnewaddress", json!([label]))
}

#[tauri::command]
fn list_receive_addresses(state: Shared) -> Result<Value, String> {
    let s = state.lock().unwrap();
    wcall(&s, "listreceivedbyaddress", json!([0, true]))
}

// ---------------------------------------------------------------- send

#[tauri::command]
fn validate_address(state: Shared, address: String) -> Result<Value, String> {
    let s = state.lock().unwrap();
    ncall(&s, "validateaddress", json!([address]))
}

/// The one spending command. Amount travels as a string and is parsed by the
/// node, so no float rounding happens on our side of the boundary.
#[tauri::command]
fn send_to_address(
    state: Shared,
    address: String,
    amount: String,
    label: Option<String>,
) -> Result<Value, String> {
    let s = state.lock().unwrap();
    let valid = ncall(&s, "validateaddress", json!([address]))?;
    if valid.get("isvalid") != Some(&Value::Bool(true)) {
        return Err("invalid address".to_owned());
    }
    let amount: Value = serde_json::from_str(&amount).map_err(|_| "invalid amount".to_owned())?;
    if !amount.is_number() {
        return Err("invalid amount".to_owned());
    }
    wcall(
        &s,
        "sendtoaddress",
        json!([address, amount, label.unwrap_or_default()]),
    )
}

// ---------------------------------------------------------------- lock state

#[tauri::command]
fn wallet_lock(state: Shared) -> Result<(), String> {
    let s = state.lock().unwrap();
    wcall(&s, "walletlock", json!([])).map(|_| ())
}

#[tauri::command]
fn wallet_unlock(state: Shared, passphrase: String, timeout_seconds: u32) -> Result<(), String> {
    let s = state.lock().unwrap();
    wcall(&s, "walletpassphrase", json!([passphrase, timeout_seconds])).map(|_| ())
}

// ---------------------------------------------------------------- gui.conf

#[tauri::command]
fn gui_get_mode(state: Shared) -> String {
    let s = state.lock().unwrap();
    guiconf::read_mode(&s.datadir.gui_conf_path())
        .unwrap_or(GuiMode::Classic)
        .as_str()
        .to_owned()
}

#[tauri::command]
fn gui_set_mode(state: Shared, mode: String) -> Result<String, String> {
    let mode = GuiMode::parse(&mode).ok_or("guimode must be classic or modern")?;
    let s = state.lock().unwrap();
    guiconf::write_mode(&s.datadir.gui_conf_path(), mode).map_err(err)?;
    Ok(mode.as_str().to_owned())
}

/// The exact command line a managed defcond would get -- surfaced in Settings
/// so what the wallet does is inspectable, never a mystery.
#[tauri::command]
fn preview_defcond_args(state: Shared) -> Vec<String> {
    let s = state.lock().unwrap();
    build_defcond_args(&s.datadir, &Datadir::default_root())
}

// ---------------------------------------------------------------- entry

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_clipboard_manager::init())
        .manage(Mutex::new(AppState {
            datadir: Datadir::new(Datadir::default_root(), Chain::Main),
            client: None,
            managed_node: None,
            wallet_name: None,
        }))
        .invoke_handler(tauri::generate_handler![
            get_connection,
            set_chain,
            connect,
            start_node,
            stop_node,
            list_wallets,
            select_wallet,
            node_status,
            wallet_summary,
            list_transactions,
            masternode_list,
            governance_list,
            coinjoin_info,
            get_new_address,
            list_receive_addresses,
            validate_address,
            send_to_address,
            wallet_lock,
            wallet_unlock,
            gui_get_mode,
            gui_set_mode,
            preview_defcond_args,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the DeFCoN wallet");
}
