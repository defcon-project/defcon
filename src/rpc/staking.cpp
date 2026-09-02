// Copyright (c) 2025 The DeFCoN Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainparams.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <node/context.h>
#include <pos/kernel.h>
#include <pos/minter.h>
#include <pos/multiwallet.h>
#include <pos/stake.h>
#include <primitives/transaction.h>
#include <rpc/register.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <univalue.h>
#include <validation.h>
#include <warnings.h>

extern int stakable_sz;
extern RecursiveMutex stakable_mutex;
extern std::vector<CStakeWallet> stakable_wallets;

static RPCHelpMan getstakinginfo()
{
    return RPCHelpMan{"getstakinginfo",
                "\nReturns an object containing staking-related information.",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::BOOL, "staking", "Whether this wallet's staking switch is on"},
                        {RPCResult::Type::BOOL, "minter_running", "Whether the staking thread is inside its loop right now. Node-wide, not per wallet: one minter serves every staking wallet, so a false here means nothing is being staked whatever the fields above report."},
                        {RPCResult::Type::NUM_TIME, "minter_running_since", /*optional=*/true, "When the minter's current uninterrupted run began (UNIX epoch seconds). Absent when it is not running."},
                        {RPCResult::Type::STR, "last_error", /*optional=*/true, "The minter's last failure. Node-wide, absent when it has never failed, and a HIGH-WATER MARK rather than a live status: nothing clears it, so that a fault which recurs stays visible instead of disappearing between failures. To tell a live fault from one already survived, compare last_error_time against minter_running_since -- an error stamped before the current run began has been recovered from."},
                        {RPCResult::Type::NUM_TIME, "last_error_time", /*optional=*/true, "When that failure happened (UNIX epoch seconds). Present only with last_error."},
                        {RPCResult::Type::STR, "errors", "Error messages"},
                        {RPCResult::Type::NUM, "pooledtx", "The size of the mempool"},
                        {RPCResult::Type::NUM, "difficulty", "The current difficulty"},
                        {RPCResult::Type::NUM, "search-interval", "The staker search interval"},
                        {RPCResult::Type::NUM, "weight", "The staker weight"},
                        {RPCResult::Type::NUM, "netstakeweight", "Network stake weight"},
                        {RPCResult::Type::NUM, "expectedtime", "Expected time to earn reward"},
                        {RPCResult::Type::OBJ, "excluded", /*optional=*/true, "Coins the staking rules keep out, by reason. Absent when none are.",
                        {
                            {RPCResult::Type::STR_AMOUNT, "immature", /*optional=*/true, "Rewards not yet deep enough to spend"},
                            {RPCResult::Type::STR_AMOUNT, "bls", /*optional=*/true, "Held at a BLS address, which cannot stake"},
                            {RPCResult::Type::STR_AMOUNT, "too_small", /*optional=*/true, "Under the minimum stakeable amount"},
                            {RPCResult::Type::STR_AMOUNT, "too_large", /*optional=*/true, "Over the maximum stakeable amount"},
                            {RPCResult::Type::STR_AMOUNT, "collateral_amount", /*optional=*/true, "Exactly a masternode collateral amount"},
                            {RPCResult::Type::STR_AMOUNT, "too_young", /*optional=*/true, "Not yet old enough to stake"},
                            {RPCResult::Type::STR_AMOUNT, "too_old", /*optional=*/true, "Older than the stake age limit"},
                        }},
                    }
                },
                RPCExamples{
                    HelpExampleCli("getstakinginfo", "")
            + HelpExampleRpc("getstakinginfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    CTxMemPool& mempool = EnsureAnyMemPool(request.context);
    const Consensus::Params& consensusParams = Params().GetConsensus();

    // Everything the chain has to answer, taken in one short lock and copied
    // out; nothing below this block touches cs_main again.
    //
    // This RPC used to hold cs_main across its whole body, which put it on the
    // wrong side of the tree's lock order. Everywhere else the wallet lock is
    // taken first and the chain lock from underneath it -- rpc/evo.cpp:1386
    // writes that order out as LOCK2(wallet->cs_wallet, cs_main), and the
    // wallet reaches the chain only through interfaces::Chain, which takes
    // cs_main at the bottom. This was the single site that inverted it, and
    // both halves of the resulting AB-BA pair shipped: this call held cs_main
    // and then wanted cs_wallet inside GetStakeWeight, while listunspent holds
    // cs_wallet and wants cs_main inside checkFinalTx. Two plain blocking locks
    // with no timeout, on a window wide enough to meet in practice -- and the
    // outcome of meeting is a node that stops, not one that slows down.
    //
    // The duration mattered too, though it was the milder half. On a wallet
    // with a few thousand unspent outputs the body measured 227-286 ms against
    // a 10 ms baseline, and ConnectTip, ActivateBestChainStep and AcceptBlock
    // all require cs_main, so a block arriving mid-call waited that long.
    int tip_height;
    int64_t tip_time;
    double tip_difficulty;
    uint64_t nNetworkWeight;
    {
        LOCK(cs_main);
        CBlockIndex* pindex = chainman.ActiveChainstate().m_chain.Tip();
        if (pindex == nullptr) {
            throw JSONRPCError(RPC_IN_WARMUP, "Chain tip not available yet");
        }
        tip_height = pindex->nHeight;
        tip_time = pindex->GetBlockTime();
        tip_difficulty = GetDifficulty(pindex);
        nNetworkWeight = GetPoSKernelPS(pindex, consensusParams);
    }

    uint64_t nWeight;
    uint64_t nExpectedTime;
    uint64_t lastCoinStakeSearchInterval;
    UniValue obj(UniValue::VOBJ);

    // The miner's maintenance rebuilds this vector on its own thread.
    LOCK(stakable_mutex);

    //multiwallet loop
    for (int y = 0; y < stakable_sz; y++)
    {
        const std::shared_ptr<CWallet> this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;

        // Resolved from the height being mined, which is the one after the
        // tip and the same height ExplainExcludedCoins is given below. Read a
        // block apart, the two halves of this answer could describe different
        // rules across an activation height -- which is why both are given the
        // one snapshot taken above rather than each reading the tip again.
        //
        // No cs_main here: GetStakeWeight takes cs_wallet and reaches the chain
        // from underneath it, which is the order the rest of the tree keeps.
        nWeight = stakable_wallets[y].GetStakeWeight(tip_time, tip_height + 1);
        lastCoinStakeSearchInterval = this_wallet->nLastCoinStakeSearchTime;

        int64_t nTargetSpacing = consensusParams.posTargetSpacing;
        // In 256-bit arithmetic: the 64-bit product of spacing and network
        // weight was within two per cent of wrapping on the devnet, and would
        // have wrapped to a small, believable number rather than an error.
        nExpectedTime = ExpectedStakeTime(nTargetSpacing, nNetworkWeight, nWeight);

        UniValue obj2(UniValue::VOBJ);
        obj2.pushKV("name", this_wallet->GetName());
        // A real JSON boolean: the help has always documented BOOL here, while
        // the ternary produced const char* and so a quoted string.
        obj2.pushKV("staking", stakable_wallets[y].CanStake());

        // Node-wide, and repeated on every wallet on purpose: one minter serves
        // them all, so a wallet's own switch says nothing about whether anything
        // is trying to stake it. Reported here rather than as a sibling key so
        // the response stays a map of wallet ids and no existing reader breaks.
        //
        // "errors" below is GetWarnings("statusbar") -- the node's general
        // warning text, which never carried the minter's failure and still
        // does not. last_error is where that now lives.
        obj2.pushKV("minter_running", fMinterRunning.load());
        const int64_t running_since = MinterRunningSince();
        if (running_since != 0) {
            obj2.pushKV("minter_running_since", running_since);
        }
        const std::string minter_error = MinterLastError();
        if (!minter_error.empty()) {
            obj2.pushKV("last_error", minter_error);
            obj2.pushKV("last_error_time", MinterLastErrorTime());
        }

        obj2.pushKV("errors", GetWarnings("statusbar").original);
        obj2.pushKV("pooledtx", (uint64_t)mempool.size());
        obj2.pushKV("difficulty", tip_difficulty);
        obj2.pushKV("search-interval", (int)lastCoinStakeSearchInterval);
        obj2.pushKV("weight", (uint64_t)nWeight);
        obj2.pushKV("netstakeweight", (uint64_t)nNetworkWeight);
        if (nWeight > 0) {
            obj2.pushKV("expectedtime", nExpectedTime);
        }

        // A full balance next to a weight of zero used to have no explanation
        // anywhere. Report what the rules held back, and only what they held
        // back, so an empty field means there is nothing to explain.
        // The tip's time and the height after it, matching GetStakeWeight above:
        // age is measured against a candidate block, and the tip's timestamp is
        // the closest one the node can state rather than guess.
        const StakeSkipReport skipped = stakable_wallets[y].ExplainExcludedCoins(tip_time, tip_height + 1);
        if (skipped.Total() > 0) {
            UniValue excluded(UniValue::VOBJ);
            if (skipped.immature > 0)   excluded.pushKV("immature", ValueFromAmount(skipped.immature));
            if (skipped.bls > 0)        excluded.pushKV("bls", ValueFromAmount(skipped.bls));
            if (skipped.below_min > 0)  excluded.pushKV("too_small", ValueFromAmount(skipped.below_min));
            if (skipped.above_max > 0)  excluded.pushKV("too_large", ValueFromAmount(skipped.above_max));
            if (skipped.collateral > 0) excluded.pushKV("collateral_amount", ValueFromAmount(skipped.collateral));
            if (skipped.too_young > 0)  excluded.pushKV("too_young", ValueFromAmount(skipped.too_young));
            if (skipped.too_old > 0)    excluded.pushKV("too_old", ValueFromAmount(skipped.too_old));
            obj2.pushKV("excluded", excluded);
        }

        obj.pushKV(std::to_string(y), obj2);
    }

    return obj;
},
    };
}

