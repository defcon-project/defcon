#!/usr/bin/env bash
#
# Copyright (c) 2018-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C

GIT_HEAD=$(git rev-parse HEAD)
if [ -n "$CIRRUS_PR" ]; then
  COMMIT_RANGE="${CIRRUS_BASE_SHA}..$GIT_HEAD"
  test/lint/commit-script-check.sh "$COMMIT_RANGE"
fi
export COMMIT_RANGE

# This only checks that the trees are pure subtrees, it is not doing a full
# check with -r to not have to fetch all the remotes.
test/lint/git-subtree-check.sh src/crypto/ctaes
test/lint/git-subtree-check.sh src/secp256k1
test/lint/git-subtree-check.sh src/minisketch
test/lint/git-subtree-check.sh src/univalue
test/lint/git-subtree-check.sh src/leveldb
test/lint/check-doc.py
test/lint/all-lint.py

if [ -n "$COMMIT_RANGE" ]; then
  echo
  git log --no-merges --oneline "$COMMIT_RANGE"
fi
