// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <masternode/sync.h>
#include <netfulfilledman.h>
#include <node/blockstorage.h>
#include <pow.h>
#include <primitives/block.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/mining.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <util/time.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <memory>

namespace {
constexpr int64_t TIMEOUT{MASTERNODE_SYNC_TIMEOUT_SECONDS};
constexpr int64_t RECENT{MASTERNODE_SYNC_RECENT_TIP_SECONDS};
constexpr int64_t START{1'700'000'000};

// A real regtest chain below the last proof-of-work block, and a masternode sync reading it.
struct MasternodeSyncSetup : public RegTestingSetup {
    // Switching the stage asserts a loaded fulfilled-request store.
    CNetFulfilledRequestManager netfulfilledman;
    std::unique_ptr<CMasternodeSync> mn_sync;

    MasternodeSyncSetup()
    {
        BOOST_REQUIRE(netfulfilledman.LoadCache(/*load_cache=*/false));
        mn_sync = std::make_unique<CMasternodeSync>(*m_node.connman, netfulfilledman, *m_node.chainman);
        mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    }
    ~MasternodeSyncSetup() { SetMockTime(std::chrono::seconds{0}); }

    // A block with the given time on the active tip, the clock set to that time. Up to the last
    // proof-of-work block the target is the proof-of-work limit, whatever the time.
    CBlockIndex* MineAt(int64_t time)
    {
        SetMockTime(std::chrono::seconds{time});
        auto block = PrepareBlock(m_node, CScript{} << OP_TRUE);
        block->nTime = time;
        while (!CheckProofOfWork(block->GetHash(), block->nBits, Params().GetConsensus())) ++block->nNonce;
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(Params(), block, /*force_processing=*/true, /*new_block=*/nullptr));
        CBlockIndex* tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip())};
        BOOST_REQUIRE_EQUAL(tip->GetBlockHash(), block->GetHash());
        return tip;
    }

    // The decision, under cs_main as TryEndBlockchainStage takes it.
    bool Done() { return WITH_LOCK(::cs_main, return mn_sync->IsBlockchainStageDone(TIMEOUT)); }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(masternode_sync_tests, MasternodeSyncSetup)

// Above the last proof-of-work block a header costs nothing, so the blockchain stage also ends while the
// active tip is recent, and headers do not hold that back; once the tip is older, that way is closed.
BOOST_AUTO_TEST_CASE(recent_active_tip_ends_blockchain_stage)
{
    CBlockIndex* tip{MineAt(START)};
    SetMockTime(std::chrono::seconds{START + 60});
    BOOST_CHECK(Done());

    // Headers, however many: they postpone only the rule from Dash, which has not been met here.
    mn_sync->AcceptedBlockHeader(tip);
    mn_sync->NotifyHeaderTip(tip, /*fInitialDownload=*/false);
    BOOST_CHECK(Done());

    // The same tip, older than the window.
    SetMockTime(std::chrono::seconds{tip->GetBlockTime() + RECENT});
    BOOST_CHECK(!Done());

    // A new block makes the active tip recent again.
    MineAt(START + RECENT + 60);
    BOOST_CHECK(Done());
}

// The active tip is read when the stage is decided, under cs_main, and nothing is kept from a notification.
// InvalidateBlock() -- by hand, or from InstantSend when a lock conflicts with a mined transaction --
// disconnects block after block and notifies only at its end; until then the queued notification still
// names the block it rolled back. The decision sees the older tip it has left.
BOOST_AUTO_TEST_CASE(blockchain_stage_reads_the_tip_after_a_rollback)
{
    CBlockIndex* older{MineAt(START)};
    CBlockIndex* recent{MineAt(START + RECENT + 600)};
    // The notification the node delivers for `recent`, with a best header elsewhere (a header-only branch), so
    // that the rule from Dash stays out of it.
    mn_sync->UpdatedBlockTip(/*pindexTip=*/older, /*pindexNew=*/recent, /*fInitialDownload=*/false);
    SetMockTime(std::chrono::seconds{recent->GetBlockTime() + 60});
    BOOST_REQUIRE(Done());

    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, recent));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()), older);
    // `older` is RECENT + 660 s old: the stage holds, and the decision does not end it.
    BOOST_CHECK(!Done());
    BOOST_CHECK(!mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_BLOCKCHAIN);
}

// During initial block download a recent tip does not end the stage: importing blocks counts as such,
// whatever the tip's time.
BOOST_AUTO_TEST_CASE(no_recent_tip_during_initial_block_download)
{
    MineAt(START);
    SetMockTime(std::chrono::seconds{START + 60});
    BOOST_REQUIRE(Done());

    fImporting = true;
    static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).ResetIbd();
    const bool done_while_importing{Done()};
    fImporting = false;
    BOOST_CHECK(!done_while_importing);
    BOOST_CHECK(Done());
}

// For an older tip the rule from Dash is all there is, unchanged: with the best header reached, the stage
// ends once nothing has arrived for the timeout, and a header postpones that.
BOOST_AUTO_TEST_CASE(dash_rule_unchanged_for_an_older_tip)
{
    CBlockIndex* tip{MineAt(START)};
    SetMockTime(std::chrono::seconds{START + RECENT + 60});
    mn_sync->UpdatedBlockTip(/*pindexTip=*/tip, /*pindexNew=*/tip, /*fInitialDownload=*/false);
    BOOST_CHECK(!Done());
    SetMockTime(std::chrono::seconds{START + RECENT + 60 + TIMEOUT + 1});
    BOOST_CHECK(Done());

    mn_sync->AcceptedBlockHeader(tip);
    BOOST_CHECK(!Done());
    SetMockTime(std::chrono::seconds{START + RECENT + 60 + 2 * TIMEOUT + 2});
    BOOST_CHECK(Done());
}

// Leaving the blockchain stage is one compare-and-swap under cs_main, one stage forward. If `mnsync next`
// has already moved the stage, the decision does nothing: it does not move the stage again, which would
// run the governance stage's work (it takes network locks) under cs_main.
BOOST_AUTO_TEST_CASE(blockchain_stage_ends_in_one_step)
{
    MineAt(START);
    SetMockTime(std::chrono::seconds{START + 60});
    BOOST_REQUIRE(Done());

    // `mnsync next` first: the stage is governance, and the decision leaves it there.
    mn_sync->SwitchToNextAsset();
    BOOST_REQUIRE_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);
    BOOST_CHECK(!mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);

    // Otherwise the decision moves exactly one stage, to governance.
    mn_sync->Reset(/*fForce=*/true, /*fNotifyReset=*/false);
    BOOST_CHECK(mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);
    BOOST_CHECK(!mn_sync->TryEndBlockchainStage(TIMEOUT));
    BOOST_CHECK_EQUAL(mn_sync->GetAssetID(), MASTERNODE_SYNC_GOVERNANCE);
}

BOOST_AUTO_TEST_SUITE_END()
