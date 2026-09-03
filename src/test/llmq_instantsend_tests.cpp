// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <consensus/params.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <test/util/setup_common.h>
#include <util/system.h>

#include <limits>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(llmq_instantsend_tests, BasicTestingSetup)

// The tip-height profile resolver is the single point that keeps every node
// signing and verifying InstantSend locks with the same quorum type across
// the switchover. Same contract as the ChainLock resolver: one-way, exact at
// the boundary, inert until both halves of the V2 configuration are present.
BOOST_AUTO_TEST_CASE(instantsend_type_resolver)
{
    Consensus::Params params;
    params.llmqTypeDIP0024InstantSend = Consensus::LLMQType::LLMQ_60_75;

    // No V2 configured: the legacy type at every height.
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 0) == Consensus::LLMQType::LLMQ_60_75);
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_60_75);

    // A V2 type without an activation height must never fire.
    params.llmqTypeDIP0024InstantSendV2 = Consensus::LLMQType::LLMQ_DEFCON;
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_60_75);

    // The boundary is exact and one-way.
    params.nInstantSendV2ActivationHeight = 7200;
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 7199) == Consensus::LLMQType::LLMQ_60_75);
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 7200) == Consensus::LLMQType::LLMQ_DEFCON);
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_DEFCON);

    // An activation height without a V2 type is a half-written configuration
    // and must fail closed to the legacy profile.
    params.llmqTypeDIP0024InstantSendV2 = Consensus::LLMQType::LLMQ_NONE;
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_60_75);

    // A network with no InstantSend at all stays that way through the resolver.
    params.llmqTypeDIP0024InstantSend = Consensus::LLMQType::LLMQ_NONE;
    BOOST_CHECK(llmq::GetInstantSendLLMQType(params, 1000000) == Consensus::LLMQType::LLMQ_NONE);
}

// The devnet is the rehearsal for the mainnet configuration: InstantSend on
// the Q60 profile, which is what the mainnet bundle will set at its own
// height. Pin the two halves together so neither can be edited alone.
BOOST_AUTO_TEST_CASE(devnet_instantsend_moves_to_q60)
{
    gArgs.SoftSetBoolArg("-devnet", true);
    const auto chainparams = CreateChainParams(*m_node.args, CBaseChainParams::DEVNET);
    gArgs.ForceRemoveArg("devnet");
    const auto& consensus = chainparams->GetConsensus();

    BOOST_CHECK(consensus.llmqTypeDIP0024InstantSendV2 == Consensus::LLMQType::LLMQ_DEFCON);
    BOOST_CHECK(consensus.nInstantSendV2ActivationHeight != std::numeric_limits<int>::max());
    // The Q60 profile must be forming before InstantSend can resolve to it:
    // its formation gate sits ahead of the ChainLock switchover, and the
    // InstantSend switchover comes after that.
    BOOST_CHECK(consensus.nInstantSendV2ActivationHeight >= consensus.nChainLocksV2ActivationHeight);
    BOOST_CHECK(chainparams->GetLLMQ(consensus.llmqTypeDIP0024InstantSendV2).has_value());
    BOOST_CHECK(chainparams->GetLLMQ(consensus.llmqTypeDIP0024InstantSend).has_value());
}

// Mainnet gets its heights in the v23 bundle, all at once. Until then the
// resolver must be inert there: no V2 type, an unreachable height.
BOOST_AUTO_TEST_CASE(mainnet_instantsend_switchover_unset)
{
    const auto chainparams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const auto& consensus = chainparams->GetConsensus();

    BOOST_CHECK(consensus.llmqTypeDIP0024InstantSendV2 == Consensus::LLMQType::LLMQ_NONE);
    BOOST_CHECK(consensus.nInstantSendV2ActivationHeight == std::numeric_limits<int>::max());
    BOOST_CHECK(llmq::GetInstantSendLLMQType(consensus, 1000000000) == consensus.llmqTypeDIP0024InstantSend);
}

BOOST_AUTO_TEST_SUITE_END()
