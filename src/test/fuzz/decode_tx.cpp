// Copyright (c) 2019-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <primitives/transaction.h>
#include <test/fuzz/fuzz.h>
#include <util/strencodings.h>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

FUZZ_TARGET(decode_tx)
{
    const std::string tx_hex = HexStr(std::string{buffer.begin(), buffer.end()});
    CMutableTransaction mtx;
    // Any input, decoded or refused, must not crash. (Bitcoin asserted that DecodeHexTx(..., false, false), which
    // tries neither encoding, fails; the two arguments are gone here, and so is the reason for that assert: a seed
    // that is a valid transaction tripped it.)
    (void)DecodeHexTx(mtx, tx_hex);
}
