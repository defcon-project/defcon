// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/utils.h>

#include <llmq/options.h>

#include <chainparams.h>
#include <deploymentstatus.h>
#include <evo/deterministicmns.h>
#include <evo/cbtx.h>
#include <evo/evodb.h>
#include <masternode/meta.h>
#include <net.h>
#include <random.h>
#include <util/irange.h>
#include <util/ranges.h>
#include <util/time.h>
#include <util/underlying.h>
#include <unordered_lru_cache.h>
#include <validation.h>

#include <atomic>
#include <optional>

class CBLSSignature;
namespace llmq {
class CQuorum;
using CQuorumPtr = std::shared_ptr<CQuorum>;
using CQuorumCPtr = std::shared_ptr<const CQuorum>;
} // namespace llmq

static bool IsV20Active(gsl::not_null<const CBlockIndex*> pindexPrev)
{
    return DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_V20);
}

namespace llmq {
namespace utils {
// Forward declarations
static std::vector<CDeterministicMNCPtr> ComputeQuorumMembers(Consensus::LLMQType llmqType, CDeterministicMNManager& dmnman,
                                                              const CBlockIndex* pQuorumBaseBlockIndex);
static uint256 GetHashModifierFromWorkBlock(const Consensus::LLMQParams& llmqParams,
                                            const CBlockIndex* pWorkBlockIndex)
{
    auto cbcl = GetNonNullCoinbaseChainlock(pWorkBlockIndex);
    if (cbcl.has_value()) {
        // We have a non-null CL signature: calculate modifier using this CL signature
        auto& [bestCLSignature, bestCLHeightDiff] = cbcl.value();
        return ::SerializeHash(std::make_tuple(llmqParams.type, pWorkBlockIndex->nHeight, bestCLSignature));
    }
    // No non-null CL signature found in coinbase: calculate modifier using block hash only
    return ::SerializeHash(std::make_pair(llmqParams.type, pWorkBlockIndex->GetBlockHash()));
}

static uint256 GetHashModifier(const Consensus::LLMQParams& llmqParams, gsl::not_null<const CBlockIndex*> pCycleQuorumBaseBlockIndex)
{
    ASSERT_IF_DEBUG(pCycleQuorumBaseBlockIndex->nHeight % llmqParams.dkgInterval == 0);
    const CBlockIndex* pWorkBlockIndex = pCycleQuorumBaseBlockIndex->GetAncestor(pCycleQuorumBaseBlockIndex->nHeight - 8);

    if (IsV20Active(pWorkBlockIndex)) {
        // v20 is active: calculate modifier using the new way.
        return GetHashModifierFromWorkBlock(llmqParams, pWorkBlockIndex);
    }

    // v20 isn't active yet: calculate modifier using the usual way
    return ::SerializeHash(std::make_pair(llmqParams.type, pCycleQuorumBaseBlockIndex->GetBlockHash()));
}

std::vector<CDeterministicMNCPtr> GetAllQuorumMembers(Consensus::LLMQType llmqType, CDeterministicMNManager& dmnman,
                                                      gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex,
                                                      bool reset_cache)
{
    static RecursiveMutex cs_members;
    static std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CDeterministicMNCPtr>, StaticSaltedHasher>> mapQuorumMembers GUARDED_BY(cs_members);
    // A parentless base index (genesis) can never host a quorum. IsQuorumTypeEnabled() handles the
    // null, but say so explicitly here: this is reached with an attacker-supplied quorumHash via
    // CFinalCommitment::Verify(), so it must reject rather than trip a precondition.
    if (pQuorumBaseBlockIndex->pprev == nullptr || !IsQuorumTypeEnabled(llmqType, pQuorumBaseBlockIndex->pprev)) {
        return {};
    }
    std::vector<CDeterministicMNCPtr> quorumMembers;
    {
        LOCK(cs_members);
        if (mapQuorumMembers.empty()) {
            InitQuorumsCache(mapQuorumMembers);
        }
        if (reset_cache) {
            mapQuorumMembers[llmqType].clear();
        } else if (mapQuorumMembers[llmqType].get(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers)) {
            return quorumMembers;
        }
    }

    quorumMembers = ComputeQuorumMembers(llmqType, dmnman, pQuorumBaseBlockIndex);

    LOCK(cs_members);
    mapQuorumMembers[llmqType].insert(pQuorumBaseBlockIndex->GetBlockHash(), quorumMembers);
    return quorumMembers;
}

std::vector<CDeterministicMNCPtr> ComputeQuorumMembers(Consensus::LLMQType llmqType, CDeterministicMNManager& dmnman,
                                                       const CBlockIndex* pQuorumBaseBlockIndex)
{
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    assert(llmq_params_opt.has_value());
    if (pQuorumBaseBlockIndex->nHeight % llmq_params_opt->dkgInterval != 0) {
        ASSERT_IF_DEBUG(false);
        return {};
    }

    const CBlockIndex* pWorkBlockIndex = IsV20Active(pQuorumBaseBlockIndex) ?
            pQuorumBaseBlockIndex->GetAncestor(pQuorumBaseBlockIndex->nHeight - 8) :
            pQuorumBaseBlockIndex;
    const auto modifier = GetHashModifier(llmq_params_opt.value(), pQuorumBaseBlockIndex);
    auto allMns = dmnman.GetListForBlock(pWorkBlockIndex);
    return allMns.CalculateQuorum(llmq_params_opt->size, modifier);
}

std::optional<std::vector<CDeterministicMNCPtr>> ComputeQuorumMembersFromWorkBlock(
    Consensus::LLMQType llmqType, CDeterministicMNManager& dmnman,
    gsl::not_null<const CBlockIndex*> pWorkBlockIndex, int quorumHeight)
{
    const auto& llmq_params_opt = Params().GetLLMQ(llmqType);
    if (!llmq_params_opt.has_value()) {
        ASSERT_IF_DEBUG(false);
        return std::nullopt;
    }
    const auto& llmq_params = llmq_params_opt.value();

    const int quorumIndex{quorumHeight % llmq_params.dkgInterval};
    const int cycleBaseHeight{quorumHeight - quorumIndex};
    if (quorumIndex != 0 ||
        pWorkBlockIndex->nHeight != cycleBaseHeight - 8) {
        ASSERT_IF_DEBUG(false);
        return std::nullopt;
    }

    if (!IsV20Active(pWorkBlockIndex)) {
        // The pre-v20 modifier needs the future (cycle) quorum base block hash, which does
        // not exist yet.
        return std::nullopt;
    }

    const auto modifier = GetHashModifierFromWorkBlock(llmq_params, pWorkBlockIndex.get());
    return dmnman.GetListForBlock(pWorkBlockIndex).CalculateQuorum(llmq_params.size, modifier);
}

uint256 DeterministicOutboundConnection(const uint256& proTxHash1, const uint256& proTxHash2)
{
    // We need to deterministically select who is going to initiate the connection. The naive way would be to simply
    // return the min(proTxHash1, proTxHash2), but this would create a bias towards MNs with a numerically low
    // hash. To fix this, we return the proTxHash that has the lowest value of:
    //   hash(min(proTxHash1, proTxHash2), max(proTxHash1, proTxHash2), proTxHashX)
    // where proTxHashX is the proTxHash to compare
    uint256 h1;
    uint256 h2;
    if (proTxHash1 < proTxHash2) {
        h1 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash1, proTxHash2, proTxHash2));
    } else {
        h1 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash1));
        h2 = ::SerializeHash(std::make_tuple(proTxHash2, proTxHash1, proTxHash2));
    }
    if (h1 < h2) {
        return proTxHash1;
    }
    return proTxHash2;
}

