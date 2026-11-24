// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/chainstate.h>

#include <chainparamsbase.h>
#include <consensus/params.h>
#include <dbwrapper.h>
#include <deploymentstatus.h>
#include <node/blockstorage.h>
#include <validation.h>

#include <limits>

#include <bls/bls.h>
#include <coins.h>
#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <evo/mnhftx.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>

//! Rebuild the evodb entries for every block between where the evodb stopped and
//! the chain tip the coins database was loaded at.
//!
//! The evodb reaches disk only in CEvoDB::CommitRootTransaction, so a process that
//! dies during a long import leaves the coins database at its tip and the evodb
//! wherever its last commit left it -- at nothing at all, if -reindex wiped it and
//! no commit followed. MigrateDBIfNeeded then reads the absent marker as an
//! interrupted migration and refuses to start, which is why this runs before it.
//!
//! Only three of the four evodb writers need replaying: CCreditPoolManager
//! memoises a derivation it recomputes whenever the entry is absent.
//!
//! The per-transaction checks of CSpecialTxProcessor::ProcessSpecialTxsInBlock are
//! deliberately skipped: these blocks were fully validated when they were first
//! connected, and CheckProRegTx cannot run here under any view. It reads the coins
//! view without the guard BuildNewListFromBlock has, so a dummy view rejects every
//! external-collateral registration outright and the live tip view rejects every
//! collateral spent since. CDeterministicMNManager::RecalculateAndRepairDiffs takes
//! the same route past the same obstacle, and for the same reason.
//!
//! Rolling the coins back instead is not available: DisconnectBlock requires the
//! evodb to sit exactly at the block it dismantles, so a lagging evodb aborts on the
//! first step.
static bool ReconcileEvoDBToTip(ChainstateManager& chainman,
                                CEvoDB& evodb,
                                CDeterministicMNManager& dmnman,
                                CMNHFManager& mnhfman,
                                LLMQContext& llmq_ctx,
                                const Consensus::Params& consensus_params) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    AssertLockHeld(cs_main);

    const CBlockIndex* tip = chainman.ActiveChain().Tip();
    if (tip == nullptr) return true;

    const CBlockIndex* evo_index{nullptr};
    uint256 evo_best;
    if (evodb.Read(EVODB_BEST_BLOCK, evo_best)) {
        evo_index = chainman.m_blockman.LookupBlockIndex(evo_best);
        if (evo_index == nullptr) {
            LogPrintf("%s -- evodb best block %s is not in the block index\n", __func__, evo_best.ToString());
            return false;
        }
        if (evo_index == tip) {
            // The healthy case, and the only path through this function that
            // said nothing at all. Silence here is indistinguishable in the
            // journal from a binary that predates the reconciliation, which is
            // the one question an operator reading a startup log has to answer.
            LogPrintf("%s -- evodb is at the chain tip (height %d); nothing to reconcile\n", __func__,
                      tip->nHeight);
            return true;
        }
        if (!chainman.ActiveChain().Contains(evo_index)) {
            LogPrintf("%s -- evodb best block %s is not on the active chain\n", __func__, evo_best.ToString());
            return false;
        }
    } else {
        // No marker under the CURRENT best-block constant does NOT mean the database
        // is empty. Every generation before this one wrote its best block under its
        // own key, and normal operation rewrites only the newest, so a database that
        // has not been migrated yet carries an older marker and a full set of entries.
        // That is not a hypothetical: the b_b6 bump shipped in no released tag, so
        // every mainnet masternode's evodb is still of the b_b4 generation.
        //
        // Replaying such a database is not merely unnecessary, it is fatal. The quorum
        // commitment writer is NOT idempotent: CQuorumBlockProcessor::ProcessBlock
        // rejects a commitment already stored for that height with bad-qc-not-allowed
        // (blockprocessor.cpp:195-200, through GetNumCommitmentsRequired, which returns
        // 0 once HasMinedCommitment is true). The replay therefore walks into its own
        // earlier, correct entry, and the node refuses to start -- strictly worse than
        // the state this function exists to mend, because the migration gates would
        // have upgraded that same database in seconds.
        //
        // So only a genuinely empty evodb may be replayed from the DIP3 height.
        // Anything else is left to the migration gates, exactly as it was before this
        // function existed. The marker list matches
        // CDeterministicMNManager::MigrationAlreadyDone (deterministicmns.cpp:1341).
        for (const std::string& older_marker : {std::string{"b_b3"}, std::string{"b_b4"}, std::string{"b_b5"}}) {
            if (evodb.GetRawDB().Exists(older_marker)) {
                LogPrintf("%s -- evodb carries the older marker %s and no %s: this database is not migrated, "
                          "not stranded; leaving it to the migration gates\n",
                          __func__, older_marker, EVODB_BEST_BLOCK);
                return true;
            }
        }
        if (!evodb.IsEmpty()) {
            LogPrintf("%s -- evodb has no %s marker but is not empty; not replaying, because the quorum "
                      "commitment writer would reject entries that are already on disk\n",
                      __func__, EVODB_BEST_BLOCK);
            return true;
        }
    }

    // With no marker at all the evodb holds nothing, so the replay starts where the
    // deterministic masternode list itself starts. Blocks below that height return
    // early from every processor called here.
    const int start_height = evo_index != nullptr ? evo_index->nHeight + 1
                                                  : std::max(1, consensus_params.DIP0003Height);
    if (start_height > tip->nHeight) {
        // Only a marker can put the evodb genuinely ahead of the coins tip. That is a
        // state this replay cannot mend, because it only moves forward.
        if (evo_index != nullptr) {
            LogPrintf("%s -- evodb is ahead of the chain tip (evodb %d, tip %d); this is not a lag and cannot be replayed forward\n",
                      __func__, start_height - 1, tip->nHeight);
            return false;
        }
        // Empty evodb and a tip below DIP0003: there is no deterministic masternode
        // list yet, so the evodb is not lagging -- it is current, and empty is the
        // correct content. Record that, exactly as MigrateDBIfNeeded does when it
        // reaches the same conclusion (deterministicmns.cpp:1392-1397).
        //
        // Returning without the marker is not enough, and this was measured: the
        // node still refuses to start. On an empty database MigrationAlreadyDone is
        // false, so the gate never reaches its own below-DIP3 branch; it falls
        // through to "b_b2 is gone, therefore a previous migration was interrupted"
        // (:1386) and returns false, which AppInit reports as "Error upgrading Evo
        // database". Writing the marker here lets all four gates take their
        // "already done" path, which is the truth for this database.
        {
            auto dbTx = evodb.BeginTransaction();
            evodb.WriteBestBlock(tip->GetBlockHash());
            dbTx->Commit();
        }
        if (!evodb.CommitRootTransaction()) {
            LogPrintf("%s -- failed to commit the below-DIP0003 marker\n", __func__);
            return false;
        }
        LogPrintf("%s -- evodb is empty and the tip (%d) is below DIP0003 (%d); marked it current, nothing to replay\n",
                  __func__, tip->nHeight, consensus_params.DIP0003Height);
        return true;
    }

    LogPrintf("%s -- evodb stopped at height %d, chain tip is %d; replaying %d block(s)\n",
              __func__, start_height - 1, tip->nHeight, tip->nHeight - start_height + 1);

    // A failure below is not atomic on the evodb. Whatever this replay committed into
    // the root transaction before the failure is written out by the shutdown flush,
    // without a marker. It is harmless today -- the migration gates overwrite the same
    // heights, measured -- but it is stated here because the premise one layer up is
    // "no marker means nothing on disk", and this path can falsify it.
    //
    // Match the repair path exactly: a dummy view, so no historical collateral is
    // looked up in a UTXO set that has moved past it.
    CCoinsView view_dummy;
    CCoinsViewCache view(&view_dummy);

    // The BLS scheme is a global that the connect path maintains, and this replay
    // bypasses that path: it calls the three writers directly rather than going
    // through CSpecialTxProcessor. CMNHFManager::ProcessBlock verifies EHF signal
    // signatures against bls::bls_legacy_scheme, so a chain carrying an EHF signal
    // is rejected here as bad-mnhf-invalid unless the flag holds what the original
    // connection left. VerifyLoadedChainstate sets it from the tip, but that runs
    // after LoadChainstate -- too late for this. Seed it from the block before the
    // replay starts, then maintain it in the loop exactly as the connect path does.
    const CBlockIndex* before_start = chainman.ActiveChain()[start_height - 1];
    const bool v19_before_start{before_start != nullptr &&
                                DeploymentActiveAfter(before_start, consensus_params,
                                                      Consensus::DEPLOYMENT_V19)};
    bls::bls_legacy_scheme.store(!v19_before_start);
    LogPrintf("%s -- bls_legacy_scheme=%d at the start of the replay\n", __func__,
              bls::bls_legacy_scheme.load());

    // Bound the replay by the same block count a full flush is bounded by. This
    // runs outside FlushStateToDisk, so that trigger cannot reach it, and an
    // unbounded replay would accumulate in one root transaction and write it in a
    // single commit -- rebuilding exactly the batch whose write hung for 45 minutes
    // in the field. Committing the marker with it also makes the replay resumable:
    // a process that dies mid-replay restarts from the intermediate marker rather
    // than from the DIP3 height.
    int64_t blocks_since_commit{0};

    for (int height = start_height; height <= tip->nHeight; ++height) {
        const CBlockIndex* pindex = chainman.ActiveChain()[height];
        assert(pindex != nullptr);

        CBlock block;
        if (!ReadBlockFromDisk(block, pindex, consensus_params)) {
            LogPrintf("%s -- failed to read the block at height %d\n", __func__, height);
            return false;
        }

        // One transaction per block, as ConnectTip does; the writes below land in the
        // cur transaction and CommitRootTransaction asserts that it is clean.
        auto dbTx = evodb.BeginTransaction();

        BlockValidationState state;
        // The order the special transaction processor uses.
        if (!llmq_ctx.quorum_block_processor->ProcessBlock(block, pindex, state, /*fJustCheck=*/false,
                                                           /*fBLSChecks=*/false)) {
            LogPrintf("%s -- quorum commitments failed at height %d: %s\n", __func__, height, state.ToString());
            return false;
        }
        std::optional<MNListUpdates> updates{std::nullopt};
        if (!dmnman.ProcessBlock(block, pindex, state, view, /*fJustCheck=*/false, updates)) {
            LogPrintf("%s -- masternode list failed at height %d: %s\n", __func__, height, state.ToString());
            return false;
        }
        if (!mnhfman.ProcessBlock(block, pindex, /*fJustCheck=*/false, state)) {
            LogPrintf("%s -- EHF signals failed at height %d: %s\n", __func__, height, state.ToString());
            return false;
        }

        // The same switch CSpecialTxProcessor::ProcessSpecialTxsInBlock makes at
        // the end of every block, and in the same position.
        if (DeploymentActiveAfter(pindex, consensus_params, Consensus::DEPLOYMENT_V19) &&
            bls::bls_legacy_scheme.load()) {
            bls::bls_legacy_scheme.store(false);
            LogPrintf("%s -- bls_legacy_scheme=%d at height %d\n", __func__,
                      bls::bls_legacy_scheme.load(), height);
        }

        dbTx->Commit();

        // Land the work so far, marker included, so the batch stays bounded and a
        // death here costs the last interval rather than the whole replay.
        if (++blocks_since_commit >= DATABASE_FLUSH_BLOCK_INTERVAL && height < tip->nHeight) {
            {
                auto markerTx = evodb.BeginTransaction();
                evodb.WriteBestBlock(pindex->GetBlockHash());
                markerTx->Commit();
            }
            if (!evodb.CommitRootTransaction()) {
                LogPrintf("%s -- failed to commit the replay at height %d\n", __func__, height);
                return false;
            }
            LogPrintf("%s -- committed the replay up to height %d\n", __func__, height);
            blocks_since_commit = 0;
        }
    }

    {
        auto dbTx = evodb.BeginTransaction();
        evodb.WriteBestBlock(tip->GetBlockHash());
        dbTx->Commit();
    }
    if (!evodb.CommitRootTransaction()) {
        LogPrintf("%s -- failed to commit the rebuilt evodb\n", __func__);
        return false;
    }

    LogPrintf("%s -- evodb reconciled to height %d\n", __func__, tip->nHeight);
    return true;
}

