// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! defcon-launcher: opens the wallet the user chose.
//!
//! ```text
//! DeFCoN Launcher
//!       │
//!       ├── guimode=classic  →  defcon-qt        (unchanged Qt wallet)
//!       └── guimode=modern   →  defcon-wallet    (React/Tauri wallet)
//! ```
//!
//! The preference lives in `gui.conf` in the datadir root -- deliberately not
//! in defcon.conf, which defcond would reject for carrying an option it does
//! not know. Everything on the launcher's command line is passed through to
//! the chosen wallet untouched, except the launcher's own `--guimode=` and
//! `--datadir=` overrides (`--datadir` is forwarded as well, since both
//! wallets understand it).
//!
//! Missing file, unreadable file, unknown value: classic. The wallet that has
//! always shipped is the one a broken preference must fall back to.

use std::path::{Path, PathBuf};
use std::process::Command;

use defcon_bridge_core::guiconf::{self, GuiMode};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let parsed = parse_args(&args);

    let datadir_root = parsed
        .datadir
        .clone()
        .unwrap_or_else(defcon_bridge_core::Datadir::default_root);
    let conf_path = datadir_root.join(guiconf::GUI_CONF_FILENAME);

    let mode = decide_mode(parsed.cli_mode, guiconf::read_mode(&conf_path));
    let binary = locate_wallet_binary(mode, std::env::current_exe().ok().as_deref());

    let status = Command::new(&binary).args(&parsed.passthrough).status();
    match status {
        Ok(status) => std::process::exit(status.code().unwrap_or(0)),
        Err(e) => {
            eprintln!(
                "defcon-launcher: could not start {} ({}): {e}",
                binary.display(),
                mode.as_str()
            );
            eprintln!(
                "defcon-launcher: check gui.conf at {} or run with --guimode=classic",
                conf_path.display()
            );
            std::process::exit(1);
        }
    }
}

/// What the launcher itself consumes from the command line.
#[derive(Debug, Default, PartialEq)]
struct ParsedArgs {
    cli_mode: Option<GuiMode>,
    datadir: Option<PathBuf>,
    /// Everything forwarded to the chosen wallet (includes --datadir, which
    /// both wallets understand; excludes --guimode, which neither does).
    passthrough: Vec<String>,
}

fn parse_args(args: &[String]) -> ParsedArgs {
    let mut parsed = ParsedArgs::default();
    for arg in args {
        if let Some(value) = arg
            .strip_prefix("--guimode=")
            .or_else(|| arg.strip_prefix("-guimode="))
        {
            parsed.cli_mode = GuiMode::parse(value);
            continue; // launcher-only: never forwarded
        }
        if let Some(value) = arg
            .strip_prefix("--datadir=")
            .or_else(|| arg.strip_prefix("-datadir="))
        {
            parsed.datadir = Some(PathBuf::from(value));
        }
        parsed.passthrough.push(arg.clone());
    }
    parsed
}

/// CLI override beats the file; a missing or invalid preference is classic.
fn decide_mode(cli: Option<GuiMode>, conf: Option<GuiMode>) -> GuiMode {
    cli.or(conf).unwrap_or(GuiMode::Classic)
}

fn wallet_binary_name(mode: GuiMode) -> &'static str {
    match mode {
        GuiMode::Classic => "defcon-qt",
        GuiMode::Modern => "defcon-wallet",
    }
}

fn locate_wallet_binary(mode: GuiMode, current_exe: Option<&Path>) -> PathBuf {
    defcon_bridge_core::node::locate_binary(wallet_binary_name(mode), current_exe)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(v: &[&str]) -> Vec<String> {
        v.iter().map(|x| x.to_string()).collect()
    }

    #[test]
    fn default_is_classic_and_conf_then_cli_take_over() {
        assert_eq!(decide_mode(None, None), GuiMode::Classic);
        assert_eq!(decide_mode(None, Some(GuiMode::Modern)), GuiMode::Modern);
        // The CLI override wins over the file, both directions.
        assert_eq!(decide_mode(Some(GuiMode::Classic), Some(GuiMode::Modern)), GuiMode::Classic);
        assert_eq!(decide_mode(Some(GuiMode::Modern), Some(GuiMode::Classic)), GuiMode::Modern);
    }

    #[test]
    fn guimode_is_consumed_and_datadir_is_forwarded() {
        let parsed = parse_args(&s(&[
            "--guimode=modern",
            "--datadir=/srv/defcon",
            "-testnet",
        ]));
        assert_eq!(parsed.cli_mode, Some(GuiMode::Modern));
        assert_eq!(parsed.datadir.as_deref(), Some(Path::new("/srv/defcon")));
        assert_eq!(parsed.passthrough, s(&["--datadir=/srv/defcon", "-testnet"]));
    }

    #[test]
    fn unknown_guimode_value_falls_back_to_conf_or_classic() {
        let parsed = parse_args(&s(&["--guimode=2"]));
        assert_eq!(parsed.cli_mode, None); // numeric values are rejected by design
        assert_eq!(decide_mode(parsed.cli_mode, None), GuiMode::Classic);
    }

    #[test]
    fn single_dash_variants_are_accepted_like_core_options() {
        let parsed = parse_args(&s(&["-guimode=modern", "-datadir=/x"]));
        assert_eq!(parsed.cli_mode, Some(GuiMode::Modern));
        assert_eq!(parsed.datadir.as_deref(), Some(Path::new("/x")));
        assert_eq!(parsed.passthrough, s(&["-datadir=/x"]));
    }

    #[test]
    fn binary_names_match_the_two_wallets() {
        assert_eq!(wallet_binary_name(GuiMode::Classic), "defcon-qt");
        assert_eq!(wallet_binary_name(GuiMode::Modern), "defcon-wallet");
    }

    #[test]
    fn end_to_end_mode_from_a_real_conf_file() {
        let dir = tempfile::tempdir().unwrap();
        let conf = dir.path().join(guiconf::GUI_CONF_FILENAME);
        guiconf::write_mode(&conf, GuiMode::Modern).unwrap();
        assert_eq!(decide_mode(None, guiconf::read_mode(&conf)), GuiMode::Modern);
        // A hand-broken value degrades to classic, never to an error.
        std::fs::write(&conf, "guimode=fancy\n").unwrap();
        assert_eq!(decide_mode(None, guiconf::read_mode(&conf)), GuiMode::Classic);
    }
}
