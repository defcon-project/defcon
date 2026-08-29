// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/params.h>
#include <evo/deterministicmns.h>
#include <masternode/collateral.h>
#include <primitives/transaction.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <memory>

BOOST_FIXTURE_TEST_SUITE(collateral_tests, BasicTestingSetup)

namespace {
//! A one-masternode list whose single member holds `collateral`, registered at
//! `registeredHeight`. Enough for the premature-spend rule, which reads only
//! the collateral outpoint and the registration height.
CDeterministicMNList ListWithCollateral(const COutPoint& collateral, int registeredHeight)
{
    CDeterministicMNList list(uint256(), registeredHeight, /*totalRegisteredCount=*/1);
    auto dmn = std::make_shared<CDeterministicMN>(/*internalId=*/1);
    dmn->proTxHash = uint256::ONE;
    dmn->collateralOutpoint = collateral;
    auto state = std::make_shared<CDeterministicMNState>();
    state->nRegisteredHeight = registeredHeight;
    // A non-null owner key: AddMN rejects a default-valued unique property.
    CKeyID owner;
    std::memset(owner.begin(), 0x11, owner.size());
    state->keyIDOwner = owner;
    dmn->pdmnState = state;
    list.AddMN(dmn);
    return list;
}
} // namespace

/**
 * The premature-collateral rule is a pure function of the list and the height.
 *
 * It used to read a process-local cache filled at startup, so the same block
 * could be judged differently by two nodes with different startup history --
 * a fork. Reading the deterministic list instead makes the verdict depend only
 * on inputs every node shares, which this pins down.
 */
BOOST_AUTO_TEST_CASE(premature_collateral_is_a_function_of_the_list)
{
    Consensus::Params params;
    params.minStaticCollateral = 100;

    const COutPoint collateral(uint256::TWO, 0);
    const int reg = 1000;
    const CDeterministicMNList list = ListWithCollateral(collateral, reg);

    // An outpoint that is not a registered collateral is always allowed,
    // whether the list is populated or empty.
    BOOST_CHECK(CheckPrematureCollateralMovement(list, COutPoint(uint256::ONE, 7), reg + 5, params));
    BOOST_CHECK(CheckPrematureCollateralMovement(CDeterministicMNList(), collateral, reg + 5, params));

    // The registered collateral, spent before maturity, is rejected.
    BOOST_CHECK(!CheckPrematureCollateralMovement(list, collateral, reg + params.minStaticCollateral - 1, params));

    // The registered collateral, spent at or after maturity, is allowed.
    BOOST_CHECK(CheckPrematureCollateralMovement(list, collateral, reg + params.minStaticCollateral, params));
    BOOST_CHECK(CheckPrematureCollateralMovement(list, collateral, reg + params.minStaticCollateral + 50, params));
}

BOOST_AUTO_TEST_SUITE_END()
