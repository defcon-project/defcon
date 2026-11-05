// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <masternode/sync.h>

#include <chainparams.h>
#include <governance/governance.h>
#include <netfulfilledman.h>
#include <netmessagemaker.h>
#include <node/interface_ui.h>
#include <shutdown.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

class CMasternodeSync;

CMasternodeSync::CMasternodeSync(CConnman& _connman, CNetFulfilledRequestManager& netfulfilledman, const ChainstateManager& chainman) :
    m_sync_start(GetTimeMillis()),
    nTimeAssetSyncStarted(GetTime()),
    nTimeLastBumped(GetTime()),
    connman(_connman),
    m_netfulfilledman(netfulfilledman),
    m_chainman(chainman)
{
}

void CMasternodeSync::Reset(bool fForce, bool fNotifyReset)
{
    // Avoid resetting the sync process if we just "recently" received a new block
    if (!fForce) {
        if (GetTime() - nTimeLastUpdateBlockTip < MASTERNODE_SYNC_RESET_SECONDS) {
            return;
        }
    }
    {
        // Under the stage lock: a switch decided before this cannot be made after it.
        LOCK(m_stage_mutex);
        nCurrentAsset = MASTERNODE_SYNC_BLOCKCHAIN;
        ++m_stage_generation;
        ++m_sync_generation;
        nTriedPeerCount = 0;
        nTimeAssetSyncStarted = GetTime();
        nTimeLastBumped = GetTime();
        nTimeLastUpdateBlockTip = 0;
        fReachedBestHeader = false;
    }
    if (fNotifyReset) {
        uiInterface.NotifyAdditionalDataSyncProgressChanged(-1);
    }
}

void CMasternodeSync::BumpAssetLastTime(const std::string& strFuncName)
{
    if (IsSynced()) return;
    nTimeLastBumped = GetTime();
    LogPrint(BCLog::MNSYNC, "CMasternodeSync::BumpAssetLastTime -- %s\n", strFuncName);
}

std::string CMasternodeSync::GetAssetName() const
{
    switch(nCurrentAsset)
    {
        case(MASTERNODE_SYNC_BLOCKCHAIN):   return "MASTERNODE_SYNC_BLOCKCHAIN";
        case(MASTERNODE_SYNC_GOVERNANCE):   return "MASTERNODE_SYNC_GOVERNANCE";
        case MASTERNODE_SYNC_FINISHED:      return "MASTERNODE_SYNC_FINISHED";
        default:                            return "UNKNOWN";
    }
}

int CMasternodeSync::SwitchLocked(int from)
{
    AssertLockHeld(m_stage_mutex);
    int left{0};
    switch (from) {
        case MASTERNODE_SYNC_BLOCKCHAIN:
            nCurrentAsset = MASTERNODE_SYNC_GOVERNANCE;
            left = from;
            break;
        case MASTERNODE_SYNC_GOVERNANCE:
            nCurrentAsset = MASTERNODE_SYNC_FINISHED;
            left = from;
            break;
    }
    if (left != 0) ++m_stage_generation;
    nTriedPeerCount = 0;
    nTimeAssetSyncStarted = GetTime();
    // BumpAssetLastTime, here under the lock so that the new stage's timeout starts with it
    if (!IsSynced()) nTimeLastBumped = GetTime();
    return left;
}

