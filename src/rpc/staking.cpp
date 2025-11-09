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

    //multiwallet loop
    for (int y = 0; y < stakable_sz; y++)
    {
        CWallet* this_wallet = stakable_wallets[y].GetWallet();
        if (!this_wallet)
            continue;

        {
            LOCK(cs_main);
            pindex = active_chain.Tip();
            nWeight = stakable_wallets[y].GetStakeWeight(pindex->GetBlockTime(), pindex->nHeight);
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
    //multiwallet loop
    int walletid = 0;
    if (!request.params[0].isNull())
        walletid = request.params[0].get_int();
    if (walletid > stakable_sz)
        return NullUniValue;

    bool status = false;
    for (int y = 0; y < stakable_sz; y++)
    {
        if (y == walletid) {
            if (stakable_wallets[y].CanStake()) {
                status = false;
                stakable_wallets[y].StakingDisabled();
            } else {
                status = true;
                stakable_wallets[y].StakingEnabled();
            }
            break;
        }
    }

    return status;
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