/** Whether the block index holds a block at or above the v23.1 activation height as refused while no block
 *  there carries the mark of this version (BLOCK_V231_RULES).
 *
 *  A version from before v23.1 refuses the network's blocks from the height on and stores each refusal. Taken
 *  over, such a refusal keeps the network's chain out: the headers built on the block are refused
 *  (bad-prevblk) and their senders punished, and the startup takes up one refused branch again at most. This
 *  version cannot tell such a refusal from one of its own by the entry, but it can by the index: once it has
 *  connected a block at or above the height, it has started on an index that held no refusal there, and
 *  every refusal stored since is its own. */
static bool HoldsRefusalFromBeforeV231(const BlockMap& block_map, const Consensus::Params& consensus_params)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const int activation_height{consensus_params.nV231ActivationHeight};
    if (activation_height == std::numeric_limits<int>::max()) return false; // unset: nothing to read
    const CBlockIndex* refused{nullptr};
    for (const auto& [_, block_index] : block_map) {
        if (block_index.nHeight < activation_height) continue;
        if (block_index.nStatus & BLOCK_V231_RULES) return false;
        if (refused == nullptr && (block_index.nStatus & BLOCK_FAILED_VALID)) refused = &block_index;
    }
    if (refused == nullptr) return false;
    LogPrintf("ERROR: The block index holds block %s at height %d, at or above the v23.1 activation height %d, "
              "as refused, and no block there that this version connected: the refusal may be an earlier "
              "version's. Restart with -reindex.\n",
              refused->GetBlockHash().ToString(), refused->nHeight, activation_height);
    return true;
}