std::unordered_set<uint256, StaticSaltedHasher> GetQuorumConnections(
    const Consensus::LLMQParams& llmqParams, CDeterministicMNManager& dmnman,
    const CSporkManager& sporkman, gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex, const uint256& forMember,
    bool onlyOutbound)
{
    if (IsAllMembersConnectedEnabled(llmqParams.type, sporkman)) {
        auto mns = GetAllQuorumMembers(llmqParams.type, dmnman, pQuorumBaseBlockIndex);
        std::unordered_set<uint256, StaticSaltedHasher> result;

        for (const auto& dmn : mns) {
            if (dmn->proTxHash == forMember) {
                continue;
            }
            // Determine which of the two MNs (forMember vs dmn) should initiate the outbound connection and which
            // one should wait for the inbound connection. We do this in a deterministic way, so that even when we
            // end up with both connecting to each other, we know which one to disconnect
            uint256 deterministicOutbound = DeterministicOutboundConnection(forMember, dmn->proTxHash);
            if (!onlyOutbound || deterministicOutbound == dmn->proTxHash) {
                result.emplace(dmn->proTxHash);
            }
        }
        return result;
    }
    return GetQuorumRelayMembers(llmqParams, dmnman, pQuorumBaseBlockIndex, forMember, onlyOutbound);
}

