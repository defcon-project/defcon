// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_MASTERNODE_SYNC_H
#define BITCOIN_MASTERNODE_SYNC_H

#include <sync.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>

extern RecursiveMutex cs_main;

class ChainstateManager;
class CConnman;
class CBlockIndex;
class CDataStream;
class CGovernanceManager;
class CMasternodeSync;
class CNetFulfilledRequestManager;
class CNode;
class PeerManager;

static constexpr int MASTERNODE_SYNC_BLOCKCHAIN      = 1;
static constexpr int MASTERNODE_SYNC_GOVERNANCE      = 4;
static constexpr int MASTERNODE_SYNC_GOVOBJ          = 10;
static constexpr int MASTERNODE_SYNC_GOVOBJ_VOTE     = 11;
static constexpr int MASTERNODE_SYNC_FINISHED        = 999;

static constexpr int MASTERNODE_SYNC_TICK_SECONDS    = 6;
static constexpr int MASTERNODE_SYNC_TIMEOUT_SECONDS = 30; // our blocks are 2.5 minutes so 30 seconds should be fine
static constexpr int MASTERNODE_SYNC_RESET_SECONDS   = 900; // Reset fReachedBestHeader in CMasternodeSync::Reset if UpdateBlockTip hasn't been called for this seconds
// An active tip whose block time is this recent counts as the end of the chain for the blockchain stage, even
// if a better header is known: a proof-of-stake header costs nothing to make, so on its own it says nothing
// about where the chain is. Twelve block intervals; a real gap this long is very rare.
static constexpr int MASTERNODE_SYNC_RECENT_TIP_SECONDS = 30 * 60;

//
// CMasternodeSync : Sync masternode assets in stages
//

class CMasternodeSync
{
public:
    /// The stage, and how many times a stage has started (a Reset() or a switch) so far
    struct Stage {
        int asset;
        uint64_t generation;
    };

private:
    /// Serializes changes of stage: a Reset() and every switch. A switch is decided and made under it (or made
    /// only if the stage and its generation are those the decision saw), so a Reset() lands before the decision
    /// or after the switch, and is never lost between them. Lock order: cs_main, then this.
    mutable Mutex m_stage_mutex;
    uint64_t m_stage_generation GUARDED_BY(m_stage_mutex){0};
    /// How many syncs have started so far (each Reset() starts one): a trusted peer's restrictions are lifted
    /// once in each (LiftRestrictions)
    uint64_t m_sync_generation GUARDED_BY(m_stage_mutex){0};
    /// When this sync manager was made, in ms: keeps this process's lifting marks apart from the stored ones of
    /// an earlier run
    const int64_t m_sync_start;
    /// Tests only: run between a decision and its switch, with the locks held
    std::function<void()> m_test_hook_before_switch GUARDED_BY(m_stage_mutex);
    /// Tests only: run after the governance stage's switch, before it marks the peers, without the locks
    std::function<void()> m_test_hook_after_switch GUARDED_BY(m_stage_mutex);

    // The vote sync's wait, of the governance stage of generation m_votes_generation (ProcessTick's thread only)
    uint64_t m_votes_generation{std::numeric_limits<uint64_t>::max()};
    int64_t m_votes_wait_start{0};
    int m_votes_last_tick{0};
    int m_votes_last_count{0};

    // Keep track of current asset; written only under m_stage_mutex, read without it
    std::atomic<int> nCurrentAsset{MASTERNODE_SYNC_BLOCKCHAIN};
    // Count peers we've requested the asset from
    std::atomic<int> nTriedPeerCount{0};

    // Time when current masternode asset sync started
    std::atomic<int64_t> nTimeAssetSyncStarted{0};
    // ... last bumped
    std::atomic<int64_t> nTimeLastBumped{0};

    /// Set to true if best header is reached in CMasternodeSync::UpdatedBlockTip
    std::atomic<bool> fReachedBestHeader{false};
    /// Last time UpdateBlockTip has been called
    std::atomic<int64_t> nTimeLastUpdateBlockTip{0};

    CConnman& connman;
    CNetFulfilledRequestManager& m_netfulfilledman;
    /// Read under cs_main when the blockchain stage is decided: the active tip itself, never a copy of it
    const ChainstateManager& m_chainman;

