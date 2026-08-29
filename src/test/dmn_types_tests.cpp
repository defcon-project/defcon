// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <chainparamsbase.h>
#include <util/system.h>

#include <limits>
#include <evo/dmn_types.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(dmn_types_tests, BasicTestingSetup)

// A masternode type's governance vote weight and its payout-slot weight are
// separate dials that happen to share values today, so a swapped read compiles
// and behaves identically. This pins each struct field to the consensus
// parameter it must come from.
BOOST_AUTO_TEST_CASE(weights_map_to_their_params)
{
    const Consensus::Params& params = Params().GetConsensus();

    const auto regular = GetMnType(MnType::Regular);
    BOOST_CHECK_EQUAL(regular.voting_weight, params.regularVoteWeight);
    BOOST_CHECK_EQUAL(regular.payment_weight, params.regularPaymentWeight);
    BOOST_CHECK_EQUAL(regular.collat_amount, params.regularMnCollateral);

    const auto evo = GetMnType(MnType::Evo);
    BOOST_CHECK_EQUAL(evo.voting_weight, params.evoVoteWeight);
    BOOST_CHECK_EQUAL(evo.payment_weight, params.evoPaymentWeight);
    BOOST_CHECK_EQUAL(evo.collat_amount, params.evoMnCollateral);

    const auto compute = GetMnType(MnType::Compute);
    BOOST_CHECK_EQUAL(compute.voting_weight, params.computeVoteWeight);
    BOOST_CHECK_EQUAL(compute.payment_weight, params.computePaymentWeight);
    BOOST_CHECK_EQUAL(compute.collat_amount, params.computeMnCollateral);

    const auto invalid = GetMnType(MnType::Invalid);
    BOOST_CHECK_EQUAL(invalid.voting_weight, 0);
    BOOST_CHECK_EQUAL(invalid.payment_weight, 0);
}

// The Compute type ships dormant: its activation height must stay unset --
// an unreachable maximum -- on every network until a coordinated release
// sets it. A height that drifts in by accident would activate a masternode
// type nobody scheduled.
BOOST_AUTO_TEST_CASE(compute_activation_height_is_pinned_dormant)
{
    for (const auto& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const auto params = CreateChainParams(ArgsManager{}, chain);
        BOOST_CHECK_EQUAL(params->GetConsensus().nComputeNodeActivationHeight, std::numeric_limits<int>::max());
        BOOST_CHECK(!IsComputeTypeActive(std::numeric_limits<int>::max() - 1, params->GetConsensus()));
    }
}

BOOST_AUTO_TEST_SUITE_END()
