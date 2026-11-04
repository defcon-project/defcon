// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <context.h>
#include <interfaces/wallet.h>
#include <pos/minter.h>
#include <pos/stake.h>
#include <pos/multiwallet.h>
#include <psbt.h>
#include <rpc/server.h>
#include <script/standard.h>
#include <univalue.h>
#include <util/translation.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>

extern RecursiveMutex stakable_mutex;

RPCHelpMan listdescriptors();

namespace {
UniValue CallStakingRPC(NodeContext& node, const char* method)
{
    JSONRPCRequest request;
    request.context = CoreContext{node};
    request.strMethod = method;
    request.params.setArray();
    return tableRPC.execute(request);
}

// Hold only the wallet lock, on a separate thread so the test can probe the
// registry without introducing a reverse lock order on its own thread.
class BusyWallet {
    std::promise<void> m_locked;
    std::promise<void> m_release;
    std::future<void> m_thread;
public:
    explicit BusyWallet(CWallet& wallet) : m_thread(std::async(std::launch::async, [&] {
        LOCK(wallet.cs_wallet);
        m_locked.set_value();
        m_release.get_future().wait();
    }))
    {
        m_locked.get_future().wait();
    }
    ~BusyWallet() { m_release.set_value(); m_thread.wait(); }
};

constexpr const char* PASSPHRASE{"pos-mixingonly-test"};

// An encrypted descriptor wallet, locked, and an unsigned spend of a coin it owns, as a PSBT that
// carries the coin.
struct EncryptedWalletWithCoin {
    std::shared_ptr<CWallet> wallet;
    PartiallySignedTransaction psbt;
};

EncryptedWalletWithCoin MakeEncryptedWalletWithCoin(NodeContext& node, const std::string& name)
{
    auto wallet = std::make_shared<CWallet>(node.chain.get(), node.coinjoin_loader.get(), name, CreateDummyWalletDatabase());
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    {
        LOCK(wallet->cs_wallet);
        wallet->SetupDescriptorScriptPubKeyMans();
    }
    CTxDestination dest;
    bilingual_str error;
    BOOST_REQUIRE(wallet->GetNewDestination("", dest, error));
    BOOST_REQUIRE(wallet->EncryptWallet(PASSPHRASE));
    BOOST_REQUIRE(wallet->Lock());

    CMutableTransaction funding;
    funding.vin.emplace_back(COutPoint(uint256::ONE, 0));
    funding.vout.emplace_back(COIN, GetScriptForDestination(dest));
    const CTransactionRef funding_tx = MakeTransactionRef(funding);

    CMutableTransaction spend;
    spend.vin.emplace_back(COutPoint(funding_tx->GetHash(), 0));
    spend.vout.emplace_back(COIN / 2, GetScriptForDestination(dest));
    PartiallySignedTransaction psbt(spend);
    psbt.inputs[0].non_witness_utxo = funding_tx;
    return {wallet, psbt};
}

bool Signed(const PartiallySignedTransaction& psbt)
{
    return !psbt.inputs[0].final_script_sig.empty() || !psbt.inputs[0].partial_sigs.empty();
}

// Takes the wallet lock on a thread of its own, waits until the test has started a call that needs
// that lock, changes the wallet's lock mode while still holding it, then lets go: the waiting call
// sees the wallet only after the change. The pause gives that call time to reach the lock; on the
// fixed code the outcome does not depend on it. No Boost check runs off the test thread.
class ChangeWhileHeld {
    std::promise<void> m_locked;
    std::promise<void> m_go;
    std::future<bool> m_thread;
public:
    ChangeWhileHeld(CWallet& wallet, std::function<bool()> change)
        : m_thread(std::async(std::launch::async, [this, &wallet, change] {
              LOCK(wallet.cs_wallet);
              m_locked.set_value();
              m_go.get_future().wait();
              return change();
          }))
    {
        m_locked.get_future().wait();
    }
    bool Go()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        m_go.set_value();
        return m_thread.get();
    }
    ~ChangeWhileHeld()
    {
        if (m_thread.valid()) {
            m_go.set_value();
            m_thread.wait();
        }
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(pos_multiwallet_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(staking_rpc_does_not_hold_registry_while_waiting_for_wallet)
{
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                           "pos-rpc-lock-test", CreateDummyWalletDatabase());
    AddWallet(wallet);
    struct Cleanup {
        std::shared_ptr<CWallet>& wallet;
        ~Cleanup() { RemoveWallet(wallet, std::nullopt); MultiwalletInitialize(); }
    } cleanup{wallet};
    MultiwalletInitialize();

    for (const char* method : {"getstakinginfo", "liststakingwallets"}) {
        BOOST_REQUIRE(ToggleWalletStaking(wallet->GetName()));
        const auto owners = wallet.use_count();
        std::future<UniValue> result;
        {
            BusyWallet busy(*wallet);
            result = std::async(std::launch::async, [&] { return CallStakingRPC(m_node, method); });
            // Both the old and new RPC implementations acquire a strong wallet
            // reference before asking for its balance. Wait for that reference,
            // not an arbitrary sleep, to know the RPC has reached this wallet.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (wallet.use_count() <= owners && std::chrono::steady_clock::now() < deadline &&
                   result.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            BOOST_CHECK_GT(wallet.use_count(), owners);
            TRY_LOCK(stakable_mutex, registry);
            BOOST_CHECK_MESSAGE(bool(registry), method << " kept the registry locked while waiting for a wallet");
            if (registry) {
                BOOST_CHECK(!ToggleWalletStaking(wallet->GetName()));
                MultiwalletMaintenance();
            }
        }
        const UniValue response = result.get();
        BOOST_REQUIRE_EQUAL(response.size(), 1U);
        BOOST_CHECK_EQUAL(response["0"]["name"].get_str(), wallet->GetName());
        const char* enabled = std::string{method} == "getstakinginfo" ? "staking" : "enabled";
        // This response describes the list captured before the concurrent
        // toggle. The next call must see the updated switch.
        BOOST_CHECK(response["0"][enabled].get_bool());
        BOOST_CHECK(!CallStakingRPC(m_node, method)["0"][enabled].get_bool());
        if (IsWalletStaking(wallet->GetName())) ToggleWalletStaking(wallet->GetName());
    }
}

BOOST_AUTO_TEST_CASE(staking_rpc_skips_expired_registry_entries)
{
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    auto gone = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                         "pos-rpc-gone", CreateDummyWalletDatabase());
    auto live = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                         "pos-rpc-live", CreateDummyWalletDatabase());
    AddWallet(gone);
    AddWallet(live);
    struct Cleanup {
        std::shared_ptr<CWallet>& gone;
        std::shared_ptr<CWallet>& live;
        ~Cleanup() {
            if (gone) RemoveWallet(gone, std::nullopt);
            RemoveWallet(live, std::nullopt);
            MultiwalletInitialize();
        }
    } cleanup{gone, live};
    MultiwalletInitialize();
    RemoveWallet(gone, std::nullopt);
    std::weak_ptr<CWallet> observer = gone;
    gone.reset();
    BOOST_REQUIRE(observer.expired());