void CMasternodeSync::AfterSwitch(int from, int64_t started, uint64_t sync_generation)
{
    switch (from) {
        case MASTERNODE_SYNC_BLOCKCHAIN:
            LogPrintf("CMasternodeSync::SwitchToNextAsset -- Completed %s in %llds\n", "MASTERNODE_SYNC_BLOCKCHAIN", GetTime() - started);
            LogPrintf("CMasternodeSync::SwitchToNextAsset -- Starting %s\n", "MASTERNODE_SYNC_GOVERNANCE");
            break;
        case MASTERNODE_SYNC_GOVERNANCE: {
            LogPrintf("CMasternodeSync::SwitchToNextAsset -- Completed %s in %llds\n", "MASTERNODE_SYNC_GOVERNANCE", GetTime() - started);
            uiInterface.NotifyAdditionalDataSyncProgressChanged(1);

            // The peers taken first, outside the stage lock (the network's locks never under it).
            std::vector<CService> peers;
            connman.ForEachNode(CConnman::AllNodes, [&peers](const CNode* pnode) { peers.push_back(pnode->addr); });
            if (const auto hook{WITH_LOCK(m_stage_mutex, return m_test_hook_after_switch)}) hook();
            {
                // Marked as synced from under the stage lock, and only if no Reset() has started another sync since
                // the switch: that sync may have exempted its trusted peers already (LiftRestrictions, under the
                // same lock), and this mark would then have them disconnected as recently synced.
                LOCK(m_stage_mutex);
                if (m_sync_generation == sync_generation) {
                    for (const CService& addr : peers) m_netfulfilledman.AddFulfilledRequest(addr, "full-sync");
                }
            }
            LogPrintf("CMasternodeSync::SwitchToNextAsset -- Sync has finished\n");
            break;
        }
    }
    if (!IsSynced()) LogPrint(BCLog::MNSYNC, "CMasternodeSync::BumpAssetLastTime -- %s\n", "CMasternodeSync::SwitchToNextAsset");
}

CMasternodeSync::Stage CMasternodeSync::GetStage() const
{
    LOCK(m_stage_mutex);
    return Stage{nCurrentAsset, m_stage_generation};
}

void CMasternodeSync::SwitchToNextAsset()
{
    assert(m_netfulfilledman.IsValid());
    int left;
    int64_t started;
    uint64_t sync_generation;
    {
        LOCK(m_stage_mutex);
        started = nTimeAssetSyncStarted;
        sync_generation = m_sync_generation;
        left = SwitchLocked(nCurrentAsset);
    }
    AfterSwitch(left, started, sync_generation);
}

bool CMasternodeSync::SwitchFrom(const Stage& seen)
{
    assert(m_netfulfilledman.IsValid());
    int64_t started;
    uint64_t sync_generation;
    {
        LOCK(m_stage_mutex);
        if (nCurrentAsset != seen.asset || m_stage_generation != seen.generation) return false;
        started = nTimeAssetSyncStarted;
        sync_generation = m_sync_generation;
        if (SwitchLocked(seen.asset) == 0) return false;
    }
    AfterSwitch(seen.asset, started, sync_generation);
    return true;
}

bool CMasternodeSync::VoteSyncDone(const Stage& seen, int tick, int votes)
{
    // The wait belongs to one governance stage: a stage started anew (a Reset() and back) waits anew. It used to
    // live in function statics that nothing reset.
    if (m_votes_generation != seen.generation) {
        m_votes_generation = seen.generation;
        m_votes_wait_start = 0;
        m_votes_last_tick = 0;
        m_votes_last_count = 0;
    }
    if (m_votes_wait_start == 0) {
        // asked all objects for votes for the first time
        m_votes_wait_start = GetTime();
    }
    // make sure the condition below is checked only once per tick
    if (m_votes_last_tick == tick) return false;
    if (GetTime() - m_votes_wait_start > MASTERNODE_SYNC_TIMEOUT_SECONDS &&
        votes - m_votes_last_count < std::max(int(0.0001 * m_votes_last_count), MASTERNODE_SYNC_TICK_SECONDS)) {
        // We already asked for all objects, waited for MASTERNODE_SYNC_TIMEOUT_SECONDS
        // after that and less then 0.01% or MASTERNODE_SYNC_TICK_SECONDS
        // (i.e. 1 per second) votes were received during the last tick.
        // We can be pretty sure that we are done syncing.
        m_votes_wait_start = 0;
        return true;
    }
    m_votes_last_tick = tick;
    m_votes_last_count = votes;
    return false;
}

bool CMasternodeSync::IsTrusted(const CNode& node)
{
    return node.HasPermission(NetPermissionFlags::NoBan) || node.IsManualConn();
}