std::optional<ChainstateLoadingError> LoadChainstate(bool fReset,
                                                     ChainstateManager& chainman,
                                                     CGovernanceManager& govman,
                                                     CMasternodeMetaMan& mn_metaman,
                                                     CMasternodeSync& mn_sync,
                                                     CSporkManager& sporkman,
                                                     std::unique_ptr<CActiveMasternodeManager>& mn_activeman,
                                                     std::unique_ptr<CChainstateHelper>& chain_helper,
                                                     std::unique_ptr<CCreditPoolManager>& cpoolman,
                                                     std::unique_ptr<CDeterministicMNManager>& dmnman,
                                                     std::unique_ptr<CEvoDB>& evodb,
                                                     std::unique_ptr<CMNHFManager>& mnhf_manager,
                                                     std::unique_ptr<LLMQContext>& llmq_ctx,
                                                     CTxMemPool* mempool,
                                                     bool fPruneMode,
                                                     bool is_addrindex_enabled,
                                                     bool is_governance_enabled,
                                                     bool is_spentindex_enabled,
                                                     bool is_timeindex_enabled,
                                                     bool is_txindex_enabled,
                                                     const Consensus::Params& consensus_params,
                                                     const std::string& network_id,
                                                     bool fReindexChainState,
                                                     int64_t nBlockTreeDBCache,
                                                     int64_t nCoinDBCache,
                                                     int64_t nCoinCacheUsage,
                                                     bool block_tree_db_in_memory,
                                                     bool coins_db_in_memory,
                                                     std::function<bool()> shutdown_requested,
                                                     std::function<void()> coins_error_cb,
                                                     bool evodb_repair,
                                                     bool force_evodb_repair)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    bool block_tree_open{false};
    if (fReindexChainState && !fReset && consensus_params.nV231ActivationHeight != std::numeric_limits<int>::max()) {
        // -reindex-chainstate keeps the block index and wipes the databases it rebuilds as they are opened
        // below. Whether the kept index can be built on has to be known before that, so its entries are read
        // here, into a map of their own: a rebuild on an index that holds an earlier version's refusal at or
        // above the v23.1 height would connect a block there, and its mark would hide the refusal from the
        // next start. The database is opened here once, as it is below, and stays open for the load.
        auto& block_tree{chainman.m_blockman.m_block_tree_db};
        block_tree.reset(); // a handle left by an earlier pass of the caller's loop
        block_tree.reset(new CBlockTreeDB(nBlockTreeDBCache, block_tree_db_in_memory, false));
        block_tree_open = true;
        BlockMap kept;
        const auto insert = [&kept](const uint256& hash) -> CBlockIndex* {
            if (hash.IsNull()) return nullptr;
            const auto [it, inserted]{kept.try_emplace(hash)};
            if (inserted) it->second.phashBlock = &it->first;
            return &it->second;
        };
        if (!block_tree->LoadBlockIndexGuts(consensus_params, insert)) {
            if (shutdown_requested && shutdown_requested()) return ChainstateLoadingError::SHUTDOWN_PROBED;
            return ChainstateLoadingError::ERROR_LOADING_BLOCK_DB;
        }
        if (HoldsRefusalFromBeforeV231(kept, consensus_params)) {
            return ChainstateLoadingError::ERROR_V231_REFUSALS_NEED_REVALIDATION;
        }
    }

    int64_t nEvoDbCache{64 * 1024 * 1024}; // TODO
    evodb.reset();
    evodb = std::make_unique<CEvoDB>(nEvoDbCache, false, fReset || fReindexChainState);

    mnhf_manager.reset();
    mnhf_manager = std::make_unique<CMNHFManager>(*evodb);

    chainman.InitializeChainstate(mempool, *evodb, chain_helper);
    chainman.m_total_coinstip_cache = nCoinCacheUsage;
    chainman.m_total_coinsdb_cache = nCoinDBCache;

    auto& pblocktree{chainman.m_blockman.m_block_tree_db};
    if (!block_tree_open) { // else opened above, with these arguments (fReset is false there)
        // new CBlockTreeDB tries to delete the existing file, which
        // fails if it's still open from the previous loop. Close it first:
        pblocktree.reset();
        pblocktree.reset(new CBlockTreeDB(nBlockTreeDBCache, block_tree_db_in_memory, fReset));
    }

    DashChainstateSetup(chainman, govman, mn_metaman, mn_sync, sporkman, mn_activeman, chain_helper, cpoolman,
                        dmnman, evodb, mnhf_manager, llmq_ctx, mempool, fReset, fReindexChainState,
                        consensus_params);

    if (fReset) {
        pblocktree->WriteReindexing(true);
        //If we're reindexing in prune mode, wipe away unusable block files and all undo data files
        if (fPruneMode)
            CleanupBlockRevFiles();
    }

    if (shutdown_requested && shutdown_requested()) return ChainstateLoadingError::SHUTDOWN_PROBED;

    // LoadBlockIndex will load m_have_pruned if we've ever removed a
    // block file from disk.
    // Note that it also sets fReindex based on the disk flag!
    // From here on out fReindex and fReset mean something different!
    if (!chainman.LoadBlockIndex()) {
        if (shutdown_requested && shutdown_requested()) return ChainstateLoadingError::SHUTDOWN_PROBED;
        return ChainstateLoadingError::ERROR_LOADING_BLOCK_DB;
    }

    // TODO: Remove this when pruning is fixed.
    // See https://github.com/dashpay/dash/pull/1817 and https://github.com/dashpay/dash/pull/1743
    if (is_governance_enabled && !is_txindex_enabled && network_id != CBaseChainParams::REGTEST) {
        return ChainstateLoadingError::ERROR_TXINDEX_DISABLED_WHEN_GOV_ENABLED;
    }

    if (!chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(consensus_params.hashGenesisBlock)) {
        return ChainstateLoadingError::ERROR_BAD_GENESIS_BLOCK;
    }

    if (!consensus_params.hashDevnetGenesisBlock.IsNull() && !chainman.BlockIndex().empty() &&
            !chainman.m_blockman.LookupBlockIndex(consensus_params.hashDevnetGenesisBlock)) {
        return ChainstateLoadingError::ERROR_BAD_DEVNET_GENESIS_BLOCK;
    }

    if (!fReset && !fReindexChainState) {
        // Check for changed -addressindex state
        if (!fAddressIndex && fAddressIndex != is_addrindex_enabled) {
            return ChainstateLoadingError::ERROR_ADDRIDX_NEEDS_REINDEX;
        }

        // Check for changed -timestampindex state
        if (!fTimestampIndex && fTimestampIndex != is_timeindex_enabled) {
            return ChainstateLoadingError::ERROR_TIMEIDX_NEEDS_REINDEX;
        }

        // Check for changed -spentindex state
        if (!fSpentIndex && fSpentIndex != is_spentindex_enabled) {
            return ChainstateLoadingError::ERROR_SPENTIDX_NEEDS_REINDEX;
        }
    }

    chainman.InitAdditionalIndexes();

    // Check for changed -prune state.  What we are concerned about is a user who has pruned blocks
    // in the past, but is now trying to run unpruned.
    if (chainman.m_blockman.m_have_pruned && !fPruneMode) {
        return ChainstateLoadingError::ERROR_PRUNED_NEEDS_REINDEX;
    }

    // At this point blocktree args are consistent with what's on disk.
    // If we're not mid-reindex (based on disk + args), add a genesis block on disk
    // (otherwise we use the one already on disk).
    // This is called again in ThreadImport after the reindex completes.
    if (!fReindex && !chainman.ActiveChainstate().LoadGenesisBlock()) {
        return ChainstateLoadingError::ERROR_LOAD_GENESIS_BLOCK_FAILED;
    }

    // At this point we're either in reindex or we've loaded a useful
    // block tree into BlockIndex()!

    for (CChainState* chainstate : chainman.GetAll()) {
        chainstate->InitCoinsDB(
            /* cache_size_bytes */ nCoinDBCache,
            /* in_memory */ coins_db_in_memory,
            /* should_wipe */ fReset || fReindexChainState);

        if (coins_error_cb) {
            chainstate->CoinsErrorCatcher().AddReadErrCallback(coins_error_cb);
        }

        // If necessary, upgrade from older database format.
        // This is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->CoinsDB().Upgrade()) {
            return ChainstateLoadingError::ERROR_CHAINSTATE_UPGRADE_FAILED;
        }

        // ReplayBlocks is a no-op if we cleared the coinsviewdb with -reindex or -reindex-chainstate
        if (!chainstate->ReplayBlocks()) {
            return ChainstateLoadingError::ERROR_REPLAYBLOCKS_FAILED;
        }

        // The on-disk coinsdb is now in a good state, create the cache
        chainstate->InitCoinsCache(nCoinCacheUsage);
        assert(chainstate->CanFlushToDisk());

        // flush evodb
        // TODO: CEvoDB instance should probably be a part of CChainState
        // (for multiple chainstates to actually work in parallel)
        // and not a global
        if (&chainman.ActiveChainstate() == chainstate && !evodb->CommitRootTransaction()) {
            return ChainstateLoadingError::ERROR_COMMITING_EVO_DB;
        }

        if (!is_coinsview_empty(chainstate)) {
            // LoadChainTip initializes the chain based on CoinsTip()'s best block
            if (!chainstate->LoadChainTip()) {
                return ChainstateLoadingError::ERROR_LOADCHAINTIP_FAILED;
            }
            assert(chainstate->m_chain.Tip() != nullptr);
            // The stored chain is taken as it is, so it must not hold a block that an earlier version
            // connected at or above the v23.1 height.
            if (chainstate->NeedsV231Revalidation()) {
                return ChainstateLoadingError::ERROR_V231_BLOCKS_NEED_REVALIDATION;
            }
        }
        // The kept block index must not hold a refusal that an earlier version stored at or above the v23.1
        // height. Under -reindex-chainstate this was asked above, before anything was wiped, and is not
        // asked again.
        if (!fReindexChainState && HoldsRefusalFromBeforeV231(chainman.m_blockman.m_block_index, consensus_params)) {
            return ChainstateLoadingError::ERROR_V231_REFUSALS_NEED_REVALIDATION;
        }
    }

    // Close any gap between the evodb and the coins tip BEFORE the migration gate
    // reads the marker, because an interrupted import leaves exactly such a gap and
    // the gate cannot tell it apart from an interrupted migration.
    if (!ReconcileEvoDBToTip(chainman, *evodb, *dmnman, *mnhf_manager, *llmq_ctx, consensus_params)) {
        return ChainstateLoadingError::ERROR_RECONCILING_EVO_DB;
    }

    if (!dmnman->MigrateDBIfNeeded() || !dmnman->MigrateDBIfNeeded2() || !dmnman->MigrateDBIfNeeded3() || !dmnman->MigrateDBIfNeeded4()) {
        return ChainstateLoadingError::ERROR_UPGRADING_EVO_DB;
    }
    if (!mnhf_manager->ForceSignalDBUpdate()) {
        return ChainstateLoadingError::ERROR_UPGRADING_SIGNALS_DB;
    }

    // A repair asked for with -evodbrepair runs here, before anything reads the
    // masternode list history: the block verification after the load disconnects
    // blocks and reads the lists before them, and the import thread, the network
    // and the end of RPC warmup come later still. Run in the background after
    // the import, as it used to, it raced all of them.
    if (evodb_repair && !fReset && !fReindexChainState &&
        (!dmnman->IsRepaired() || force_evodb_repair)) {
        using Outcome = CDeterministicMNManager::HistoryCheck::Outcome;
        CDeterministicMNManager& mnman = *dmnman;
        const LLMQContext& llmq = *llmq_ctx;
        const auto rebuild = [&mnman, &llmq](const CBlock& block, gsl::not_null<const CBlockIndex*> pindexPrev,
                                             const CDeterministicMNList& prevList, const CCoinsViewCache& view,
                                             bool debugLogs, BlockValidationState& state, CDeterministicMNList& mnListRet) {
            return mnman.RebuildListFromBlock(block, pindexPrev, prevList, state, view, mnListRet,
                                              debugLogs);
        };
        const auto check = mnman.VerifyOrRepairDiffs(chainman, rebuild, /*repair=*/true);
        if (check.outcome == Outcome::REPAIR_FAILED || check.outcome == Outcome::FAILED) {
            LogPrintf("%s -- failed to repair the masternode list diffs:\n%s\n", __func__, check.details);
            return ChainstateLoadingError::ERROR_REPAIRING_MN_LIST;
        }
        if (check.outcome == Outcome::UNRELIABLE) {
            // As for a verification without a repair: not fatal, not marked repaired.
            LogPrintf("%s -- masternode list diffs still do not reproduce the stored snapshots after the "
                      "repair; run `evodb repair` or restart with -evodbrepair to try again, or -reindex to "
                      "rebuild\n", __func__);
        }
    }

    // After the gates above the list history must be complete. The import
    // thread is the next reader, and a database error there would end the
    // process instead of reaching this caller.
    if (!VerifyMasternodeListAtTip(chainman.ActiveChain().Tip(), *dmnman)) {
        return ChainstateLoadingError::ERROR_LOADING_MN_LIST;
    }

    return std::nullopt;
}

