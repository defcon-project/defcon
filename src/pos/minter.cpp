// Copyright (c) 2025 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/minter.h>

#include <masternode/sync.h>
#include <pos/multiwallet.h>
#include <sync.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <net.h>

#include <map>

extern int stakable_sz;
extern RecursiveMutex stakable_mutex;
extern std::vector<CStakeWallet> stakable_wallets;

std::thread m_staking_thread;

std::atomic<bool> fStopMinerProc(false);
std::atomic<bool> fTryToSync(false);
std::atomic<bool> fIsStaking(false);
std::atomic<int64_t> nTimeLastStake(0);
std::atomic<bool> fMinterRunning(false);

namespace {
Mutex g_minter_error_mutex;
std::string g_minter_last_error GUARDED_BY(g_minter_error_mutex);
int64_t g_minter_last_error_time GUARDED_BY(g_minter_error_mutex){0};
} // namespace

void SetMinterLastError(const std::string& what)
{
    LOCK(g_minter_error_mutex);
    g_minter_last_error = what;
    g_minter_last_error_time = GetTime();
}

std::string MinterLastError()
{
    LOCK(g_minter_error_mutex);
    return g_minter_last_error;
}

int64_t MinterLastErrorTime()
{
    LOCK(g_minter_error_mutex);
    return g_minter_last_error_time;
}

namespace {
/**
 * When each wallet was last examined for permanently-excluded value.
 *
 * Keyed by name rather than index: maintenance rebuilds the stakable vector and
 * the indices move, while the name is what an operator reads in the message.
 */
Mutex g_excluded_scan_mutex;
std::map<std::string, int64_t> g_excluded_scanned_at GUARDED_BY(g_excluded_scan_mutex);

constexpr int64_t EXCLUDED_SCAN_INTERVAL = 60 * 60;

/**
 * Say, at ordinary log level, that a wallet holds value the staking rules will
 * not release.
 *
 * The breakdown itself already existed, but only through getstakinginfo -- so
 * it answered whoever thought to ask, and a wallet could carry a large
 * permanently unstakeable balance for months while looking entirely normal.
 * One did: 700,800 DFCN, found only because somebody went looking for it.
 */
void MaybeWarnAboutExcludedValue(const std::string& name, const CStakeWallet& wallet,
                                 int height, CAmount min_stake_value)
{
    const int64_t now = GetTime();
    {
        LOCK(g_excluded_scan_mutex);
        auto it = g_excluded_scanned_at.find(name);
        if (it != g_excluded_scanned_at.end() && now - it->second < EXCLUDED_SCAN_INTERVAL) return;
        // Stamped before the scan rather than after, and only on the way to a
        // scan: ExplainExcludedCoins walks every coin the wallet holds, and the
        // miner arrives here every 2.5 seconds. Rate-limiting the message alone
        // would leave that walk running on every tick.
        g_excluded_scanned_at[name] = now;
    }

    const StakeSkipReport report = wallet.ExplainExcludedCoins(height);
    if (!ShouldWarnAboutExcludedValue(report, min_stake_value)) return;

    // LogPrintf, not LogPrint(BCLog::POS, ...). The point of the warning is to
    // reach an operator who did not know to turn staking debug logging on.
    LogPrintf("Staking: wallet %s holds %s that the staking rules will not release (%s). "
              "This does not clear on its own; the coins have to be spent into a stakeable shape.\n",
              name, FormatMoney(PermanentlyExcluded(report)), DescribePermanentExclusions(report));
}
} // namespace

