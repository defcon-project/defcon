// Copyright (c) 2025 The Pacplatform Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/minter.h>

#include <masternode/sync.h>
#include <pos/multiwallet.h>
#include <timedata.h>
#include <net.h>

extern int stakable_sz;
extern std::vector<CStakeWallet> stakable_wallets;

std::thread m_staking_thread;

std::atomic<bool> fStopMinerProc(false);
std::atomic<bool> fTryToSync(false);
std::atomic<bool> fIsStaking(false);
std::atomic<int64_t> nTimeLastStake(0);

bool CheckStake(ChainstateManager& chainman, CBlock *pblock)
{
    uint256 proofHash, hashTarget;
    uint256 hashBlock = pblock->GetHash();

    if (!pblock->IsProofOfStake()) {
        LogPrint(BCLog::POS, "%s: %s is not a proof-of-stake block.", __func__, hashBlock.GetHex());
        return false;
    }

    {
        BlockMap::iterator mi{chainman.m_blockman.m_block_index.find(pblock->hashPrevBlock)};
        if (mi == chainman.m_blockman.m_block_index.end()) {
            LogPrint(BCLog::POS, "%s: %s prev block not found: %s.", __func__, hashBlock.GetHex(), pblock->hashPrevBlock.GetHex());
            return false;
        }

        if (!chainman.ActiveChain().Contains(&mi->second)) {
            LogPrint(BCLog::POS, "%s: %s prev block in active chain: %s.", __func__, hashBlock.GetHex(), pblock->hashPrevBlock.GetHex());
            return false;
        }

        BlockValidationState state;
        if (!CheckProofOfStake(chainman.ActiveChainstate(), state, &mi->second, *pblock->vtx[1], pblock->nTime, pblock->nBits, proofHash, hashTarget)) {
            LogPrint(BCLog::POS, "%s: proof-of-stake checking failed.", __func__);
            return false;
        }

        CBlockIndex* active_chain_tip = chainman.ActiveChain().Tip();
        if (pblock->hashPrevBlock != active_chain_tip->GetBlockHash()) {
            LogPrint(BCLog::POS, "%s: Generated block is stale.", __func__);
            return false;
        }
    }

    LogPrint(BCLog::POS, "%s: New proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", __func__, hashBlock.GetHex(), proofHash.GetHex(), hashTarget.GetHex());

    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
    if (!chainman.ProcessNewBlock(Params(), shared_pblock, true, nullptr)) {
        LogPrint(BCLog::POS, "%s: Block not accepted.", __func__);
        return false;
    }

    return true;
}

static bool IsMainnet()
{
    return Params().NetworkIDString() == "main";
}

void StopStaking()
{
    fStopMinerProc = true;
    if (m_staking_thread.joinable()) {
        m_staking_thread.join();
    }
}

