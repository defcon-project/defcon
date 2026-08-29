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
                        {RPCResult::Type::BOOL, "staking", "'true' if wallet is currently staking"},
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
    LOCK(cs_main);
    CChainState& active_chainstate = chainman.ActiveChainstate();
    CChain& active_chain = active_chainstate.m_chain;

    CBlockIndex* pindex;
    uint64_t nWeight;
    uint64_t nExpectedTime;
    uint64_t lastCoinStakeSearchInterval;
    UniValue obj(UniValue::VOBJ);

    // The miner's maintenance rebuilds this vector on its own thread.
    LOCK(stakable_mutex);

    //multiwallet loop
    for (int y = 0; y < stakable_sz; y++)
    {
        CWallet* this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;

        {
            LOCK(cs_main);
            pindex = active_chain.Tip();
            // Resolved from the height being mined, which is the one after
            // the tip and the same height ExplainExcludedCoins is given below.
            // Read a block apart, the two halves of this answer could describe
            // different rules across an activation height.
            nWeight = stakable_wallets[y].GetStakeWeight(pindex->GetBlockTime(), pindex->nHeight + 1);
            lastCoinStakeSearchInterval = this_wallet->nLastCoinStakeSearchTime;
        }

        const Consensus::Params& consensusParams = Params().GetConsensus();
        uint64_t nNetworkWeight = GetPoSKernelPS(pindex, consensusParams);
        int64_t nTargetSpacing = consensusParams.posTargetSpacing;
        nExpectedTime = 0;
        if (nWeight > 0) {
            nExpectedTime = nTargetSpacing * nNetworkWeight / nWeight;
        }

        UniValue obj2(UniValue::VOBJ);
        obj2.pushKV("name", this_wallet->GetName());
        obj2.pushKV("staking", stakable_wallets[y].CanStake() ? "true" : "false");
        obj2.pushKV("errors", GetWarnings("statusbar").original);
        obj2.pushKV("pooledtx", (uint64_t)mempool.size());
        obj2.pushKV("difficulty", GetDifficulty(pindex));
        obj2.pushKV("search-interval", (int)lastCoinStakeSearchInterval);
        obj2.pushKV("weight", (uint64_t)nWeight);
        obj2.pushKV("netstakeweight", (uint64_t)nNetworkWeight);
        if (nWeight > 0) {
            obj2.pushKV("expectedtime", nExpectedTime);
        }

        // A full balance next to a weight of zero used to have no explanation
        // anywhere. Report what the rules held back, and only what they held
        // back, so an empty field means there is nothing to explain.
        const StakeSkipReport skipped = stakable_wallets[y].ExplainExcludedCoins(pindex->nHeight + 1);
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
        CWallet* this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;
        obj2.pushKV("name", this_wallet->GetName());
        obj2.pushKV("enabled", stakable_wallets[y].CanStake() ? "true" : "false");
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