bool CMasternodeSync::DisconnectsAsRecentlySynced(const CNode& node)
{
    // A trusted peer never: it is exempt from the sync's restrictions, and a sync finishing around a Reset() can
    // leave its mark on it in more than one order (the finish, the next sync's exemption and this decision run on
    // different threads). The next sync's exemption lifts the mark, and the peer is asked again.
    return !IsTrusted(node) && m_netfulfilledman.HasFulfilledRequest(node.addr, "full-sync");
}

bool CMasternodeSync::LiftRestrictions(const CNode& node)
{
    if (!IsTrusted(node)) return false;
    bool lifted{false};
    // Everything fulfilled for the peer is forgotten as before: once in this process, and again whenever the mark
    // has expired (FulfilledRequestExpireTime), as often as the peer itself lets us ask it for the whole governance
    // list (asked again sooner, it scores us as misbehaving).
    const std::string process_mark{strprintf("allow-sync-%lld", m_sync_start)};
    if (!m_netfulfilledman.HasFulfilledRequest(node.addr, process_mark)) {
        m_netfulfilledman.RemoveAllFulfilledRequests(node.addr);
        m_netfulfilledman.AddFulfilledRequest(node.addr, process_mark);
        lifted = true;
    }
    // And once in each sync, after every Reset() too, that the last sync finished with it: else the trusted peer is
    // disconnected as recently synced. This used to happen once per process only (a function static mark), so
    // after a Reset() the "full-sync" mark of the sync before stayed. Under the stage lock, as the finish marks its
    // peers (AfterSwitch): a finish of the sync before marks either before this, or not at all.
    {
        LOCK(m_stage_mutex);
        const std::string sync_mark{strprintf("%s-%llu", process_mark, m_sync_generation)};
        if (!m_netfulfilledman.HasFulfilledRequest(node.addr, sync_mark)) {
            m_netfulfilledman.RemoveFulfilledRequest(node.addr, "full-sync");
            m_netfulfilledman.AddFulfilledRequest(node.addr, sync_mark);
            lifted = true;
        }
    }
    if (lifted) LogPrintf("CMasternodeSync::ProcessTick -- skipping mnsync restrictions for peer=%d\n", node.GetId());
    return lifted;
}

void CMasternodeSync::SetTestHookBeforeSwitch(std::function<void()> hook)
{
    LOCK(m_stage_mutex);
    m_test_hook_before_switch = std::move(hook);
}

void CMasternodeSync::SetTestHookAfterSwitch(std::function<void()> hook)
{
    LOCK(m_stage_mutex);
    m_test_hook_after_switch = std::move(hook);
}

std::string CMasternodeSync::GetSyncStatus() const
{
    switch (nCurrentAsset) {
        case MASTERNODE_SYNC_BLOCKCHAIN:    return _("Synchronizing blockchain…").translated;
        case MASTERNODE_SYNC_GOVERNANCE:    return _("Synchronizing governance objects…").translated;
        case MASTERNODE_SYNC_FINISHED:      return _("Synchronization finished").translated;
        default:                            return "";
    }
}

