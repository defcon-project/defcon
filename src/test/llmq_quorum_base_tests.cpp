// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <deploymentstatus.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <llmq/utils.h>
#include <random.h>
#include <sync.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>

// A quorum hash arrives from the network (a DKG message, a relayed qfcommit, a mined commitment),
// and the block it names is looked up and its parent passed on as `pQuorumBaseBlockIndex->pprev`.
// Genesis has no parent, so that pointer is null. It used to reach IsQuorumTypeEnabled()'s
// gsl::not_null parameter, whose precondition calls std::terminate(): [[noreturn]] noexcept, so no
// exception handler could contain it. A parentless base must be rejected instead.
//
// The first case is the regression test of upstream Dash ca56af88, adapted to the free function
// llmq::IsQuorumTypeEnabled(). Upstream keeps it in evo_utils_tests, which this fork's CI does not
// run yet, so it lives here.

BOOST_AUTO_TEST_SUITE(llmq_quorum_base_tests)

BOOST_FIXTURE_TEST_CASE(genesis_quorum_base_null_pprev_is_safe, RegTestingSetup)
{
    const CBlockIndex* genesis = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip());
    BOOST_REQUIRE(genesis != nullptr);
    BOOST_REQUIRE_EQUAL(genesis->nHeight, 0);
    BOOST_REQUIRE(genesis->pprev == nullptr);

    const auto llmq_type = Params().GetConsensus().llmqTypeChainLocks;

    // The sink itself, as reached via `pQuorumBaseBlockIndex->pprev`. Passed through a variable
    // rather than a literal so this still compiles against the pre-fix gsl::not_null signature,
    // where it terminated at runtime.
    const CBlockIndex* const null_index = genesis->pprev;
    BOOST_CHECK(!llmq::IsQuorumTypeEnabled(llmq_type, null_index));

    // The GetAllQuorumMembers() path that CFinalCommitment::Verify() takes.
    BOOST_CHECK(llmq::utils::GetAllQuorumMembers(llmq_type, *Assert(m_node.dmnman), *Assert(m_node.llmq_ctx)->qsnapman,
                                                 genesis).empty());
}

// The block path: CheckLLMQCommitment() accepts genesis as a quorum base (it is an ancestor of
// every block) and hands a non-null commitment to CFinalCommitment::Verify(), which looked up the
// members through the base block's missing parent. The commitment below is well-formed in every
// field Verify() checks before that lookup, so only the parentless base can reject it.
BOOST_FIXTURE_TEST_CASE(genesis_quorum_base_commitment_is_rejected, RegTestingSetup)
{
    const CBlockIndex* genesis = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip());
    BOOST_REQUIRE(genesis != nullptr);
    BOOST_REQUIRE(genesis->pprev == nullptr);

    const auto& llmq_params = *Assert(Params().GetLLMQ(Params().GetConsensus().llmqTypeChainLocks));

    llmq::CFinalCommitment qc(llmq_params, genesis->GetBlockHash());
    qc.nVersion = llmq::CFinalCommitment::GetVersion(llmq::IsQuorumRotationEnabled(llmq_params, genesis),
                                                     DeploymentActiveAfter(genesis, Params().GetConsensus(),
                                                                           Consensus::DEPLOYMENT_V19));
    qc.quorumIndex = 0;
    std::fill(qc.signers.begin(), qc.signers.end(), true);
    std::fill(qc.validMembers.begin(), qc.validMembers.end(), true);
    CBLSSecretKey sk;
    sk.MakeNewKey();
    qc.quorumPublicKey = sk.GetPublicKey();
    qc.quorumVvecHash = GetRandHash();
    qc.membersSig = sk.Sign(GetRandHash(), bls::bls_legacy_scheme.load());
    qc.quorumSig = sk.Sign(GetRandHash(), bls::bls_legacy_scheme.load());
    BOOST_REQUIRE(!qc.IsNull());

    BOOST_CHECK(!qc.Verify(*Assert(m_node.dmnman), *Assert(m_node.llmq_ctx)->qsnapman, genesis, /*checkSigs=*/false));
}

BOOST_AUTO_TEST_SUITE_END()