bool VerifyMasternodeListAtTip(const CBlockIndex* tip, CDeterministicMNManager& dmnman)
{
    if (tip == nullptr) return true;
    try {
        (void)dmnman.GetListForBlock(tip);
    } catch (const dbwrapper_error& e) {
        LogPrintf("%s -- cannot build the masternode list at the tip (height %d): %s\n", __func__, tip->nHeight,
                  e.what());
        return false;
    }
    return true;
}

void DashChainstateSetup(ChainstateManager& chainman,
                         CGovernanceManager& govman,
                         CMasternodeMetaMan& mn_metaman,
                         CMasternodeSync& mn_sync,
                         CSporkManager& sporkman,
                         std::unique_ptr<CActiveMasternodeManager>& mn_activeman,
                         std::unique_ptr<CChainstateHelper>& chain_helper,
                         std::unique_ptr<CCreditPoolManager>& cpoolman,
                         std::unique_ptr<CDeterministicMNManager>& dmnman,
                         std::unique_ptr<CEvoDB>& evodb,
                         std::unique_ptr<CMNHFManager>& mnhf_manager,
                         std::unique_ptr<LLMQContext>& llmq_ctx,
                         CTxMemPool* mempool,
                         bool fReset,
                         bool fReindexChainState,
                         const Consensus::Params& consensus_params)
{
    // Same logic as pblocktree
    dmnman.reset();
    dmnman = std::make_unique<CDeterministicMNManager>(chainman.ActiveChainstate(), *evodb);

    cpoolman.reset();
    cpoolman = std::make_unique<CCreditPoolManager>(*evodb);

    if (llmq_ctx) {
        llmq_ctx->Interrupt();
        llmq_ctx->Stop();
    }
    llmq_ctx.reset();
    llmq_ctx = std::make_unique<LLMQContext>(chainman, *dmnman, *evodb, mn_metaman, *mnhf_manager, sporkman,
                                             *mempool, mn_activeman.get(), mn_sync, /*unit_tests=*/false, /*wipe=*/fReset || fReindexChainState);
    mempool->ConnectManagers(dmnman.get(), llmq_ctx->isman.get());
    // Enable CMNHFManager::{Process, Undo}Block
    mnhf_manager->ConnectManagers(&chainman, llmq_ctx->qman.get());

    chain_helper.reset();
    chain_helper = std::make_unique<CChainstateHelper>(*cpoolman, *dmnman, *mnhf_manager, govman, *(llmq_ctx->isman), *(llmq_ctx->quorum_block_processor),
                                                       chainman, consensus_params, mn_sync, sporkman, *(llmq_ctx->clhandler),
                                                       *(llmq_ctx->qman));
}