static RPCHelpMan liststakingwallets()
{
    return RPCHelpMan{"liststakingwallets",
        "\nReturns a list of staking-capable wallets.",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::OBJ, "id", "List of wallet ids for loaded wallets.",
                {
                    {RPCResult::Type::STR, "name", "Wallet name"},
                    {RPCResult::Type::BOOL, "enabled", "Wallet staking status"},
                    {RPCResult::Type::STR, "balance", "Wallet current balance"}
                }},
            }},
        RPCExamples{
            HelpExampleCli("liststakingwallets", "")
    + HelpExampleCli("liststakingwallets", "")
    + HelpExampleRpc("liststakingwallets", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    UniValue obj(UniValue::VOBJ);

    // The miner's maintenance rebuilds this vector on its own thread.
    LOCK(stakable_mutex);

    //multiwallet loop
    for (int y = 0; y < stakable_sz; y++)
    {
        UniValue obj2(UniValue::VOBJ);
        const std::shared_ptr<CWallet> this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;
        obj2.pushKV("name", this_wallet->GetName());
        obj2.pushKV("enabled", stakable_wallets[y].CanStake());
        obj2.pushKV("balance", FormatMoney(this_wallet->GetAvailableBalance() - this_wallet->nReserveBalance));
        obj.pushKV(std::to_string(y), obj2);
    }

    return obj;
},
    };
}

