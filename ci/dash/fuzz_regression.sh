#!/usr/bin/env bash
# Copyright (c) 2026 The DeFCoN Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Fuzz regression for every pull request: build the fuzz binary with libFuzzer and sanitizers, then run every fuzz
# target once over its saved inputs. A target with no saved inputs runs once on an empty input, so a target that
# cannot even start fails the run (in September 2026 none of them started, and nothing noticed).
#
#   ci/dash/fuzz_regression.sh build               configure in-tree and build src/test/fuzz/fuzz
#   ci/dash/fuzz_regression.sh corpus <dir>        build the corpus in <dir> (which must not exist): the pinned Dash
#                                                  qa-assets inputs of this build's targets
#   ci/dash/fuzz_regression.sh run <dir>           check that <dir> holds exactly the pinned Dash inputs, and that the
#                                                  binary has exactly the expected targets; then run every target once
#                                                  over <dir>/<target>
#   <dir> is a plain absolute path, as the job gives it.
#
# Every saved input must reach a target that runs. An input for a target this build does not have or does not run
# fails the corpus step, and the run step trusts nothing the corpus step left behind: it derives what the corpus must
# hold from this script's pins, so a wrong, partial or swapped corpus fails it. DeFCoN's own seeds are not part of it
# yet: where they will live is still open.
#
# FUZZ_JOBS: parallel make jobs and targets (default: nproc). FUZZ_TIME_LIMIT: seconds the run may take (default 1800).
# The build needs clang-18 with compiler-rt, libevent, gmp and boost; the corpus step needs git and the network.

export LC_ALL=C.UTF-8
set -Eeuo pipefail

QA_ASSETS_URL=https://github.com/dashpay/qa-assets
# Pinned: a newer corpus is a reviewed change, not a moving target under every pull request.
QA_ASSETS_COMMIT=b4d1a7ce41a9a21d522381944ba84b60b1ad5b60
# What that commit gives this build (the inputs of its targets, as list_inputs writes them: count and sha256 of the
# list). They change with QA_ASSETS_COMMIT, EXPECTED_TARGETS and QA_ASSETS_DASH_ONLY, and only with them.
QA_ASSETS_USED_INPUTS=3403
QA_ASSETS_USED_SHA256=e02dcc488e1327c58e6d2d5a6692df1e1e862c440f4482ed966d7f6be9b41e11
# The targets this build must have, one per line, sorted. The binary lists only the targets it was built with, so a
# target whose source fell out of the build would otherwise vanish from the run unnoticed; a change that adds or
# removes a target updates this list with it.
EXPECTED_TARGETS=ci/dash/fuzz_targets.txt
# Dash targets with inputs in that qa-assets commit that this build does not have (Dash's own later targets); their
# inputs are left out by name. Any other directory there that is not a target here fails the corpus step.
QA_ASSETS_DASH_ONLY=ci/dash/fuzz_qa_assets_dash_only.txt
# Targets that fail today for a known reason, tracked elsewhere; each leaves this list with its fix.
#  - rpc stops at its first RPC command that is on neither of its two lists; 83 Dash and DeFCoN commands are on
#    none (as in Dash upstream), so it has never run past that check.
EXCLUDED_TARGETS=(
  rpc
)

jobs=${FUZZ_JOBS:-$(nproc)}