std::unordered_set<uint256, StaticSaltedHasher> GetQuorumRelayMembers(
    const Consensus::LLMQParams& llmqParams, CDeterministicMNManager& dmnman,
    gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex, const uint256& forMember, bool onlyOutbound)
{
    auto mns = GetAllQuorumMembers(llmqParams.type, dmnman, pQuorumBaseBlockIndex);
    std::unordered_set<uint256, StaticSaltedHasher> result;

    auto calcOutbound = [&](size_t i, const uint256& proTxHash) {
        // Relay to nodes at indexes (i+2^k)%n, where
        //   k: 0..max(1, floor(log2(n-1))-1)
        //   n: size of the quorum/ring
        std::unordered_set<uint256, StaticSaltedHasher> r{};
        if (mns.size() == 1) {
            // No outbound connections are needed when there is one MN only.
            // Also note that trying to calculate results via the algorithm below
            // would result in an endless loop.
            return r;
        }
        int gap = 1;
        int gap_max = (int)mns.size() - 1;
        int k = 0;
        while ((gap_max >>= 1) || k <= 1) {
            size_t idx = (i + gap) % mns.size();
            // It doesn't matter if this node is going to be added to the resulting set or not,
            // we should always bump the gap and the k (step count) regardless.
            // Refusing to bump the gap results in an incomplete set in the best case scenario
            // (idx won't ever change again once we hit `==`). Not bumping k guarantees an endless
            // loop when the first or the second node we check is the one that should be skipped
            // (k <= 1 forever).
            gap <<= 1;
            k++;
            const auto& otherDmn = mns[idx];
            if (otherDmn->proTxHash == proTxHash) {
                continue;
            }
            r.emplace(otherDmn->proTxHash);
        }
        return r;
    };

    for (const auto i : irange::range(mns.size())) {
        const auto& dmn = mns[i];
        if (dmn->proTxHash == forMember) {
            auto r = calcOutbound(i, dmn->proTxHash);
            result.insert(r.begin(), r.end());
        } else if (!onlyOutbound) {
            auto r = calcOutbound(i, dmn->proTxHash);
            if (r.count(forMember)) {
                result.emplace(dmn->proTxHash);
            }
        }
    }

    return result;
}

std::set<size_t> CalcDeterministicWatchConnections(Consensus::LLMQType llmqType, gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex,
                                                   size_t memberCount, size_t connectionCount)
{
    static uint256 qwatchConnectionSeed;
    static std::atomic<bool> qwatchConnectionSeedGenerated{false};
    static RecursiveMutex qwatchConnectionSeedCs;
    if (!qwatchConnectionSeedGenerated) {
        LOCK(qwatchConnectionSeedCs);
        qwatchConnectionSeed = GetRandHash();
        qwatchConnectionSeedGenerated = true;
    }

    std::set<size_t> result;
    uint256 rnd = qwatchConnectionSeed;
    for ([[maybe_unused]] const auto _ : irange::range(connectionCount)) {
        rnd = ::SerializeHash(std::make_pair(rnd, std::make_pair(llmqType, pQuorumBaseBlockIndex->GetBlockHash())));
        result.emplace(rnd.GetUint64(0) % memberCount);
    }
    return result;
}

bool EnsureQuorumConnections(const Consensus::LLMQParams& llmqParams, CConnman& connman,
                             CDeterministicMNManager& dmnman, const CSporkManager& sporkman,
                             const CDeterministicMNList& tip_mn_list,
                             gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex, const uint256& myProTxHash,
                             bool is_masternode)
{
    if (!is_masternode && !IsWatchQuorumsEnabled()) {
        return false;
    }

    auto members = GetAllQuorumMembers(llmqParams.type, dmnman, pQuorumBaseBlockIndex);
    if (members.empty()) {
        return false;
    }

    bool isMember = ranges::find_if(members, [&](const auto& dmn) { return dmn->proTxHash == myProTxHash; }) != members.end();

    if (!isMember && !IsWatchQuorumsEnabled()) {
        return false;
    }

    LogPrint(BCLog::NET_NETCONN, "%s -- isMember=%d for quorum %s:\n",
            __func__, isMember, pQuorumBaseBlockIndex->GetBlockHash().ToString());

    std::unordered_set<uint256, StaticSaltedHasher> connections;
    std::unordered_set<uint256, StaticSaltedHasher> relayMembers;
    if (isMember) {
        connections = GetQuorumConnections(llmqParams, dmnman, sporkman, pQuorumBaseBlockIndex, myProTxHash,
                                           true);
        relayMembers = GetQuorumRelayMembers(llmqParams, dmnman, pQuorumBaseBlockIndex, myProTxHash, true);
    } else {
        auto cindexes = CalcDeterministicWatchConnections(llmqParams.type, pQuorumBaseBlockIndex, members.size(), 1);
        for (auto idx : cindexes) {
            connections.emplace(members[idx]->proTxHash);
        }
        relayMembers = connections;
    }
    if (!connections.empty()) {
        if (!connman.HasMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash()) &&
            LogAcceptDebug(BCLog::LLMQ)) {
            std::string debugMsg = strprintf("%s -- adding masternodes quorum connections for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
            for (const auto& c : connections) {
                auto dmn = tip_mn_list.GetValidMN(c);
                if (!dmn) {
                    debugMsg += strprintf("  %s (not in valid MN set anymore)\n", c.ToString());
                } else {
                    debugMsg += strprintf("  %s (%s)\n", c.ToString(), dmn->pdmnState->addr.ToStringAddrPort());
                }
            }
            LogPrint(BCLog::NET_NETCONN, debugMsg.c_str()); /* Continued */
        }
        connman.SetMasternodeQuorumNodes(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), connections);
    }
    if (!relayMembers.empty()) {
        connman.SetMasternodeQuorumRelayMembers(llmqParams.type, pQuorumBaseBlockIndex->GetBlockHash(), relayMembers);
    }
    return true;
}