void DashChainstateSetupClose(std::unique_ptr<CChainstateHelper>& chain_helper,
                              std::unique_ptr<CCreditPoolManager>& cpoolman,
                              std::unique_ptr<CDeterministicMNManager>& dmnman,
                              std::unique_ptr<CMNHFManager>& mnhf_manager,
                              std::unique_ptr<LLMQContext>& llmq_ctx,
                              CTxMemPool* mempool)

{
    chain_helper.reset();
    if (mnhf_manager) {
        mnhf_manager->DisconnectManagers();
    }
    llmq_ctx.reset();
    cpoolman.reset();
    mempool->DisconnectManagers();
    dmnman.reset();
}

std::optional<ChainstateLoadVerifyError> VerifyLoadedChainstate(ChainstateManager& chainman,
                                                                CEvoDB& evodb,
                                                                bool fReset,
                                                                bool fReindexChainState,
                                                                const Consensus::Params& consensus_params,
                                                                int check_blocks,
                                                                int check_level,
                                                                std::function<int64_t()> get_unix_time_seconds,
                                                                std::function<void(bool)> notify_bls_state)
{
    auto is_coinsview_empty = [&](CChainState* chainstate) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
        return fReset || fReindexChainState || chainstate->CoinsTip().GetBestBlock().IsNull();
    };

    LOCK(cs_main);

    for (CChainState* chainstate : chainman.GetAll()) {
        if (!is_coinsview_empty(chainstate)) {
            const CBlockIndex* tip = chainstate->m_chain.Tip();
            if (tip && tip->nTime > get_unix_time_seconds() + MAX_FUTURE_BLOCK_TIME) {
                return ChainstateLoadVerifyError::ERROR_BLOCK_FROM_FUTURE;
            }
            const bool v19active{DeploymentActiveAfter(tip, consensus_params, Consensus::DEPLOYMENT_V19)};
            if (v19active) {
                bls::bls_legacy_scheme.store(false);
                if (notify_bls_state) notify_bls_state(bls::bls_legacy_scheme.load());
            }

            if (!CVerifyDB().VerifyDB(
                    *chainstate, consensus_params, chainstate->CoinsDB(),
                    evodb,
                    check_level,
                    check_blocks)) {
                return ChainstateLoadVerifyError::ERROR_CORRUPTED_BLOCK_DB;
            }

            // VerifyDB() disconnects blocks which might result in us switching back to legacy.
            // Make sure we use the right scheme.
            if (v19active && bls::bls_legacy_scheme.load()) {
                bls::bls_legacy_scheme.store(false);
                if (notify_bls_state) notify_bls_state(bls::bls_legacy_scheme.load());
            }

            if (check_level >= 3) {
                chainstate->ResetBlockFailureFlags(nullptr);
            }

        } else {
            // TODO: CEvoDB instance should probably be a part of CChainState
            // (for multiple chainstates to actually work in parallel)
            // and not a global
            if (&chainman.ActiveChainstate() == chainstate && !evodb.IsEmpty()) {
                // EvoDB processed some blocks earlier but we have no blocks anymore, something is wrong
                return ChainstateLoadVerifyError::ERROR_EVO_DB_SANITY_FAILED;
            }
        }
    }

    return std::nullopt;
}