void PoSMiner(NodeContext& node)
{
    if (!gArgs.GetBoolArg("-staking", true)) {
        LogPrint(BCLog::POS, "%s: -staking is false.\n", __func__);
        return;
    }

    int nBestHeight;
    int64_t nBestTime;

    const CChainParams& params = Params();
    int min_nodes = IsMainnet() ? 3 : 0;

    CConnman& connman = *node.connman;
    CTxMemPool& mempool = *node.mempool;
    CMasternodeSync& mn_sync = *node.mn_sync;
    ChainstateManager& chainman = *node.chainman;

    CBlockIndex* active_chain_tip = nullptr;

    MultiwalletInitialize();

    //multiwallet loop
    while (!fStopMinerProc)
    {
        if (fStopMinerProc)
            return;

        UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::SHORTDELAY});

        MultiwalletMaintenance();

        bool foundBlock{false};
        CScript coinbaseScript;
        for (int y = 0; y < stakable_sz; y++)
        {
            if (fStopMinerProc)
                return;
            if (!stakable_wallets[y].CanStake())
                continue;
            CWallet* this_wallet = stakable_wallets[y].GetWallet();
            if (!this_wallet)
                continue;
            if (foundBlock)
                continue;

            int num_nodes;
            {
                LOCK(cs_main);
                active_chain_tip = chainman.ActiveChain().Tip();
                nBestHeight = active_chain_tip->nHeight;
                nBestTime = active_chain_tip->nTime;
                num_nodes = connman.GetNodeCount(ConnectionDirection::Both);
            }

            if (IsMainnet())
            {
                if (fTryToSync) {
                    fTryToSync = false;
                    if (num_nodes < min_nodes || chainman.ActiveChainstate().IsInitialBlockDownload()) {
                        fIsStaking = false;
                        LogPrint(BCLog::POS, "%s: TryToSync\n", __func__);
                        UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::LARGEDELAY});
                        continue;
                    }
                }

                if (num_nodes < min_nodes || chainman.ActiveChainstate().IsInitialBlockDownload()) {
                    fIsStaking = false;
                    fTryToSync = true;
                    LogPrint(BCLog::POS, "%s: IsInitialBlockDownload\n", __func__);
                    UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::LARGEDELAY});
                    continue;
                }
            }

            if (nBestHeight < params.GetConsensus().lastPowBlock) {
                fIsStaking = false;
                LogPrint(BCLog::POS, "%s: WaitForPoS\n", __func__);
                UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::LARGEDELAY});
                continue;
            }

            if (!mn_sync.IsSynced()) {
                fIsStaking = false;
                fTryToSync = true;
                LogPrint(BCLog::POS, "%s: IsMasternodeSynced\n", __func__);
                UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::LARGEDELAY});
                continue;
            }

            //abandon orphaned coinstakes
            this_wallet->AbandonOrphanedCoinstakes();

            int64_t nTime = GetAdjustedTime();
            int64_t nMask = params.GetConsensus().posTimestampMask;
            int64_t nSearchTime = nTime & ~nMask;

            if (nSearchTime <= nBestTime)
            {
                if (nTime < nBestTime) {
                    LogPrint(BCLog::POS, "%s: Can't stake before last block time.\n", __func__);
                    UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::LARGEDELAY});
                    continue;
                }

                int64_t nNextSearch = nSearchTime + nMask;
                continue;
            }

            LogPrint(BCLog::POS, "%s - using wallet %d\n", __func__, y);

            std::unique_ptr<CBlockTemplate> pblocktemplate;

            size_t i = 0;
            CAmount reserve_balance;
            {
                {
                    LOCK(this_wallet->cs_wallet);
                    if (nSearchTime <= this_wallet->nLastCoinStakeSearchTime) {
                        continue;
                    }

                    if (this_wallet->IsLocked()) {
                        this_wallet->m_is_staking = NOT_STAKING_LOCKED;
                        LogPrint(BCLog::POS, "%s: Wallet %d, locked wallet.\n", __func__, i);
                        UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::SHORTDELAY});
                        continue;
                    }

                    reserve_balance = this_wallet->nReserveBalance;
                }

                CAmount balance = this_wallet->GetAvailableBalance();

                if (balance <= reserve_balance) {
                    LOCK(this_wallet->cs_wallet);
                    this_wallet->m_is_staking = NOT_STAKING_BALANCE;
                    this_wallet->nLastCoinStakeSearchTime = nSearchTime + 60;
                    LogPrint(BCLog::POS, "%s: Wallet %d, low balance.\n", __func__, i);
                    UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::SHORTDELAY});
                    continue;
                }

                if (!pblocktemplate.get()) {
                    pblocktemplate = BlockAssembler(chainman.ActiveChainstate(), node, mempool, Params()).CreateNewBlock(coinbaseScript, true);
                    if (!pblocktemplate.get()) {
                        fIsStaking = false;
                        LogPrint(BCLog::POS, "%s: Couldn't create new block.\n", __func__);
                        UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::SHORTDELAY});
                        continue;
                    }
                }

                this_wallet->m_is_staking = IS_STAKING;
                fIsStaking = true;
                if (stakable_wallets[y].SignBlock(chainman.ActiveChainstate(), pblocktemplate.get(), nBestHeight + 1, nSearchTime)) {
                    CBlock *pblock = &pblocktemplate->block;
                    if (CheckStake(chainman, pblock)) {
                        foundBlock = true;
                        nTimeLastStake = GetTime();
                        UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::SHORTDELAY});
                        continue;
                    }
                }
                else
                {
                    int nRequiredDepth = COINBASE_MATURITY + 1;
                    LOCK(this_wallet->cs_wallet);
                    if (this_wallet->m_greatest_txn_depth < nRequiredDepth) {
                        this_wallet->m_is_staking = NOT_STAKING_DEPTH;
                        this_wallet->nLastCoinStakeSearchTime = nSearchTime + 60;
                        LogPrint(BCLog::POS, "%s: No outputs with required depth.\n", __func__);
                        UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::SHORTDELAY});
                        continue;
                    }
                }
            }
        }
    }
}

void ThreadStakeMiner(NodeContext& node)
{
    try
    {
        PoSMiner(node);
    }
    catch (std::exception& e) {
        PrintExceptionContinue(std::current_exception(), e.what());
    } catch (...) {
        PrintExceptionContinue(NULL, "ThreadStakeMiner()");
    }
}