    // Maintenance has not run yet. The stale entry must be skipped without
    // renumbering the surviving wallet within this snapshot.
    for (const char* method : {"getstakinginfo", "liststakingwallets"}) {
        const UniValue response = CallStakingRPC(m_node, method);
        BOOST_REQUIRE_EQUAL(response.size(), 1U);
        BOOST_CHECK_EQUAL(response["1"]["name"].get_str(), live->GetName());
    }
    MultiwalletMaintenance();
    for (const char* method : {"getstakinginfo", "liststakingwallets"}) {
        const UniValue response = CallStakingRPC(m_node, method);
        BOOST_REQUIRE_EQUAL(response.size(), 1U);
        BOOST_CHECK_EQUAL(response["0"]["name"].get_str(), live->GetName());
    }
}

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

// last_error is never cleared, on purpose: a fault that recurs has to stay
// visible instead of vanishing between failures. What makes that readable rather
// than permanently alarming is comparing it against when the current run began --
// an error stamped before that has already been survived.
//
// The safety-critical half of the rule is this one. If the timestamp were left
// behind when the minter is NOT running, a reader would compare a live failure
// against a stale start time and conclude the node had recovered from the very
// fault that stopped it. Zero cannot be greater than any error time, so the
// comparison fails closed.
BOOST_AUTO_TEST_CASE(minter_running_since_is_zero_while_stopped)
{
    BOOST_CHECK_EQUAL(MinterRunningSince(), 0);

    SetMinterLastError("a failure with the minter stopped");
    BOOST_CHECK(MinterLastErrorTime() > 0);
    BOOST_CHECK(MinterRunningSince() < MinterLastErrorTime());

    SetMinterLastError("");
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

    // Unlocked for mixing only, the wallet still counts as locked for sending,
    // but its keys are there and its private descriptors render. That is the
    // unlock a staking wallet is meant to run under, so it must be served.
    {
        std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(),
                                                                    "pos-mixingonly-test", CreateDummyWalletDatabase());
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        {
            LOCK(wallet->cs_wallet);
            wallet->SetupDescriptorScriptPubKeyMans();
        }
        BOOST_REQUIRE(wallet->EncryptWallet("pos-descriptor-test"));
        BOOST_REQUIRE(wallet->Lock());
        BOOST_REQUIRE(wallet->Unlock("pos-descriptor-test", /*fForMixingOnly=*/true));
        BOOST_REQUIRE(wallet->IsLocked());
        BOOST_REQUIRE(!wallet->IsLocked(/*fForMixing=*/true));

        BOOST_CHECK(EnsureCoinstakeDescriptors(*wallet));
    }
}

