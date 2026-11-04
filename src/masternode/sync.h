// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_MASTERNODE_SYNC_H
#define BITCOIN_MASTERNODE_SYNC_H

#include <sync.h>

#include <atomic>
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
private:
    // Keep track of current asset
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

    void Reset(bool fForce = false, bool fNotifyReset = true);
    /// Its governance step takes network and fulfilled-request locks, so never under cs_main
    void SwitchToNextAsset() LOCKS_EXCLUDED(::cs_main);

    void ProcessMessage(const CNode& peer, std::string_view msg_type, CDataStream& vRecv) const;
    void ProcessTick(const PeerManager& peerman, const CGovernanceManager& govman);

    void AcceptedBlockHeader(const CBlockIndex *pindexNew);
    void NotifyHeaderTip(const CBlockIndex *pindexNew, bool fInitialDownload);
    void UpdatedBlockTip(const CBlockIndex *pindexTip, const CBlockIndex *pindexNew, bool fInitialDownload);

    /// Whether the blockchain stage may end now, given the stage timeout for the current number of peers: the
    /// best header reached and nothing arrived for the timeout (since Dash), or the active tip recent, out of
    /// initial block download
    bool IsBlockchainStageDone(int64_t nTimeSyncTimeout) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    /// Ends the blockchain stage if it may end now: decided and switched under cs_main
    bool TryEndBlockchainStage(int64_t nTimeSyncTimeout);

    void DoMaintenance(const PeerManager& peerman, const CGovernanceManager& govman);
};

#endif // BITCOIN_MASTERNODE_SYNC_H
