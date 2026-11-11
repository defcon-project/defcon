// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_COMMANDFLAGS_H
#define BITCOIN_RPC_COMMANDFLAGS_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * What a listed RPC command does, for people choosing one; the Qt Commands page
 * shows it. The table in commandflags.cpp has a row for every command the
 * command lists offer (CRPCCommand::listed(), the node's, the wallet's and the
 * ZMQ commands) and for nothing else, in every build: the rpc_command_flags test
 * holds it to exactly that set. A doubtful case errs towards caution, and the
 * page's words follow that ("can", "may").
 */
enum RPCCommandFlag : uint8_t {
    //! Some argument combination changes the wallet (keys, labels, locked
    //! coins, settings, unlock state), the node (bans, peers, settings, mempool,
    //! stopping) or sends something to the network. In-memory changes count:
    //! anything the call itself leaves behind that makes a later call of this
    //! or another command answer or behave differently, such as a time or a
    //! template the next call uses ("gobject list" sets where "gobject diff"
    //! starts, getblocktemplate keeps its template) or a stopped task
    //! ("scantxoutset abort"). Not counted: housekeeping the node also does on
    //! its own, done earlier by the call (listbanned drops expired bans, as the
    //! periodic ban-list dump does; gettxoutsetinfo flushes the coin cache;
    //! savemempool writes mempool.dat, as shutdown does), writing the wallet to
    //! a file the user names (backupwallet, dumpwallet), and the node's own
    //! counters, timings and logs.
    CMDFLAG_CHANGES = 1 << 0,
    //! It needs a loaded wallet: without one it refuses (or, like setstaking,
    //! finds nothing to act on). A command that only uses a wallet when one is
    //! there is not flagged.
    CMDFLAG_WALLET = 1 << 1,
    //! Some path refuses because the wallet is locked: an
    //! RPC_WALLET_UNLOCK_NEEDED error, or another refusal for a locked wallet
    //! (setstaking answers false). Implies CMDFLAG_WALLET.
    CMDFLAG_UNLOCK = 1 << 2,
    //! Every use of it makes proof-of-work blocks, which the chain accepts only
    //! up to Consensus::Params::lastPowBlock: above it the command fails.
    //! Implies CMDFLAG_CHANGES.
    CMDFLAG_BEFORE_POS = 1 << 3,
    //! It has listed subcommands, and CRPCTable::execute hands "parent sub ..."
    //! to the subcommand, so what it does depends on the one given. Carries no
    //! other flag.
    CMDFLAG_SUBCOMMANDS = 1 << 4,
    //! A masternode answers it without a wallet, from its own state
    //! (masternodes run with the wallet disabled); elsewhere CMDFLAG_WALLET
    //! holds. Implies CMDFLAG_WALLET.
    CMDFLAG_MASTERNODE_NO_WALLET = 1 << 5,
};

//! The flags of a listed command by its full name ("protx register"), or
//! nullopt for a name the table does not have.
std::optional<uint8_t> GetRPCCommandFlags(const std::string& name);

//! The whole table in name order, as this build has it.
std::vector<std::pair<std::string, uint8_t>> RPCCommandFlagTable();

#endif // BITCOIN_RPC_COMMANDFLAGS_H