void CMasternodeSync::ProcessMessage(const CNode& peer, std::string_view msg_type, CDataStream& vRecv) const
{
    //Sync status count
    if (msg_type != NetMsgType::SYNCSTATUSCOUNT) return;

    //do not care about stats if sync process finished
    if (IsSynced()) return;

    int nItemID;
    int nCount;
    vRecv >> nItemID >> nCount;

    LogPrint(BCLog::MNSYNC, "SYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, peer.GetId());
}

void CMasternodeSync::ProcessTick(const PeerManager& peerman, const CGovernanceManager& govman)
{
    assert(m_netfulfilledman.IsValid());

    static int nTick = 0;
    nTick++;

    // reset the sync process if the last call to this function was more than 60 minutes ago (client was in sleep mode)
    static int64_t nTimeLastProcess = GetTime();
    if (!Params().IsMockableChain() && GetTime() - nTimeLastProcess > 60 * 60 && !fMasternodeMode) {
        LogPrintf("CMasternodeSync::ProcessTick -- WARNING: no actions for too long, restarting sync...\n");
        Reset(true);
        nTimeLastProcess = GetTime();
        return;
    }

    if(GetTime() - nTimeLastProcess < MASTERNODE_SYNC_TICK_SECONDS) {
        // too early, nothing to do here
        return;
    }

    nTimeLastProcess = GetTime();
    const CConnman::NodesSnapshot snap{connman, /* filter = */ CConnman::FullyConnectedOnly};

    // gradually request the rest of the votes after sync finished
    if(IsSynced()) {
        govman.RequestGovernanceObjectVotes(snap.Nodes(), connman, peerman);
        return;
    }

    // Calculate "progress" for LOG reporting / GUI notification
    double nSyncProgress = double(nTriedPeerCount + (nCurrentAsset - 1) * 8) / (8*4);
    LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d nCurrentAsset %d nTriedPeerCount %d nSyncProgress %f\n", nTick, nCurrentAsset, nTriedPeerCount, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

    for (auto& pnode : snap.Nodes())
    {
        CNetMsgMaker msgMaker(pnode->GetCommonVersion());

        // Don't try to sync any data from outbound non-relay "masternode" connections.
        // Inbound connection this early is most likely a "masternode" connection
        // initiated from another node, so skip it too.
        if (!pnode->CanRelay() || (fMasternodeMode && pnode->IsInboundConn())) continue;

        {
            LiftRestrictions(*pnode);

            if(m_netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // We already fully synced from this node recently: disconnect to free this connection slot for
                // another peer, unless it is trusted (DisconnectsAsRecentlySynced). Either way nothing is asked of
                // it now.
                if (DisconnectsAsRecentlySynced(*pnode)) {
                    pnode->fDisconnect = true;
                    LogPrintf("CMasternodeSync::ProcessTick -- disconnecting from recently synced peer=%d\n", pnode->GetId());
                }
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC

            if(!m_netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // always get sporks first, only request once from each peer
                m_netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETSPORKS));
                LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d nCurrentAsset %d -- requesting sporks from peer=%d\n", nTick, nCurrentAsset, pnode->GetId());
            }

            if (nCurrentAsset == MASTERNODE_SYNC_BLOCKCHAIN) {
                int64_t nTimeSyncTimeout = snap.Nodes().size() > 3 ? MASTERNODE_SYNC_TICK_SECONDS : MASTERNODE_SYNC_TIMEOUT_SECONDS;
                if (TryEndBlockchainStage(nTimeSyncTimeout)) {
                    // At this point we know that:
                    // a) there are peers (because we are looping on at least one of them);
                    // b) we waited for at least MASTERNODE_SYNC_TICK_SECONDS/MASTERNODE_SYNC_TIMEOUT_SECONDS
                    //    (depending on the number of connected peers) since we reached the headers tip the last
                    //    time (i.e. since fReachedBestHeader has been set to true);
                    // c) there were no blocks (UpdatedBlockTip, NotifyHeaderTip) or headers (AcceptedBlockHeader)
                    //    for at least MASTERNODE_SYNC_TICK_SECONDS/MASTERNODE_SYNC_TIMEOUT_SECONDS (depending on
                    //    the number of connected peers).
                    // Or, in place of b) and c): our active tip is recent (IsBlockchainStageDone).
                    // We must be at the tip already; TryEndBlockchainStage has moved to the next asset.
                    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

                    if (gArgs.GetBoolArg("-syncmempool", DEFAULT_SYNC_MEMPOOL)) {
                        // Now that the blockchain is synced request the mempool from the connected outbound nodes if possible
                        for (auto pNodeTmp : snap.Nodes()) {
                            bool fRequestedEarlier = m_netfulfilledman.HasFulfilledRequest(pNodeTmp->addr, "mempool-sync");
                            if (!pNodeTmp->IsInboundConn() && !fRequestedEarlier && !pNodeTmp->IsBlockRelayOnly()) {
                                m_netfulfilledman.AddFulfilledRequest(pNodeTmp->addr, "mempool-sync");
                                connman.PushMessage(pNodeTmp, msgMaker.Make(NetMsgType::MEMPOOL));
                                LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d nCurrentAsset %d -- syncing mempool from peer=%d\n", nTick, nCurrentAsset, pNodeTmp->GetId());
                            }
                        }
                    }
                }
            }

            // GOVOBJ : SYNC GOVERNANCE ITEMS FROM OUR PEERS

            // Decided on the stage and its generation as read here: a Reset() since then (`mnsync reset`, a
            // block notification) restarts the blockchain stage, and the switch leaves that alone.
            const Stage gov_seen{GetStage()};
            if (gov_seen.asset == MASTERNODE_SYNC_GOVERNANCE) {
                if (!govman.IsValid()) {
                    SwitchFrom(gov_seen);
                    return;
                }
                LogPrint(BCLog::GOBJECT, "CMasternodeSync::ProcessTick -- nTick %d nCurrentAsset %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nCurrentAsset, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);

                // check for timeout first
                if(GetTime() - nTimeLastBumped > MASTERNODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrint(BCLog::MNSYNC, "CMasternodeSync::ProcessTick -- nTick %d nCurrentAsset %d -- timeout\n", nTick, nCurrentAsset);
                    if(nTriedPeerCount == 0) {
                        LogPrintf("CMasternodeSync::ProcessTick -- WARNING: failed to sync %s\n", GetAssetName());
                        // it's kind of ok to skip this for now, hopefully we'll catch up later?
                    }
                    SwitchFrom(gov_seen);
                    return;
                }

                // only request obj sync once from each peer
                if(m_netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) {
                    // will request votes on per-obj basis from each node in a separate loop below
                    // to avoid deadlocks here
                    continue;
                }
                m_netfulfilledman.AddFulfilledRequest(pnode->addr, "governance-sync");

                nTriedPeerCount++;

                SendGovernanceSyncRequest(pnode);

                break; //this will cause each peer to get one request each six seconds for the various assets we need
            }
        }
    }


    const Stage votes_seen{GetStage()};
    if (votes_seen.asset != MASTERNODE_SYNC_GOVERNANCE) {
        // looped through all nodes and not syncing governance yet/already, release them
        return;
    }

    // request votes on per-obj basis from each node
    for (const auto& pnode : snap.Nodes()) {
        if(!m_netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) {
            continue; // to early for this node
        }
        int nObjsLeftToAsk = govman.RequestGovernanceObjectVotes(*pnode, connman, peerman);
        // check for data
        if (nObjsLeftToAsk == 0 && VoteSyncDone(votes_seen, nTick, govman.GetVoteCount())) {
            LogPrintf("CMasternodeSync::ProcessTick -- nTick %d nCurrentAsset %d -- asked for all objects, nothing to do\n", nTick, MASTERNODE_SYNC_GOVERNANCE);
            SwitchFrom(votes_seen);
            return;
        }
    }
}

void CMasternodeSync::SendGovernanceSyncRequest(CNode* pnode) const
{
    CNetMsgMaker msgMaker(pnode->GetCommonVersion());

    CBloomFilter filter;

    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::MNGOVERNANCESYNC, uint256(), filter));
}