void AddQuorumProbeConnections(const Consensus::LLMQParams& llmqParams, CConnman& connman, CDeterministicMNManager& dmnman,
                               CMasternodeMetaMan& mn_metaman,
                               const CSporkManager& sporkman, const CDeterministicMNList& tip_mn_list,
                               gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex, const uint256& myProTxHash)
{
    assert(mn_metaman.IsValid());

    if (!IsQuorumPoseEnabled(llmqParams.type, sporkman)) {
        return;
    }

    auto members = GetAllQuorumMembers(llmqParams.type, dmnman, pQuorumBaseBlockIndex);
    auto curTime = GetTime<std::chrono::seconds>().count();

    std::set<uint256> probeConnections;
    for (const auto& dmn : members) {
        if (dmn->proTxHash == myProTxHash) {
            continue;
        }
        auto lastOutbound = mn_metaman.GetMetaInfo(dmn->proTxHash)->GetLastOutboundSuccess();
        if (curTime - lastOutbound < 10 * 60) {
            // avoid re-probing nodes too often
            continue;
        }
        probeConnections.emplace(dmn->proTxHash);
    }

    if (!probeConnections.empty()) {
        if (LogAcceptDebug(BCLog::LLMQ)) {
            std::string debugMsg = strprintf("%s -- adding masternodes probes for quorum %s:\n", __func__, pQuorumBaseBlockIndex->GetBlockHash().ToString());
            for (const auto& c : probeConnections) {
                auto dmn = tip_mn_list.GetValidMN(c);
                if (!dmn) {
                    debugMsg += strprintf("  %s (not in valid MN set anymore)\n", c.ToString());
                } else {
                    debugMsg += strprintf("  %s (%s)\n", c.ToString(), dmn->pdmnState->addr.ToStringAddrPort());
                }
            }
            LogPrint(BCLog::NET_NETCONN, debugMsg.c_str()); /* Continued */
        }
        connman.AddPendingProbeConnections(probeConnections);
    }
}

template <typename CacheType>
void InitQuorumsCache(CacheType& cache, bool limit_by_connections)
{
    for (const auto& llmq : Params().GetConsensus().llmqs) {
        cache.emplace(std::piecewise_construct, std::forward_as_tuple(llmq.type),
                      std::forward_as_tuple(limit_by_connections ? llmq.keepOldConnections : llmq.keepOldKeys));
    }
}
template void InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>>& cache, bool limit_by_connections);
template void InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::vector<CQuorumCPtr>, StaticSaltedHasher>>& cache, bool limit_by_connections);
template void InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>, std::less<Consensus::LLMQType>, std::allocator<std::pair<Consensus::LLMQType const, unordered_lru_cache<uint256, std::shared_ptr<llmq::CQuorum>, StaticSaltedHasher, 0ul, 0ul>>>>&cache, bool limit_by_connections);
template void InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, int, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, int, StaticSaltedHasher>>& cache, bool limit_by_connections);
template void InitQuorumsCache<std::map<Consensus::LLMQType, unordered_lru_cache<uint256, uint256, StaticSaltedHasher>>>(std::map<Consensus::LLMQType, unordered_lru_cache<uint256, uint256, StaticSaltedHasher>>& cache, bool limit_by_connections);
} // namespace utils
} // namespace llmq
