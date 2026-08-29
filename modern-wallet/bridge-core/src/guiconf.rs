// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! `gui.conf` -- the contract between the launcher and the two wallets.
//!
//! A deliberately separate file in the datadir root (`%APPDATA%\Defcon` on
//! Windows, `~/.defcon` elsewhere), never `defcon.conf`: defcond rejects
//! options it does not know, so the GUI preference must not live there. The
//! format is the same `key=value` lines with `#` comments, and the one key the
//! launcher reads is human-readable by request:
//!
//! ```text
//! guimode=classic
//! guimode=modern
//! ```
//!
//! Anything else in the file -- comments, unknown keys a future version may
//! add -- survives a rewrite byte for byte. Only the `guimode` line moves.

use std::fs;
use std::io;
use std::path::Path;

pub const GUI_CONF_FILENAME: &str = "gui.conf";

/// Which wallet the launcher starts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GuiMode {
    /// The unchanged Qt wallet (`defcon-qt`).
    Classic,
    /// The React/Tauri wallet (`defcon-wallet`).
    Modern,
}

impl GuiMode {
    pub fn as_str(self) -> &'static str {
        match self {
            GuiMode::Classic => "classic",
            GuiMode::Modern => "modern",
        }
    }

    /// Parses a `guimode` value. Whitespace-tolerant and case-insensitive,
    /// because the file is meant to be edited by hand; anything that is not
    /// one of the two known words is None, and the caller falls back to the
    /// safe default (classic).
    pub fn parse(value: &str) -> Option<GuiMode> {
        match value.trim().to_ascii_lowercase().as_str() {
            "classic" => Some(GuiMode::Classic),
            "modern" => Some(GuiMode::Modern),
            _ => None,
        }
    }
}

/// The parsed file: the mode, plus every line verbatim so a rewrite preserves
/// what it does not understand.
#[derive(Clone, Debug, Default)]
pub struct GuiConf {
    pub guimode: Option<GuiMode>,
    lines: Vec<String>,
}

impl GuiConf {
    /// Parses `gui.conf` text. The last `guimode` line wins, matching how
    /// defcon.conf treats repeated keys.
    pub fn parse(text: &str) -> GuiConf {
        let mut conf = GuiConf {
            guimode: None,
            lines: text.lines().map(str::to_owned).collect(),
        };
        for line in &conf.lines {
            if let Some(value) = key_value(line, "guimode") {
                if let Some(mode) = GuiMode::parse(value) {
                    conf.guimode = Some(mode);
                }
            }
        }
        conf
    }

    /// Renders the file with `guimode` set to `mode`: the existing `guimode`
    /// line is replaced in place, or one is appended if there was none. Every
    /// other line is preserved exactly.
    pub fn render_with_mode(&self, mode: GuiMode) -> String {
        let mut out: Vec<String> = Vec::with_capacity(self.lines.len() + 1);
        let mut replaced = false;
        for line in &self.lines {
            if key_value(line, "guimode").is_some() {
                if !replaced {
                    out.push(format!("guimode={}", mode.as_str()));
                    replaced = true;
                }
                // Repeated guimode lines collapse into the one we wrote:
                // keeping stale duplicates around would make the file lie.
                continue;
            }
            out.push(line.clone());
        }
        if !replaced {
            out.push(format!("guimode={}", mode.as_str()));
        }
        let mut text = out.join("\n");
        text.push('\n');
        text
    }
}

/// Reads the mode from `gui.conf` at `path`. A missing file, an unreadable
/// file or an unknown value are all None -- the launcher treats every one of
/// them as "classic", because the safe default is the wallet that has always
/// shipped.
pub fn read_mode(path: &Path) -> Option<GuiMode> {
    let text = fs::read_to_string(path).ok()?;
    GuiConf::parse(&text).guimode
}

/// Writes `mode` into `gui.conf` at `path`, preserving unrelated content and
/// creating the parent directory if needed.
pub fn write_mode(path: &Path, mode: GuiMode) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let existing = fs::read_to_string(path).unwrap_or_default();
    let rendered = GuiConf::parse(&existing).render_with_mode(mode);
    fs::write(path, rendered)
}

/// `key=value` extraction for one line: returns the value when the line's key
/// (before the first `=`) matches, ignoring surrounding whitespace. Comment
/// lines never match.
fn key_value<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let trimmed = line.trim_start();
    if trimmed.starts_with('#') {
        return None;
    }
    let (k, v) = trimmed.split_once('=')?;
    if k.trim() == key {
        Some(v)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_both_modes_and_rejects_anything_else() {
        assert_eq!(GuiMode::parse("classic"), Some(GuiMode::Classic));
        assert_eq!(GuiMode::parse("modern"), Some(GuiMode::Modern));
        assert_eq!(GuiMode::parse("  Modern \n"), Some(GuiMode::Modern));
        assert_eq!(GuiMode::parse("CLASSIC"), Some(GuiMode::Classic));
        // The numeric values the task explicitly rules out, and junk.
        assert_eq!(GuiMode::parse("1"), None);
        assert_eq!(GuiMode::parse("2"), None);
        assert_eq!(GuiMode::parse(""), None);
        assert_eq!(GuiMode::parse("qt"), None);
    }

    #[test]
    fn last_guimode_line_wins_and_comments_do_not_count() {
        let conf = GuiConf::parse("# guimode=modern\nguimode=classic\nguimode=modern\n");
        assert_eq!(conf.guimode, Some(GuiMode::Modern));
    }

    #[test]
    fn rewrite_preserves_unknown_lines_and_replaces_in_place() {
        let text = "# DeFCoN GUI preferences\nfuturekey=value\nguimode=classic\n";
        let out = GuiConf::parse(text).render_with_mode(GuiMode::Modern);
        assert_eq!(out, "# DeFCoN GUI preferences\nfuturekey=value\nguimode=modern\n");
    }

    #[test]
    fn rewrite_appends_when_missing_and_collapses_duplicates() {
        let out = GuiConf::parse("# empty\n").render_with_mode(GuiMode::Classic);
        assert_eq!(out, "# empty\nguimode=classic\n");

        let dupes = "guimode=classic\nother=1\nguimode=modern\n";
        let out = GuiConf::parse(dupes).render_with_mode(GuiMode::Classic);
        assert_eq!(out, "guimode=classic\nother=1\n");
    }

    #[test]
    fn file_roundtrip_creates_parent_and_reads_back() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("sub").join(GUI_CONF_FILENAME);
        assert_eq!(read_mode(&path), None); // missing file -> None
        write_mode(&path, GuiMode::Modern).unwrap();
        assert_eq!(read_mode(&path), Some(GuiMode::Modern));
        write_mode(&path, GuiMode::Classic).unwrap();
        assert_eq!(read_mode(&path), Some(GuiMode::Classic));
    }
}
