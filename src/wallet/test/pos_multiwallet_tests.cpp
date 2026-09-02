// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <pos/minter.h>
#include <pos/stake.h>
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

// Whether a wallet is switched on for staking and whether anything is actually
// staking it are different facts, and the reason getstakinginfo now reports both
// is that they can disagree. When the minter thread died on its first attempt,
// eight nodes went on answering staking:true with a full weight; every field the
// RPC had described the wallet, and the wallet was fine. Nothing in it could
// have said that the thread was gone.
//
// So the assertion that matters is not that the flag has some value -- it is
// that the flag and the wallet's switch move independently of each other.
BOOST_AUTO_TEST_CASE(minter_liveness_is_independent_of_wallet_state)
{
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                                "pos-minter-liveness-test", CreateDummyWalletDatabase());
    AddWallet(wallet);
    MultiwalletInitialize();

    // No minter thread runs in a unit test, which is exactly the state the old
    // RPC could not describe.
    BOOST_CHECK(!fMinterRunning);

    BOOST_CHECK(ToggleWalletStaking("pos-minter-liveness-test"));
    BOOST_CHECK(IsWalletStaking("pos-minter-liveness-test"));

    // The wallet says it is staking. Nothing is staking it, and now that is
    // visible rather than merely true.
    BOOST_CHECK(!fMinterRunning);

    // And the other direction: a live minter says nothing about any one wallet's
    // switch, so turning the wallet off must not move the flag.
    fMinterRunning = true;
    BOOST_CHECK(!ToggleWalletStaking("pos-minter-liveness-test"));
    BOOST_CHECK(!IsWalletStaking("pos-minter-liveness-test"));
    BOOST_CHECK(fMinterRunning);
    fMinterRunning = false;

    RemoveWallet(wallet, std::nullopt);
    MultiwalletInitialize();
}

// The minter's failure has to survive the throw that caused it, because the one
// that started this could not even be printed: JSONRPCError throws a UniValue,
// which is not a std::exception, so the handler had no what() to log and wrote
// "Exception: <null>". Recording the message under its own lock is what lets the
// RPC answer with something a reader can act on.
BOOST_AUTO_TEST_CASE(minter_last_error_round_trips)
{
    SetMinterLastError("no signing provider for the kernel script");
    BOOST_CHECK_EQUAL(MinterLastError(), "no signing provider for the kernel script");

    const int64_t first = MinterLastErrorTime();
    BOOST_CHECK(first > 0);

    // A later failure replaces the earlier one. Keeping only the most recent is
    // deliberate: a node failing every retry would otherwise grow an unbounded
    // list of identical messages, and the newest is the one worth reporting.
    SetMinterLastError("second failure");
    BOOST_CHECK_EQUAL(MinterLastError(), "second failure");
    BOOST_CHECK(MinterLastErrorTime() >= first);

    // Empty means "has not failed", and getstakinginfo omits the field entirely
    // in that state -- so an empty string has to be storable, not just initial.
    SetMinterLastError("");
    BOOST_CHECK(MinterLastError().empty());
}

// A registry entry outlives the wallet it names -- the list is rebuilt on every
// toggle, the miner works from a copy of it for seconds at a time, and
// unloadwallet can free the wallet in between. The entry used to hold a raw
// CWallet*, which contributed nothing to the wallet's refcount: UnloadWallet's
// wait for the last owner could not see this reference at all, and returned at
// the moment the wallet was deleted. The one guard that existed for it,
// RemoveWallet(), had no caller anywhere in the tree.
BOOST_AUTO_TEST_CASE(a_registry_entry_cannot_hand_out_a_freed_wallet)
{
    Consensus::Params params = Params().GetConsensus();

    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                                "pos-lifetime-test", CreateDummyWalletDatabase());
    std::weak_ptr<CWallet> observer = wallet;
    CStakeWallet entry(wallet, params);

    {
        // Asking for the wallet gives a reference that is strong for as long as
        // the caller keeps it. That is the contract the miner relies on: it
        // dereferences for the length of a staking attempt.
        std::shared_ptr<CWallet> held = entry.GetWallet();
        BOOST_REQUIRE(held != nullptr);

        wallet.reset();
        BOOST_CHECK(!observer.expired());
    }

    // Every strong reference is gone. The entry is still here, because nothing
    // removes it -- so it has to say the wallet is gone rather than hand back a
    // pointer to freed memory, which is exactly what it did before.
    BOOST_CHECK(observer.expired());
    BOOST_CHECK(entry.GetWallet() == nullptr);
}

// Enabling staking registers the pk() twin a descriptor wallet needs, because
// a coinstake pays vout[1] to pay-to-pubkey and a wallet tracking only pkh()
// does not recognise its own coinstake -- it books the staked amount and the
// reward as an outgoing send. The registration could not happen on a wallet
// whose private descriptors will not render, and it reported success anyway,
// so the switch went on and the wallet staked into exactly that state.
// Unlocking afterwards did not repair it: this runs on the off->on edge alone.
BOOST_AUTO_TEST_CASE(coinstake_descriptors_refuse_a_wallet_that_cannot_provide_them)
{
    // A watch-only wallet holds no key that could sign a coinstake at all.
    {
        std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                                    "pos-watchonly-test", CreateDummyWalletDatabase());
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS | WALLET_FLAG_DISABLE_PRIVATE_KEYS);
        BOOST_CHECK(!EnsureCoinstakeDescriptors(*wallet));
    }

    // An encrypted wallet that is locked renders no private descriptor, so the
    // loop finds nothing to mirror and used to call that success.
    {
        std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                                    "pos-locked-test", CreateDummyWalletDatabase());
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        {
            LOCK(wallet->cs_wallet);
            wallet->SetupDescriptorScriptPubKeyMans();
        }
        BOOST_REQUIRE(wallet->EncryptWallet("pos-descriptor-test"));
        BOOST_REQUIRE(wallet->Lock());
        BOOST_REQUIRE(wallet->IsLocked());

        BOOST_CHECK(!EnsureCoinstakeDescriptors(*wallet));

        // The guard refuses a state, not a wallet. Unlocked, the same wallet
        // must be served -- otherwise the fix would cost every encrypted wallet
        // its staking rather than making it safe.
        BOOST_REQUIRE(wallet->Unlock("pos-descriptor-test"));
        BOOST_CHECK(EnsureCoinstakeDescriptors(*wallet));
    }
}

BOOST_AUTO_TEST_SUITE_END()
