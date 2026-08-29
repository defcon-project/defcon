// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! A minimal JSON-RPC 1.0 client for defcond, over 127.0.0.1 only.
//!
//! Authentication is the node's own cookie: the bridge reads
//! `<network dir>/.cookie` (written fresh on every daemon start) and sends it
//! as HTTP basic auth. The credential never leaves this process -- the React
//! layer sees Tauri command results, not this client.

use std::fmt;
use std::fs;
use std::path::Path;
use std::time::Duration;

use base64::Engine as _;
use serde_json::{json, Value};

/// Errors, separated by what the caller can do about them.
#[derive(Debug)]
pub enum RpcError {
    /// The node is not reachable at all (not running, or still starting).
    Transport(String),
    /// Reached the node but was refused (bad or stale cookie).
    Auth,
    /// HTTP-level failure that is not auth.
    Http(u16, String),
    /// The node answered with a JSON-RPC error object.
    Node { code: i64, message: String },
    /// The response was not the JSON shape a node produces.
    Protocol(String),
}

impl fmt::Display for RpcError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            RpcError::Transport(e) => write!(f, "node unreachable: {e}"),
            RpcError::Auth => write!(f, "RPC authentication failed (stale cookie?)"),
            RpcError::Http(code, body) => write!(f, "HTTP {code}: {body}"),
            RpcError::Node { code, message } => write!(f, "node error {code}: {message}"),
            RpcError::Protocol(e) => write!(f, "malformed RPC response: {e}"),
        }
    }
}

impl std::error::Error for RpcError {}

/// One authenticated connection target. Cheap to clone.
#[derive(Clone)]
pub struct RpcClient {
    url: String,
    auth_header: String,
    agent: ureq::Agent,
}

impl RpcClient {
    /// A client for 127.0.0.1:`port` with `user:pass` credentials.
    pub fn new(port: u16, credentials: &str) -> RpcClient {
        let auth = base64::engine::general_purpose::STANDARD.encode(credentials);
        RpcClient {
            url: format!("http://127.0.0.1:{port}/"),
            auth_header: format!("Basic {auth}"),
            agent: ureq::AgentBuilder::new()
                .timeout_connect(Duration::from_secs(3))
                .timeout(Duration::from_secs(120)) // rescans and long calls
                .build(),
        }
    }

    /// A client authenticated from the node's cookie file.
    pub fn from_cookie(port: u16, cookie_path: &Path) -> Result<RpcClient, RpcError> {
        let cookie = fs::read_to_string(cookie_path)
            .map_err(|e| RpcError::Transport(format!("cookie unreadable: {e}")))?;
        let cookie = cookie.trim();
        if !cookie.contains(':') {
            return Err(RpcError::Protocol("cookie is not user:pass".to_owned()));
        }
        Ok(RpcClient::new(port, cookie))
    }

    /// Calls `method` with `params`, optionally against a named wallet
    /// endpoint (multiwallet nodes route by URL).
    pub fn call_wallet(
        &self,
        wallet: Option<&str>,
        method: &str,
        params: Value,
    ) -> Result<Value, RpcError> {
        let url = match wallet {
            Some(name) => format!("{}wallet/{}", self.url, name),
            None => self.url.clone(),
        };
        let body = json!({ "jsonrpc": "1.0", "id": "defcon-wallet", "method": method, "params": params });

        let response = self
            .agent
            .post(&url)
            .set("Authorization", &self.auth_header)
            .set("Content-Type", "application/json")
            .send_string(&body.to_string());

        let (status, text) = match response {
            Ok(resp) => {
                let status = resp.status();
                let text = resp
                    .into_string()
                    .map_err(|e| RpcError::Protocol(e.to_string()))?;
                (status, text)
            }
            // The node answers RPC errors with non-2xx statuses and a JSON
            // body; ureq reports those as Status errors carrying the response.
            Err(ureq::Error::Status(code, resp)) => {
                let text = resp.into_string().unwrap_or_default();
                (code, text)
            }
            Err(ureq::Error::Transport(t)) => return Err(RpcError::Transport(t.to_string())),
        };

        if status == 401 || status == 403 {
            return Err(RpcError::Auth);
        }

        let parsed: Value = serde_json::from_str(&text).map_err(|_| {
            if status != 200 {
                RpcError::Http(status, truncate(&text, 200))
            } else {
                RpcError::Protocol(truncate(&text, 200))
            }
        })?;

        if let Some(err) = parsed.get("error").filter(|e| !e.is_null()) {
            return Err(RpcError::Node {
                code: err.get("code").and_then(Value::as_i64).unwrap_or(0),
                message: err
                    .get("message")
                    .and_then(Value::as_str)
                    .unwrap_or("unknown")
                    .to_owned(),
            });
        }
        parsed
            .get("result")
            .cloned()
            .ok_or_else(|| RpcError::Protocol("no result field".to_owned()))
    }