bool CMasternodeSync::IsBlockchainStageDone(int64_t nTimeSyncTimeout) const
{
    AssertLockHeld(::cs_main);
    // Since Dash: the best header is reached, and nothing -- block or header -- arrived for the timeout.
    if (fReachedBestHeader && (GetTime() - nTimeLastBumped > nTimeSyncTimeout)) return true;
    // Besides: the active tip is recent. Above the last proof-of-work block a header is accepted without any
    // proof, so anyone can put one above our tip that no block will ever follow, and then the best header is
    // never reached. The tip is read here, under cs_main, where validation changes it, and not kept from a
    // notification: nothing kept can fall behind the chain (a queued notification, a Reset(), or blocks that
    // InvalidateBlock() disconnects one by one, notifying only at the end).
    const CBlockIndex* tip{m_chainman.ActiveChain().Tip()};
    return tip != nullptr && !m_chainman.ActiveChainstate().IsInitialBlockDownload() &&
           GetTime() - tip->GetBlockTime() < MASTERNODE_SYNC_RECENT_TIP_SECONDS;
}

bool CMasternodeSync::TryEndBlockchainStage(int64_t nTimeSyncTimeout)
{
    int64_t started;
    uint64_t sync_generation;
    {
        // Decided and switched under cs_main, which validation holds while it changes the active tip, and under
        // the stage lock, which Reset() takes: a Reset() lands before the decision or after the switch, never
        // between them. If `mnsync next` moved the stage already, nothing happens here; the next stage's work
        // (it takes network locks) runs after both locks.
        LOCK2(::cs_main, m_stage_mutex);
        if (nCurrentAsset != MASTERNODE_SYNC_BLOCKCHAIN || !IsBlockchainStageDone(nTimeSyncTimeout)) return false;
        if (m_test_hook_before_switch) m_test_hook_before_switch();
        started = nTimeAssetSyncStarted;
        sync_generation = m_sync_generation;
        SwitchLocked(MASTERNODE_SYNC_BLOCKCHAIN);
    }
    AfterSwitch(MASTERNODE_SYNC_BLOCKCHAIN, started, sync_generation);
    return true;
}