bool CheckStake(ChainstateManager& chainman, CBlock *pblock)
{
    uint256 proofHash, hashTarget;
    uint256 hashBlock = pblock->GetHash();

    if (!pblock->IsProofOfStake()) {
        LogPrint(BCLog::POS, "%s: %s is not a proof-of-stake block.", __func__, hashBlock.GetHex());
        return false;
    }

    {
        // m_block_index is GUARDED_BY(cs_main) and CheckProofOfStake is declared
        // EXCLUSIVE_LOCKS_REQUIRED(cs_main); the lookup, the chain-membership test and the
        // tip comparison below must all see one consistent view. GCC does not run Clang's
        // thread-safety analysis, so nothing flagged the missing lock. (cs_main is
        // recursive, so the LOCK inside CheckProofOfStake stays valid.)
        LOCK(cs_main);
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

        // Work from a copy. The GUI and RPC threads flip entries and the
        // maintenance above rebuilds the vector under stakable_mutex, while
        // one staking attempt below can take seconds -- holding the lock
        // across it would stall every toggle, and reading the globals
        // without it is a data race.
        std::vector<CStakeWallet> wallets_snapshot;
        {
            LOCK(stakable_mutex);
            wallets_snapshot = stakable_wallets;
        }

        bool foundBlock{false};
        CScript coinbaseScript;
        for (int y = 0; y < (int)wallets_snapshot.size(); y++)
        {
            if (fStopMinerProc)
                return;
            if (!wallets_snapshot[y].CanStake())
                continue;
            CWallet* this_wallet = wallets_snapshot[y].GetWallet();
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

                continue;
            }

            LogPrint(BCLog::POS, "%s - using wallet %d\n", __func__, y);

            // Placed after the sync, PoW-boundary and masternode-sync gates, so
            // anything still held back at this point is held back by the staking
            // rules themselves rather than by the state of the chain.
            MaybeWarnAboutExcludedValue(this_wallet->GetName(), wallets_snapshot[y],
                                        nBestHeight + 1, params.GetConsensus().stakeValueRange[0]);

            std::unique_ptr<CBlockTemplate> pblocktemplate;

            CAmount reserve_balance;
            {
                {
                    LOCK(this_wallet->cs_wallet);
                    if (nSearchTime <= this_wallet->nLastCoinStakeSearchTime) {
                        continue;
                    }

                    if (this_wallet->IsLocked()) {
                        this_wallet->m_is_staking = NOT_STAKING_LOCKED;
                        LogPrint(BCLog::POS, "%s: Wallet %d, locked wallet.\n", __func__, y);
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
                    LogPrint(BCLog::POS, "%s: Wallet %d, low balance.\n", __func__, y);
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
                if (wallets_snapshot[y].SignBlock(chainman.ActiveChainstate(), pblocktemplate.get(), nBestHeight + 1, nSearchTime)) {
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

namespace {
/**
 * Holds fMinterRunning true for exactly as long as the miner is inside its
 * loop -- including on the way out through an exception, which is the case
 * that matters. Setting the flag by hand after PoSMiner returns would leave
 * it stuck true for the one exit that means the miner stopped working.
 */
struct MinterRunningScope {
    MinterRunningScope() { fMinterRunning = true; }
    ~MinterRunningScope() { fMinterRunning = false; }
};
} // namespace

void ThreadStakeMiner(NodeContext& node)
{
    // Restart the miner after an unexpected failure rather than returning.
    //
    // This used to catch once and fall out of the function, which ended staking
    // for the lifetime of the process: the node kept running, getstakinginfo
    // kept reporting a healthy staking wallet, and no block was ever produced
    // again. A fault that recurs is at least visible in the log now, and a
    // transient one no longer costs the node its ability to stake.
    //
    // Restarting moved the problem rather than removing it, though: a node that
    // fails on every attempt now retries forever, and from RPC that is still
    // indistinguishable from one that simply has not won a block yet. So each
    // failure is recorded where getstakinginfo can report it, not only logged.
    while (!fStopMinerProc) {
        try {
            MinterRunningScope running;
            PoSMiner(node);
            return; // clean return: shutdown was requested
        } catch (const std::exception& e) {
            SetMinterLastError(e.what());
            PrintExceptionContinue(std::current_exception(), e.what());
        } catch (...) {
            // Not a std::exception -- a thrown UniValue, for instance, which is
            // what JSONRPCError produces. There is no message to recover, so
            // say what happened instead of printing a bare null.
            static const char* kOpaque = "staking failed with a non-standard exception "
                                         "(a thrown UniValue, for instance) and will be retried";
            SetMinterLastError(kOpaque);
            PrintExceptionContinue(nullptr, "ThreadStakeMiner(): staking failed and will be retried");
        }

        if (fStopMinerProc) break;
        // Never spin: a permanent fault would otherwise fill the log as fast as
        // the disk allows.
        UninterruptibleSleep(std::chrono::milliseconds{CStakeWallet::LARGEDELAY});
    }
}
