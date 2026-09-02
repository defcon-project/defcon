// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <hash.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <uint256.h>

#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(llmq_blockprocessor_tests, TestingSetup)

namespace {
constexpr auto TEST_TYPE = Consensus::LLMQType::LLMQ_50_60;
constexpr int TEST_SIZE = 50;

// A commitment carrying nothing but what the mineable-commitment bookkeeping
// reads: the quorum it belongs to and how many members signed it. Nothing here
// is verified -- AddMineableCommitment is reached only after Verify() has
// already passed, and the two competing commitments this test models are both
// consensus-valid.
llmq::CFinalCommitment MakeCommitment(const uint256& quorumHash, int signers)
{
    llmq::CFinalCommitment fqc;
    fqc.llmqType = TEST_TYPE;
    fqc.quorumHash = quorumHash;
    fqc.signers.assign(TEST_SIZE, false);
    fqc.validMembers.assign(TEST_SIZE, true);
    for (int i = 0; i < signers; ++i) {
        fqc.signers[i] = true;
    }
    assert(fqc.CountSigners() == signers);
    return fqc;
}
} // namespace

// Competing final commitments for one quorum are normal: members that saw
// different contributions produce different validMembers views, and each view
// that minSize members agree on becomes a commitment in its own right. The node
// keeps the one with the most signers, because that is the only discriminator
// the protocol offers between two otherwise valid commitments for the same slot.
//
// Superseding one used to leave the other behind. The reference into
// minableCommitmentsByQuorum was repointed at the new hash *before* it was used
// to erase, so the erase removed a key that had not been inserted yet and the
// displaced commitment stayed in minableCommitments -- served by
// GetMineableCommitmentByHash and never freed.
BOOST_AUTO_TEST_CASE(mineable_commitment_replacement_erases_the_old_one)
{
    BOOST_REQUIRE(m_node.llmq_ctx && m_node.llmq_ctx->quorum_block_processor);
    auto& blockprocessor = *m_node.llmq_ctx->quorum_block_processor;

    const uint256 quorumHash = uint256S("01");
    const auto weak = MakeCommitment(quorumHash, 4);
    const auto strong = MakeCommitment(quorumHash, 40);
    const uint256 weakHash = ::SerializeHash(weak);
    const uint256 strongHash = ::SerializeHash(strong);
    BOOST_REQUIRE(weakHash != strongHash);

    BOOST_CHECK(blockprocessor.AddMineableCommitment(weak).has_value());

    llmq::CFinalCommitment out;
    BOOST_CHECK(blockprocessor.GetMineableCommitmentByHash(weakHash, out));
    BOOST_CHECK_EQUAL(out.CountSigners(), 4);

    // The better commitment must take the slot, and be relayed for doing so.
    BOOST_CHECK(blockprocessor.AddMineableCommitment(strong).has_value());
    BOOST_CHECK(blockprocessor.GetMineableCommitmentByHash(strongHash, out));
    BOOST_CHECK_EQUAL(out.CountSigners(), 40);

    // The regression: the superseded commitment must be gone, not merely
    // unreferenced.
    BOOST_CHECK(!blockprocessor.GetMineableCommitmentByHash(weakHash, out));
}

// The other direction, which has always worked and must keep working: a weaker
// commitment arriving second changes nothing and is not relayed onward.
BOOST_AUTO_TEST_CASE(mineable_commitment_weaker_does_not_displace_stronger)
{
    BOOST_REQUIRE(m_node.llmq_ctx && m_node.llmq_ctx->quorum_block_processor);
    auto& blockprocessor = *m_node.llmq_ctx->quorum_block_processor;

    const uint256 quorumHash = uint256S("02");
    const auto strong = MakeCommitment(quorumHash, 40);
    const auto weak = MakeCommitment(quorumHash, 4);
    const uint256 strongHash = ::SerializeHash(strong);
    const uint256 weakHash = ::SerializeHash(weak);

    BOOST_CHECK(blockprocessor.AddMineableCommitment(strong).has_value());
    BOOST_CHECK(!blockprocessor.AddMineableCommitment(weak).has_value());

    llmq::CFinalCommitment out;
    BOOST_CHECK(blockprocessor.GetMineableCommitmentByHash(strongHash, out));
    BOOST_CHECK_EQUAL(out.CountSigners(), 40);
    BOOST_CHECK(!blockprocessor.GetMineableCommitmentByHash(weakHash, out));
}

// Equal signer counts are two different commitments for the same slot with
// nothing to choose between them. The first one seen is kept, and the second is
// not relayed -- otherwise two nodes could relay each other's copy forever.
BOOST_AUTO_TEST_CASE(mineable_commitment_equal_signers_keeps_the_first)
{
    BOOST_REQUIRE(m_node.llmq_ctx && m_node.llmq_ctx->quorum_block_processor);
    auto& blockprocessor = *m_node.llmq_ctx->quorum_block_processor;

    const uint256 quorumHash = uint256S("03");
    auto first = MakeCommitment(quorumHash, 10);
    auto second = MakeCommitment(quorumHash, 10);
    // Same signer count, a different set of signers -- which is what two
    // competing commitments for one slot actually look like. quorumIndex is not
    // usable here: this commitment version is the NON_INDEXED one, so the field
    // is not serialised and both would hash identically.
    second.signers[0] = false;
    second.signers[10] = true;
    const uint256 firstHash = ::SerializeHash(first);
    const uint256 secondHash = ::SerializeHash(second);
    BOOST_REQUIRE(firstHash != secondHash);

    BOOST_CHECK(blockprocessor.AddMineableCommitment(first).has_value());
    BOOST_CHECK(!blockprocessor.AddMineableCommitment(second).has_value());

    llmq::CFinalCommitment out;
    BOOST_CHECK(blockprocessor.GetMineableCommitmentByHash(firstHash, out));
    BOOST_CHECK(!blockprocessor.GetMineableCommitmentByHash(secondHash, out));
}

BOOST_AUTO_TEST_SUITE_END()