// A wallet unlocked for mixing only holds its keys, so whatever asks it to sign a spend -- the send
// RPC, walletprocesspsbt, the desktop wallet's PSBT and multisig windows after their unlock prompt
// was cancelled -- gets the transaction back filled in but unsigned. The windows sign through
// interfaces::Wallet::fillPSBT, as here.
BOOST_AUTO_TEST_CASE(mixing_only_unlock_signs_no_psbt)
{
    const EncryptedWalletWithCoin setup = MakeEncryptedWalletWithCoin(m_node, "pos-fillpsbt-test");
    CWallet& wallet = *setup.wallet;
    const std::unique_ptr<interfaces::Wallet> iwallet = interfaces::MakeWallet(setup.wallet);
    const auto fill_and_sign = [&] {
        PartiallySignedTransaction psbt{setup.psbt};
        bool complete{true};
        BOOST_CHECK(iwallet->fillPSBT(SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/false, /*n_signed=*/nullptr, psbt, complete) == TransactionError::OK);
        BOOST_CHECK_EQUAL(complete, Signed(psbt));
        return Signed(psbt);
    };

    BOOST_CHECK(!fill_and_sign()); // locked

    BOOST_REQUIRE(wallet.Unlock(PASSPHRASE, /*fForMixingOnly=*/true));
    BOOST_CHECK(!fill_and_sign()); // unlocked for mixing only

    BOOST_REQUIRE(wallet.Lock());
    BOOST_REQUIRE(wallet.Unlock(PASSPHRASE));
    BOOST_CHECK(fill_and_sign()); // fully unlocked: signed, so the checks above can tell
}

// The signing guard reads the mode under the wallet lock, which unlocking takes too. The wallet is
// locked when the call starts and is unlocked for mixing only while the call waits for that lock:
// the call signs nothing.
BOOST_AUTO_TEST_CASE(fillpsbt_reads_the_mode_under_the_wallet_lock)
{
    const EncryptedWalletWithCoin setup = MakeEncryptedWalletWithCoin(m_node, "pos-fillpsbt-race-test");
    CWallet& wallet = *setup.wallet;

    ChangeWhileHeld change(wallet, [&] { return wallet.Unlock(PASSPHRASE, /*fForMixingOnly=*/true); });
    auto call = std::async(std::launch::async, [&] {
        PartiallySignedTransaction psbt{setup.psbt};
        bool complete{true};
        const TransactionError err = wallet.FillPSBT(psbt, complete, SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/false);
        return err == TransactionError::OK && !complete && !Signed(psbt);
    });
    BOOST_REQUIRE(change.Go());
    BOOST_CHECK(call.get());
    BOOST_CHECK(wallet.IsLocked());
    BOOST_CHECK(!wallet.IsLocked(/*fForMixing=*/true));
}

