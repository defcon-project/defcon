// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <rpc/commandflags.h>

#include <algorithm>
#include <cstring>
#include <iterator>

namespace {
struct CommandFlags {
    const char* name;
    uint8_t flags;
};

// One row per listed command, in name order (byte order: a parent before its
// subcommands, "_" and "-" before letters). A row sits under the same build
// condition as the command's registration; where the flags themselves differ by
// build, both rows are written out. Every row follows the rules in
// commandflags.h.
// clang-format off
constexpr CommandFlags ROWS[] = {
#ifdef ENABLE_WALLET
    {"abandontransaction", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"abortrescan", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"addmultisigaddress", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
    {"addnode", CMDFLAG_CHANGES},
    {"analyzepsbt", 0},
#ifdef ENABLE_WALLET
    {"backupwallet", CMDFLAG_WALLET},
#endif
    {"bls", CMDFLAG_SUBCOMMANDS},
    {"bls fromsecret", 0},
#ifdef ENABLE_WALLET
    {"bls generate", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#else
    {"bls generate", 0},
#endif
    {"clearbanned", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"coinjoin", CMDFLAG_SUBCOMMANDS},
    {"coinjoin reset", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"coinjoin start", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"coinjoin stop", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"coinjoinsalt", CMDFLAG_SUBCOMMANDS},
    {"coinjoinsalt generate", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"coinjoinsalt get", CMDFLAG_WALLET},
    {"coinjoinsalt set", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"combineoutputs", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"combinepsbt", 0},
    {"combinerawtransaction", 0},
    {"converttopsbt", 0},
    {"createmultisig", 0},
    {"createpsbt", 0},
    {"createrawtransaction", 0},
#ifdef ENABLE_WALLET
    {"createwallet", CMDFLAG_CHANGES},
#endif
    {"debug", CMDFLAG_CHANGES},
    {"decodepsbt", 0},
    {"decoderawtransaction", 0},
    {"decodescript", 0},
    {"deriveaddresses", 0},
    {"disconnectnode", CMDFLAG_CHANGES},
    {"dslstatus", 0},
#ifdef ENABLE_WALLET
    {"dumphdinfo", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"dumpprivkey", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"dumpwallet", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"encryptwallet", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
    {"estimatesmartfee", 0},
    {"finalizepsbt", 0},
#ifdef ENABLE_WALLET
    {"fundrawtransaction", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
#if ENABLE_MINER
    {"generateblock", CMDFLAG_CHANGES | CMDFLAG_BEFORE_POS},
    {"generatetoaddress", CMDFLAG_CHANGES | CMDFLAG_BEFORE_POS},
    {"generatetodescriptor", CMDFLAG_CHANGES | CMDFLAG_BEFORE_POS},
#endif
    {"getaddednodeinfo", 0},
    {"getaddressbalance", 0},
    {"getaddressdeltas", 0},
#ifdef ENABLE_WALLET
    {"getaddressesbylabel", CMDFLAG_WALLET},
    {"getaddressinfo", CMDFLAG_WALLET},
#endif
    {"getaddressmempool", 0},
    {"getaddresstxids", 0},
    {"getaddressutxos", 0},
#ifdef ENABLE_WALLET
    {"getbalance", CMDFLAG_WALLET},
    {"getbalances", CMDFLAG_WALLET},
#endif
    {"getbestblockhash", 0},
    {"getbestchainlock", 0},
    {"getblock", 0},
    {"getblockchaininfo", 0},
    {"getblockcount", 0},
    {"getblockfilter", 0},
    {"getblockfrompeer", CMDFLAG_CHANGES},
    {"getblockhash", 0},
    {"getblockhashes", 0},
    {"getblockheader", 0},
    {"getblockheaders", 0},
    {"getblockstats", 0},
    {"getblocktemplate", CMDFLAG_CHANGES},
    {"getchaintips", 0},
    {"getchaintxstats", 0},
#ifdef ENABLE_WALLET
    {"getcoinjoininfo", CMDFLAG_WALLET | CMDFLAG_MASTERNODE_NO_WALLET},
#else
    {"getcoinjoininfo", 0},
#endif
    {"getconnectioncount", 0},
    {"getdescriptorinfo", 0},
    {"getdifficulty", 0},
    {"getgovernanceinfo", 0},
    {"getindexinfo", 0},
    {"getislocks", 0},
    {"getmemoryinfo", 0},
    {"getmempoolancestors", 0},
    {"getmempooldescendants", 0},
    {"getmempoolentry", 0},
    {"getmempoolinfo", 0},
    {"getmerkleblocks", 0},
    {"getmininginfo", 0},
    {"getnettotals", 0},
    {"getnetworkhashps", 0},
    {"getnetworkinfo", 0},
#ifdef ENABLE_WALLET
    {"getnewaddress", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
    {"getnodeaddresses", 0},
    {"getpeerinfo", 0},
#ifdef ENABLE_WALLET
    {"getrawchangeaddress", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
    {"getrawmempool", 0},
    {"getrawtransaction", 0},
    {"getrawtransactionmulti", 0},
#ifdef ENABLE_WALLET
    {"getreceivedbyaddress", CMDFLAG_WALLET},
    {"getreceivedbylabel", CMDFLAG_WALLET},
#endif
    {"getrpcinfo", 0},
    {"getspecialtxes", 0},
    {"getspentinfo", 0},
    {"getstakinginfo", 0},
    {"getsuperblockbudget", 0},
#ifdef ENABLE_WALLET
    {"gettransaction", CMDFLAG_WALLET},
#endif
    {"gettxchainlocks", 0},
    {"gettxout", 0},
    {"gettxoutproof", 0},
    {"gettxoutsetinfo", 0},
#ifdef ENABLE_WALLET
    {"getwalletinfo", CMDFLAG_WALLET},
#endif
#if ENABLE_ZMQ
    {"getzmqnotifications", 0},
#endif
    {"gobject", CMDFLAG_SUBCOMMANDS},
    {"gobject check", 0},
    {"gobject count", 0},
    {"gobject deserialize", 0},
    {"gobject diff", CMDFLAG_CHANGES},
    {"gobject get", 0},
    {"gobject getcurrentvotes", 0},
    {"gobject list", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"gobject list-prepared", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"gobject prepare", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"gobject submit", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"gobject vote-alias", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"gobject vote-many", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"help", 0},
#ifdef ENABLE_WALLET
    {"importaddress", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"importblsprivkey", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"importdescriptors", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"importelectrumwallet", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"importmulti", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"importprivkey", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"importprunedfunds", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"importpubkey", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"importwallet", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"joinpsbts", 0},
#ifdef ENABLE_WALLET
    {"keypoolrefill", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"listaddressbalances", CMDFLAG_WALLET},
    {"listaddressgroupings", CMDFLAG_WALLET},
#endif
    {"listbanned", 0},
#ifdef ENABLE_WALLET
    {"listblsaddresses", CMDFLAG_WALLET},
    {"listdescriptors", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"listlabels", CMDFLAG_WALLET},
    {"listlockunspent", CMDFLAG_WALLET},
    {"listreceivedbyaddress", CMDFLAG_WALLET},
    {"listreceivedbylabel", CMDFLAG_WALLET},
    {"listsinceblock", CMDFLAG_WALLET},
#endif
    {"liststakingwallets", 0},
#ifdef ENABLE_WALLET
    {"listtransactions", CMDFLAG_WALLET},
    {"listunspent", CMDFLAG_WALLET},
    {"listwalletdir", 0},
    {"listwallets", 0},
    {"loadwallet", CMDFLAG_CHANGES},
    {"lockunspent", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
    {"logging", CMDFLAG_CHANGES},
    {"masternode", CMDFLAG_SUBCOMMANDS},
    {"masternode connect", CMDFLAG_CHANGES},
    {"masternode count", 0},
    {"masternode list", 0},
#ifdef ENABLE_WALLET
    {"masternode outputs", CMDFLAG_WALLET},
#endif
    {"masternode payments", 0},
    {"masternode status", 0},
    {"masternode winners", 0},
    {"masternodelist", 0},
    {"mnsync", CMDFLAG_CHANGES},
    {"ping", CMDFLAG_CHANGES},
    {"preciousblock", CMDFLAG_CHANGES},
    {"prioritisetransaction", CMDFLAG_CHANGES},
    {"protx", CMDFLAG_SUBCOMMANDS},
    {"protx diff", 0},
    {"protx info", 0},
    {"protx list", 0},
    {"protx listdiff", 0},
#ifdef ENABLE_WALLET
    {"protx register", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_compute", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_fund", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_fund_compute", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_fund_legacy", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_legacy", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_prepare", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_prepare_compute", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_prepare_legacy", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx register_submit", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx revoke", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx update_registrar", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx update_registrar_legacy", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx update_service", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"protx update_service_compute", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"pruneblockchain", CMDFLAG_CHANGES},
    {"quorum", CMDFLAG_SUBCOMMANDS},
    {"quorum dkginfo", 0},
    {"quorum dkgstatus", 0},
    {"quorum getdata", CMDFLAG_CHANGES},
    {"quorum getrecsig", 0},
    {"quorum hasrecsig", 0},
    {"quorum info", 0},
    {"quorum isconflicting", 0},
    {"quorum list", 0},
    {"quorum listextended", 0},
    {"quorum memberof", 0},
    {"quorum selectquorum", 0},
    {"quorum sign", CMDFLAG_CHANGES},
    {"quorum verify", 0},
#ifdef ENABLE_WALLET
    {"removeprunedfunds", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"rescanblockchain", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"restorewallet", CMDFLAG_CHANGES},
#endif
    {"savemempool", 0},
    {"scantxoutset", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"send", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"sendmany", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"sendrawtransaction", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"sendtoaddress", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"setban", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"setcoinjoinamount", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"setcoinjoinrounds", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"sethdseed", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"setlabel", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
    {"setnetworkactive", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"setstaking", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#else
    {"setstaking", 0},
#endif
#ifdef ENABLE_WALLET
    {"settxfee", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"setwalletflag", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"signmessage", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"signmessagewithprivkey", 0},
    {"signrawtransactionwithkey", 0},
#ifdef ENABLE_WALLET
    {"signrawtransactionwithwallet", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"spork", 0},
    {"sporkupdate", CMDFLAG_CHANGES},
    {"stop", CMDFLAG_CHANGES},
    {"submitblock", CMDFLAG_CHANGES},
    {"submitchainlock", CMDFLAG_CHANGES},
    {"submitheader", CMDFLAG_CHANGES},
    {"testmempoolaccept", 0},
#ifdef ENABLE_WALLET
    {"unloadwallet", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"upgradetohd", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"upgradewallet", CMDFLAG_CHANGES | CMDFLAG_WALLET | CMDFLAG_UNLOCK},
#endif
    {"uptime", 0},
    {"utxoupdatepsbt", 0},
    {"validateaddress", 0},
    {"verifychain", 0},
    {"verifychainlock", 0},
    {"verifyislock", 0},
    {"verifymessage", 0},
    {"verifytxoutproof", 0},
    {"voteraw", CMDFLAG_CHANGES},
#ifdef ENABLE_WALLET
    {"walletcreatefundedpsbt", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"walletlock", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"walletpassphrase", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"walletpassphrasechange", CMDFLAG_CHANGES | CMDFLAG_WALLET},
    {"walletprocesspsbt", CMDFLAG_WALLET | CMDFLAG_UNLOCK},
    {"wipewallettxes", CMDFLAG_CHANGES | CMDFLAG_WALLET},
#endif
};
// clang-format on
} // namespace

std::optional<uint8_t> GetRPCCommandFlags(const std::string& name)
{
    const auto it = std::lower_bound(std::begin(ROWS), std::end(ROWS), name,
                                     [](const CommandFlags& row, const std::string& key) { return std::strcmp(row.name, key.c_str()) < 0; });
    if (it == std::end(ROWS) || name != it->name) return std::nullopt;
    return it->flags;
}

std::vector<std::pair<std::string, uint8_t>> RPCCommandFlagTable()
{
    std::vector<std::pair<std::string, uint8_t>> table;
    for (const CommandFlags& row : ROWS) table.emplace_back(row.name, row.flags);
    return table;
}