static RPCHelpMan setstaking()
{
    return RPCHelpMan{"setstaking",
        "\nToggles staking status on a staking wallet.",
        {
            {"id", RPCArg::Type::NUM, RPCArg::Optional::NO, "The walletid (as shown by liststakingwallets)."},
        },
        RPCResult{
            RPCResult::Type::BOOL, "enabled", "Wallet staking status"},
        RPCExamples{
            HelpExampleCli("setstaking", "1")
    + HelpExampleRpc("setstaking", "1")
        },
    [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    int walletid = 0;
    if (!request.params[0].isNull())
        walletid = request.params[0].get_int();

    std::string name;
    {
        LOCK(stakable_mutex);
        // The old bound let walletid == stakable_sz fall through the loop and
        // answer "disabled" for a wallet that does not exist.
        if (walletid < 0 || walletid >= stakable_sz)
            return NullUniValue;
        name = stakable_wallets[walletid].GetName();
    }

    // One enable path for RPC and GUI alike; the coinstake-descriptor
    // preparation lives inside the toggle.
    return ToggleWalletStaking(name);
},
    };
}


void RegisterStakingRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] =
{ //  category              actor (function)
  //  --------------------- ------------------------
    { "staking",            &getstakinginfo,                     },
    { "staking",            &liststakingwallets,                 },
    { "staking",            &setstaking,                         },
};
// clang-format on
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