// The strict lock check reads the mode and the key together, under the wallet lock. The wallet is
// fully unlocked when the check starts, and is locked and unlocked for mixing only while the check
// waits for that lock: it reports the wallet locked. Read apart, it saw the old mode and the new key,
// and called a wallet unlocked for mixing only fully unlocked.
BOOST_AUTO_TEST_CASE(strict_lock_check_reads_mode_and_key_together)
{
    const EncryptedWalletWithCoin setup = MakeEncryptedWalletWithCoin(m_node, "pos-strict-check-race-test");
    CWallet& wallet = *setup.wallet;
    BOOST_REQUIRE(wallet.Unlock(PASSPHRASE));
    BOOST_REQUIRE(!wallet.IsLocked());

    ChangeWhileHeld change(wallet, [&] { return wallet.Lock() && wallet.Unlock(PASSPHRASE, /*fForMixingOnly=*/true); });
    auto check = std::async(std::launch::async, [&] { return wallet.IsLocked(); });
    BOOST_REQUIRE(change.Go());
    BOOST_CHECK(check.get());
    BOOST_CHECK(!wallet.IsLocked(/*fForMixing=*/true));
}

// A strict lock check reads the mode and the key together, under the wallet lock, and the private
// export it guards runs under the same lock. The wallet is fully unlocked when listdescriptors
// starts, and is locked and unlocked for mixing only while the call waits for that lock: the call
// refuses. Read apart, the check saw the old mode and the new key, and the keys went out.
BOOST_AUTO_TEST_CASE(private_descriptor_export_reads_the_mode_under_the_wallet_lock)
{
    const EncryptedWalletWithCoin setup = MakeEncryptedWalletWithCoin(m_node, "pos-listdescriptors-race-test");
    CWallet& wallet = *setup.wallet;
    BOOST_REQUIRE(wallet.Unlock(PASSPHRASE));
    AddWallet(setup.wallet);

    JSONRPCRequest request;
    request.context = CoreContext{m_node};
    request.URI = "/wallet/" + wallet.GetName();
    request.params.setArray();
    request.params.push_back(true);

    {
        ChangeWhileHeld change(wallet, [&] { return wallet.Lock() && wallet.Unlock(PASSPHRASE, /*fForMixingOnly=*/true); });
        auto call = std::async(std::launch::async, [&]() -> std::string {
            try {
                ::listdescriptors().HandleRequest(request);
                return "exported";
            } catch (const UniValue& error) {
                return find_value(error, "message").get_str();
            }
        });
        BOOST_REQUIRE(change.Go());
        BOOST_CHECK_EQUAL(call.get(), "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }
    BOOST_CHECK(wallet.IsLocked());
    BOOST_CHECK(!wallet.IsLocked(/*fForMixing=*/true));
    RemoveWallet(setup.wallet, std::nullopt);
}

// The coinstake descriptors are collected under the wallet lock the locked check reads under. The
// wallet is unlocked for mixing only when the call starts and is fully locked while the call waits
// for that lock: the call refuses and registers nothing, rather than reporting success with no
// descriptor registered.
BOOST_AUTO_TEST_CASE(coinstake_descriptors_read_the_lock_under_the_wallet_lock)
{
    const EncryptedWalletWithCoin setup = MakeEncryptedWalletWithCoin(m_node, "pos-descriptor-race-test");
    CWallet& wallet = *setup.wallet;
    BOOST_REQUIRE(wallet.Unlock(PASSPHRASE, /*fForMixingOnly=*/true));
    const size_t managers{WITH_LOCK(wallet.cs_wallet, return wallet.GetAllScriptPubKeyMans().size())};

    ChangeWhileHeld change(wallet, [&] { return wallet.Lock(); });
    auto call = std::async(std::launch::async, [&] { return EnsureCoinstakeDescriptors(wallet); });
    BOOST_REQUIRE(change.Go());
    BOOST_CHECK(!call.get());
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet.cs_wallet, return wallet.GetAllScriptPubKeyMans().size()), managers);
}

BOOST_AUTO_TEST_SUITE_END()