void CMasternodeSync::AcceptedBlockHeader(const CBlockIndex *pindexNew)
{
    LogPrint(BCLog::MNSYNC, "CMasternodeSync::AcceptedBlockHeader -- pindexNew->nHeight: %d\n", pindexNew->nHeight);

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block header arrives while we are still syncing blockchain
        BumpAssetLastTime("CMasternodeSync::AcceptedBlockHeader");
    }
}

void CMasternodeSync::NotifyHeaderTip(const CBlockIndex *pindexNew, bool fInitialDownload)
{
    if (pindexNew == nullptr) {
        return;
    }
    LogPrint(BCLog::MNSYNC, "CMasternodeSync::NotifyHeaderTip -- pindexNew->nHeight: %d fInitialDownload=%d\n", pindexNew->nHeight, fInitialDownload);
    if (IsSynced())
        return;

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block arrives while we are still syncing blockchain
        BumpAssetLastTime("CMasternodeSync::NotifyHeaderTip");
    }
}

void CMasternodeSync::UpdatedBlockTip(const CBlockIndex *pindexTip, const CBlockIndex *pindexNew, bool fInitialDownload)
{
    LogPrint(BCLog::MNSYNC, "CMasternodeSync::UpdatedBlockTip -- pindexNew->nHeight: %d fInitialDownload=%d\n", pindexNew->nHeight, fInitialDownload);
    nTimeLastUpdateBlockTip = GetTime<std::chrono::seconds>().count();

    if (IsSynced())
        return;

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block arrives while we are still syncing blockchain
        BumpAssetLastTime("CMasternodeSync::UpdatedBlockTip");
    }

    if (fInitialDownload) {
        // switched too early
        if (IsBlockchainSynced()) {
            Reset(true);
        }

        // no need to check any further while still in IBD mode
        return;
    }

    // Note: since we sync headers first, it should be ok to use this
    if (pindexTip == nullptr) return;
    bool fReachedBestHeaderNew = pindexNew->GetBlockHash() == pindexTip->GetBlockHash();

    if (fReachedBestHeader && !fReachedBestHeaderNew) {
        // Switching from true to false means that we previously stuck syncing headers for some reason,
        // probably initial timeout was not enough,
        // because there is no way we can update tip not having best header
        Reset(true);
    }

    fReachedBestHeader = fReachedBestHeaderNew;
    LogPrint(BCLog::MNSYNC, "CMasternodeSync::UpdatedBlockTip -- pindexNew->nHeight: %d pindexTip->nHeight: %d fInitialDownload=%d fReachedBestHeader=%d\n",
                pindexNew->nHeight, pindexTip->nHeight, fInitialDownload, fReachedBestHeader);
}

void CMasternodeSync::DoMaintenance(const PeerManager& peerman, const CGovernanceManager& govman)
{
    if (ShutdownRequested()) return;

    ProcessTick(peerman, govman);
}
