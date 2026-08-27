// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <crypto/common.h>
#include <llmq/snapshot.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace llmq;

namespace {
uint256 GetTestBlockHash(uint32_t n)
{
    uint256 h;
    WriteLE32(h.begin(), n);
    return h;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(llmq_snapshot_tests, BasicTestingSetup)

// dash#7349, adapted: a QRINFO request may name the same base block more than
// once. The non-legacy path sorts and deduplicates; the legacy path keeps the
// caller-supplied duplicates so the wire response to older peers stays
// bit-for-bit identical -- which is only sound because a sorted duplicate is a
// no-op for GetLastBaseBlockHash, pinned here.
BOOST_AUTO_TEST_CASE(get_last_base_block_hash_repeated_base_blocks)
{
    std::vector<CBlockIndex> blocks(4);
    std::vector<uint256> hashes{
        GetTestBlockHash(10),
        GetTestBlockHash(20),
        GetTestBlockHash(30),
        GetTestBlockHash(40),
    };
    for (size_t i{0}; i < blocks.size(); ++i) {
        blocks[i].nHeight = static_cast<int>((i + 1) * 10);
        blocks[i].phashBlock = &hashes[i];
    }

    // Non-legacy: sorts internally, so unsorted input with duplicates is fine.
    std::vector<const CBlockIndex*> unsorted_repeated_base_blocks{
        &blocks[2],
        &blocks[0],
        &blocks[1],
        &blocks[1],
    };
    BOOST_CHECK(GetLastBaseBlockHash(unsorted_repeated_base_blocks, &blocks[3], false) == hashes[2]);
    BOOST_CHECK(GetLastBaseBlockHash(unsorted_repeated_base_blocks, &blocks[1], false) == hashes[1]);

    // Legacy: relies on the caller-supplied sort and tolerates duplicates as a no-op.
    std::vector<const CBlockIndex*> sorted_repeated_base_blocks{
        &blocks[0],
        &blocks[1],
        &blocks[1],
        &blocks[2],
    };
    std::vector<const CBlockIndex*> sorted_unique_base_blocks{
        &blocks[0],
        &blocks[1],
        &blocks[2],
    };
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[3], true) == hashes[2]);
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[1], true) == hashes[1]);
    // Legacy no-op proof: duplicate vs unique input produces the same hash.
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[3], true) ==
                GetLastBaseBlockHash(sorted_unique_base_blocks, &blocks[3], true));
    BOOST_CHECK(GetLastBaseBlockHash(sorted_repeated_base_blocks, &blocks[1], true) ==
                GetLastBaseBlockHash(sorted_unique_base_blocks, &blocks[1], true));
}

BOOST_AUTO_TEST_SUITE_END()