    /// Node-level call (no wallet routing).
    pub fn call(&self, method: &str, params: Value) -> Result<Value, RpcError> {
        self.call_wallet(None, method, params)
    }
}

fn truncate(s: &str, at: usize) -> String {
    if s.len() <= at {
        s.to_owned()
    } else {
        format!("{}…", &s[..at])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader, Read, Write};
    use std::net::TcpListener;
    use std::thread;

    /// A one-shot HTTP server: answers every request on one connection with
    /// the canned status and body, and hands back what it received.
    fn serve_once(status: u16, body: &'static str) -> (u16, thread::JoinHandle<String>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let handle = thread::spawn(move || {
            let (stream, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(stream);
            let mut request = String::new();
            let mut content_length = 0usize;
            loop {
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                if let Some(rest) = line.to_ascii_lowercase().strip_prefix("content-length:") {
                    content_length = rest.trim().parse().unwrap();
                }
                request.push_str(&line);
                if line == "\r\n" {
                    break;
                }
            }
            let mut payload = vec![0u8; content_length];
            reader.read_exact(&mut payload).unwrap();
            request.push_str(&String::from_utf8_lossy(&payload));

            let mut stream = reader.into_inner();
            let reply = format!(
                "HTTP/1.1 {status} X\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                body.len()
            );
            stream.write_all(reply.as_bytes()).unwrap();
            request
        });
        (port, handle)
    }

    #[test]
    fn success_result_is_unwrapped_and_auth_header_sent() {
        let (port, server) = serve_once(200, r#"{"result":{"blocks":42},"error":null,"id":"defcon-wallet"}"#);
        let client = RpcClient::new(port, "user:pass");
        let result = client.call("getblockchaininfo", json!([])).unwrap();
        assert_eq!(result["blocks"], 42);

        let request = server.join().unwrap();
        // base64("user:pass")
        assert!(request.contains("Authorization: Basic dXNlcjpwYXNz"));
        assert!(request.contains(r#""method":"getblockchaininfo""#));
    }

    #[test]
    fn node_error_maps_to_rpc_error_even_on_http_500() {
        let (port, _server) =
            serve_once(500, r#"{"result":null,"error":{"code":-18,"message":"Wallet not loaded"},"id":"x"}"#);
        let client = RpcClient::new(port, "u:p");
        match client.call("getbalances", json!([])) {
            Err(RpcError::Node { code, message }) => {
                assert_eq!(code, -18);
                assert!(message.contains("Wallet not loaded"));
            }
            other => panic!("expected node error, got {other:?}"),
        }
    }

    #[test]
    fn unauthorized_maps_to_auth() {
        let (port, _server) = serve_once(401, "");
        let client = RpcClient::new(port, "u:p");
        match client.call("getblockcount", json!([])) {
            Err(RpcError::Auth) => {}
            other => panic!("expected auth error, got {other:?}"),
        }
    }

    #[test]
    fn unreachable_maps_to_transport() {
        // A port nothing listens on: bind, learn the port, drop the listener.
        let port = {
            let l = TcpListener::bind("127.0.0.1:0").unwrap();
            l.local_addr().unwrap().port()
        };
        let client = RpcClient::new(port, "u:p");
        match client.call("ping", json!([])) {
            Err(RpcError::Transport(_)) => {}
            other => panic!("expected transport error, got {other:?}"),
        }
    }

    #[test]
    fn wallet_routing_hits_the_wallet_endpoint() {
        let (port, server) = serve_once(200, r#"{"result":true,"error":null,"id":"x"}"#);
        let client = RpcClient::new(port, "u:p");
        client.call_wallet(Some("First"), "getbalances", json!([])).unwrap();
        let request = server.join().unwrap();
        assert!(request.starts_with("POST /wallet/First "), "got: {request}");
    }

    #[test]
    fn cookie_client_reads_user_pass() {
        let dir = tempfile::tempdir().unwrap();
        let cookie = dir.path().join(".cookie");
        std::fs::write(&cookie, "__cookie__:abcdef0123456789\n").unwrap();
        assert!(RpcClient::from_cookie(1, &cookie).is_ok());

        std::fs::write(&cookie, "no-colon-here").unwrap();
        assert!(matches!(RpcClient::from_cookie(1, &cookie), Err(RpcError::Protocol(_))));

        assert!(matches!(
            RpcClient::from_cookie(1, &dir.path().join("missing")),
            Err(RpcError::Transport(_))
        ));
    }
}