    /// Moves on from `from` to the next stage and starts it; returns the stage left (0 if none)
    int SwitchLocked(int from) EXCLUSIVE_LOCKS_REQUIRED(m_stage_mutex);
    /// A peer the sync trusts: noban, or connected by hand
    static bool IsTrusted(const CNode& node);
    /// What leaving a stage does outside the lock: the governance step takes network locks. `sync_generation` is
    /// the sync the switch belonged to: if a Reset() has started another since, its peers are not marked.
    void AfterSwitch(int from, int64_t started, uint64_t sync_generation) LOCKS_EXCLUDED(::cs_main, m_stage_mutex);

public:
    explicit CMasternodeSync(CConnman& _connman, CNetFulfilledRequestManager& netfulfilledman, const ChainstateManager& chainman);

    void SendGovernanceSyncRequest(CNode* pnode) const;

    bool IsBlockchainSynced() const { return nCurrentAsset > MASTERNODE_SYNC_BLOCKCHAIN; }
    bool IsSynced() const { return nCurrentAsset == MASTERNODE_SYNC_FINISHED; }

    int GetAssetID() const { return nCurrentAsset; }
    int GetAttempt() const { return nTriedPeerCount; }
    void BumpAssetLastTime(const std::string& strFuncName);
    int64_t GetAssetStartTime() const { return nTimeAssetSyncStarted; }
    std::string GetAssetName() const;
    std::string GetSyncStatus() const;

    void Reset(bool fForce = false, bool fNotifyReset = true) LOCKS_EXCLUDED(m_stage_mutex);
    /// The stage and its generation, read together
    Stage GetStage() const LOCKS_EXCLUDED(m_stage_mutex);
    /// Moves on from whatever the stage is now (`mnsync next`). Its governance step takes network and
    /// fulfilled-request locks, so never under cs_main.
    void SwitchToNextAsset() LOCKS_EXCLUDED(::cs_main, m_stage_mutex);
    /// Moves on only if the stage is still the one a decision saw, same generation: a Reset() since then, or a
    /// Reset() and a new start of the same stage, leaves it alone. Returns whether it moved.
    bool SwitchFrom(const Stage& seen) LOCKS_EXCLUDED(::cs_main, m_stage_mutex);
    /// Tests only: see m_test_hook_before_switch
    void SetTestHookBeforeSwitch(std::function<void()> hook) LOCKS_EXCLUDED(m_stage_mutex);
    /// Tests only: see m_test_hook_after_switch
    void SetTestHookAfterSwitch(std::function<void()> hook) LOCKS_EXCLUDED(m_stage_mutex);
    /// Whether the governance stage `seen` has finished its vote sync, checked once per tick with every object
    /// asked for: MASTERNODE_SYNC_TIMEOUT_SECONDS since then, and hardly any new votes in the last tick. The wait
    /// is that stage's own; a new stage (another generation) starts it anew. ProcessTick's thread only.
    bool VoteSyncDone(const Stage& seen, int tick, int votes);
    /// A trusted peer (noban or manual) is exempt from the sync's per-peer restrictions: everything fulfilled for
    /// it is forgotten once in this process (and again once that mark expires), and once in each sync, after every
    /// Reset() too, its "full-sync" mark, so it is not disconnected as recently synced; what was asked of it within
    /// the expiry stays asked. Returns whether anything was forgotten now.
    bool LiftRestrictions(const CNode& node) LOCKS_EXCLUDED(m_stage_mutex);
    /// Whether the sync disconnects `node` as recently synced from (marked "full-sync" by a finished sync, to free
    /// its slot): never a trusted peer, whatever order a finish, a Reset() and the next sync's exemption ran in.
    bool DisconnectsAsRecentlySynced(const CNode& node);

    void ProcessMessage(const CNode& peer, std::string_view msg_type, CDataStream& vRecv) const;
    void ProcessTick(const PeerManager& peerman, const CGovernanceManager& govman);

    void AcceptedBlockHeader(const CBlockIndex *pindexNew);
    void NotifyHeaderTip(const CBlockIndex *pindexNew, bool fInitialDownload);
    void UpdatedBlockTip(const CBlockIndex *pindexTip, const CBlockIndex *pindexNew, bool fInitialDownload);

    /// Whether the blockchain stage may end now, given the stage timeout for the current number of peers: the
    /// best header reached and nothing arrived for the timeout (since Dash), or the active tip recent, out of
    /// initial block download
    bool IsBlockchainStageDone(int64_t nTimeSyncTimeout) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /// Ends the blockchain stage if it may end now: decided and switched under cs_main and m_stage_mutex
    bool TryEndBlockchainStage(int64_t nTimeSyncTimeout) LOCKS_EXCLUDED(m_stage_mutex);

    void DoMaintenance(const PeerManager& peerman, const CGovernanceManager& govman);
};

#endif // BITCOIN_MASTERNODE_SYNC_H
