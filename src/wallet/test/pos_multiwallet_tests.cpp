// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/multiwallet.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <memory>

BOOST_FIXTURE_TEST_SUITE(pos_multiwallet_tests, WalletTestingSetup)

// ThreadStakeMiner restarts PoSMiner after an unexpected failure, and PoSMiner
// begins by rebuilding the stakable-wallet list. That rebuild used to hand
// every wallet back a cleared staking switch, so the first exception anywhere
// in the miner silently ended staking for the life of the process: the thread
// survived, getstakinginfo kept answering staking:true, and no block was ever
// staked again. The rebuild has to preserve the switch, in both positions.
BOOST_AUTO_TEST_CASE(reinitialize_preserves_staking_switch)
{
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                                "pos-multiwallet-test", CreateDummyWalletDatabase());
    AddWallet(wallet);

    MultiwalletInitialize();
    BOOST_CHECK(!IsWalletStaking("pos-multiwallet-test"));

    BOOST_CHECK(ToggleWalletStaking("pos-multiwallet-test"));
    BOOST_CHECK(IsWalletStaking("pos-multiwallet-test"));

    // A name nothing carries answers false rather than touching anything.
    BOOST_CHECK(!ToggleWalletStaking("no-such-wallet"));
    BOOST_CHECK(IsWalletStaking("pos-multiwallet-test"));

    // The failure-retry path: a second initialize over a live list.
    MultiwalletInitialize();
    BOOST_CHECK(IsWalletStaking("pos-multiwallet-test"));

    // The periodic rebuild inside the miner loop preserves it too.
    MultiwalletMaintenance();
    BOOST_CHECK(IsWalletStaking("pos-multiwallet-test"));

    BOOST_CHECK(!ToggleWalletStaking("pos-multiwallet-test"));
    BOOST_CHECK(!IsWalletStaking("pos-multiwallet-test"));

    // Off stays off across a rebuild.
    MultiwalletInitialize();
    BOOST_CHECK(!IsWalletStaking("pos-multiwallet-test"));

    RemoveWallet(wallet, std::nullopt);
    // Leave clean process-global state for whatever test runs next.
    MultiwalletInitialize();
}

BOOST_AUTO_TEST_SUITE_END()