usage() { echo "usage: $0 build | corpus <dir> | run <dir>" >&2; exit 2; }
fail() { echo "$*" >&2; exit 1; }
is_listed() { grep -qxF -- "$1" "$2"; }
is_excluded() { local t; for t in "${EXCLUDED_TARGETS[@]}"; do [[ "$t" == "$1" ]] && return 0; done; return 1; }
# Fails if <dir> itself is a symbolic link (a broken one too): find does not look into it, but cd, cp and the fuzz
# runner follow it. <dir> must be spelled plainly (see plain_path), or a spelling like "link/" would hide the link.
not_a_link() { [[ ! -L "$1" ]] || fail "$1 is a symbolic link"; }
# Fails if <dir> holds anything that is neither a regular file nor a directory (a symbolic link, say): find -type f
# would leave it out, unseen.
only_files_and_dirs() {
  local odd
  not_a_link "$1"
  odd=$(find "$1" -mindepth 1 ! -type f ! -type d -print -quit)
  [[ -z "$odd" ]] || fail "$odd is neither a regular file nor a directory"
}
# Every top-level entry of <dir> is a directory: an input outside a target's directory reaches no target.
only_dirs_on_top() {
  local loose
  not_a_link "$1"
  loose=$(find "$1" -mindepth 1 -maxdepth 1 ! -type d -print -quit)
  [[ -z "$loose" ]] || fail "$loose is not in a target directory"
}
# Refuses <dir> unless it is a plain absolute path: it starts with "/", and has no newline, no trailing "/" and no
# empty, "." or ".." component. Nothing rewrites the path: every later check, cd, cp and the fuzz runner use it
# exactly as given, so the last component not_a_link looks at is the one they all see. Being absolute, it is not
# looked up in CDPATH by cd and cannot be read as an option by find or mkdir.
plain_path() {
  local p=$1
  [[ "$p" == /?* && "$p" != *$'\n'* && "$p" != */ && "$p/" != *//* && "$p/" != */./* && "$p/" != */../* ]] ||
    fail "$p: give the directory as a plain absolute path (no trailing /, no //, no . or .. component, no newline)"
}
# list_inputs <dir> <out>: one line per input, "<sha256>  ./<target>/<path>", sorted bytewise. Listings go through
# files, never a process substitution, whose failure set -e would not see.
list_inputs() {
  only_files_and_dirs "$1"
  (cd "$1" && find . -type f -print0 | xargs -0 -r sha256sum) | LC_ALL=C sort > "$2"
}

case "${1:-}" in
  build)
    ./autogen.sh
    ./configure CC=clang-18 CXX=clang++-18 \
      --enable-fuzz --with-sanitizers=fuzzer,address,undefined,integer \
      --disable-wallet --without-bdb --without-sqlite --without-gui --disable-bench --disable-stacktraces \
      --enable-suppress-external-warnings
    make -j"$jobs" -C src test/fuzz/fuzz
    ;;
  corpus)
    [[ $# -eq 2 ]] || usage
    dest=$2
    plain_path "$dest"
    [[ ! -e "$dest" && ! -L "$dest" ]] || fail "corpus: $dest exists already; the corpus is built in a new directory"
    fetched=$(mktemp -d)
    trap 'rm -rf "$fetched"' EXIT
    git -C "$fetched" init -q
    git -C "$fetched" fetch -q --depth=1 "$QA_ASSETS_URL" "$QA_ASSETS_COMMIT"
    git -C "$fetched" checkout -q FETCH_HEAD
    [[ "$(git -C "$fetched" rev-parse HEAD)" == "$QA_ASSETS_COMMIT" ]]
    mkdir -p "$dest"
    seeds=$fetched/fuzz_seed_corpus
    only_dirs_on_top "$seeds"
    find "$seeds" -mindepth 1 -maxdepth 1 -printf '%f\n' > "$fetched/dash.list"
    used=0; left=0
    while IFS= read -r name; do
      if is_listed "$name" "$EXPECTED_TARGETS"; then
        if is_excluded "$name"; then fail "corpus: qa-assets has inputs for $name, which this build does not run (EXCLUDED_TARGETS)"; fi
        only_files_and_dirs "$seeds/$name"
        cp -R "$seeds/$name" "$dest/$name"
        used=$((used + 1))
      elif is_listed "$name" "$QA_ASSETS_DASH_ONLY"; then
        left=$((left + 1))
      else
        fail "corpus: qa-assets has inputs for $name, which is neither a target here nor in $QA_ASSETS_DASH_ONLY"
      fi
    done < "$fetched/dash.list"
    list_inputs "$dest" "$fetched/all"
    inputs=$(grep -c '' "$fetched/all" || true)
    targets=$(find "$dest" -mindepth 1 -maxdepth 1 -type d | wc -l)
    echo "corpus: $inputs inputs for $targets targets; qa-assets: $used directories used, $left Dash-only left out"
    ;;
  run)
    [[ $# -eq 2 ]] || usage
    corpus=$2
    plain_path "$corpus"
    [[ -d "$corpus" ]] || fail "run: no corpus directory $corpus"
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    # What the corpus must hold, derived here and not read from it: the pinned Dash inputs (count and checksum of
    # their list). A wrong, partial, extended or swapped corpus fails.
    only_dirs_on_top "$corpus"
    list_inputs "$corpus" "$work/all"
    dash_inputs=$(grep -c '' "$work/all" || true)
    dash_sha256=$(sha256sum < "$work/all" | cut -d' ' -f1)
    if [[ "$dash_inputs" != "$QA_ASSETS_USED_INPUTS" || "$dash_sha256" != "$QA_ASSETS_USED_SHA256" ]]; then
      fail "run: the Dash inputs in $corpus ($dash_inputs, $dash_sha256) are not the pinned ones ($QA_ASSETS_USED_INPUTS, $QA_ASSETS_USED_SHA256)"
    fi
    # The binary prints its targets and then aborts, so its own exit code says nothing; an empty or partial list
    # fails the comparison below.
    built=$(PRINT_ALL_FUZZ_TARGETS_AND_ABORT=1 src/test/fuzz/fuzz 2>/dev/null || true)
    printf '%s\n' "$built" | LC_ALL=C sort -u > "$work/built"
    if ! diff -u "$EXPECTED_TARGETS" "$work/built"; then
      echo "The fuzz binary's targets are not those of $EXPECTED_TARGETS (-: expected, missing; +: built, not expected)." >&2
      echo "A change that adds or removes a target updates that list with it." >&2
      exit 1
    fi
    excluded=$(IFS=,; echo "${EXCLUDED_TARGETS[*]}")
    echo "corpus: $dash_inputs pinned Dash inputs; not run (known failures, see EXCLUDED_TARGETS): $excluded"
    timeout --kill-after=60 "${FUZZ_TIME_LIMIT:-1800}" python3 test/fuzz/test_runner.py --par "$jobs" --exclude "$excluded" "$corpus"
    ;;
  *)
    usage
    ;;
esac
