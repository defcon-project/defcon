// Copyright (c) 2018-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_LLMQ_OPTIONS_H
#define BITCOIN_LLMQ_OPTIONS_H

#include <llmq/params.h>
#include <gsl/pointers.h>

#include <map>
#include <optional>
#include <vector>

class CBlockIndex;
class CSporkManager;
namespace Consensus { struct Params; }

namespace llmq
{

enum class QvvecSyncMode {
    Invalid = -1,
    Always = 0,
    OnlyIfTypeMember = 1,
};

static constexpr bool DEFAULT_ENABLE_QUORUM_DATA_RECOVERY{true};

// If true, we will connect to all new quorums and watch their communication
static constexpr bool DEFAULT_WATCH_QUORUMS{true};

bool IsAllMembersConnectedEnabled(const Consensus::LLMQType llmqType, const CSporkManager& sporkman);
bool IsQuorumPoseEnabled(const Consensus::LLMQType llmqType, const CSporkManager& sporkman);

bool IsQuorumRotationEnabled(const Consensus::LLMQParams& llmqParams, gsl::not_null<const CBlockIndex*> pindex);

/// Returns the state of `-llmq-data-recovery`
bool QuorumDataRecoveryEnabled();

/// Returns the state of `-watchquorums`
bool IsWatchQuorumsEnabled();

/// Returns the parsed entries given by `-llmq-qvvec-sync`
std::map<Consensus::LLMQType, QvvecSyncMode> GetEnabledQuorumVvecSyncEntries();

bool IsQuorumTypeEnabled(Consensus::LLMQType llmqType, gsl::not_null<const CBlockIndex*> pindexPrev);

/** Resolve which LLMQ profile signs and verifies the ChainLock for a given
 *  signed height. One-way and height-only by design: signing and verification
 *  must agree from the signed height alone, never from live masternode
 *  counts, sporks or local configuration. */
[[nodiscard]] Consensus::LLMQType GetChainLocksLLMQType(const ::Consensus::Params& params, int nSignedHeight);

bool IsQuorumTypeEnabledInternal(Consensus::LLMQType llmqType, gsl::not_null<const CBlockIndex*> pindexPrev, std::optional<bool> optDIP0024IsActive, std::optional<bool> optHaveDIP0024Quorums);

std::vector<Consensus::LLMQType> GetEnabledQuorumTypes(gsl::not_null<const CBlockIndex*> pindex);
std::vector<std::reference_wrapper<const Consensus::LLMQParams>> GetEnabledQuorumParams(gsl::not_null<const CBlockIndex*> pindex);

} // namespace llmq

#endif // BITCOIN_LLMQ_OPTIONS_H
