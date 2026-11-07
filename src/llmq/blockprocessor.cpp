// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/options.h>
#include <llmq/utils.h>

#include <evo/evodb.h>
#include <evo/specialtx.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <net.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <saltedhasher.h>
#include <sync.h>
#include <util/irange.h>
#include <util/underlying.h>
#include <validation.h>

#include <map>

namespace llmq
{
static const std::string DB_MINED_COMMITMENT = "q_mc";
static const std::string DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT = "q_mcih";

static const std::string DB_BEST_BLOCK_UPGRADE = "q_bbu2";

CQuorumBlockProcessor::CQuorumBlockProcessor(CChainState& chainstate, CDeterministicMNManager& dmnman, CEvoDB& evoDb) :
    m_chainstate(chainstate),
    m_dmnman(dmnman),
    m_evoDb(evoDb)
{
    utils::InitQuorumsCache(mapHasMinedCommitmentCache);
}

MessageProcessingResult CQuorumBlockProcessor::ProcessMessage(const CNode& peer, std::string_view msg_type,
                                                              CDataStream& vRecv,
                                                              const ConsumeRequestFn& consume_request)
{
    if (msg_type != NetMsgType::QFCOMMITMENT) {
        return {};
    }

    CFinalCommitment qc;
    vRecv >> qc;

    // A QFCOMMITMENT is only ever sent in reply to a GETDATA (see ProcessGetData), so one we never
    // asked this peer for was pushed at us. Drop it up front: most of the checks below reject
    // without scoring the peer -- deliberately, since we may just be lagging behind -- so an
    // unsolicited peer could otherwise repeat the block lookups and map probes indefinitely. A bare
    // announcement deliberately does not qualify: it would let the peer authorise its own payload by
    // sending INV first. (dash#7484, adapted)
    if (!consume_request(CInv{MSG_QUORUM_FINAL_COMMITMENT, ::SerializeHash(qc)})) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- unrequested commitment from peer=%d\n", __func__,
                 peer.GetId());
        return MisbehavingError{UNREQUESTED_OBJECT_MISBEHAVIOR_SCORE, "unrequested quorum commitment"};
    }

    // Note: no m_to_erase, the request was already consumed by the solicitation check above.
    MessageProcessingResult ret;

    if (qc.IsNull()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- null commitment from peer=%d\n", __func__, peer.GetId());
        ret.m_error = MisbehavingError{100};
        return ret;
    }

    const auto& llmq_params_opt = Params().GetLLMQ(qc.llmqType);
    if (!llmq_params_opt.has_value()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- invalid commitment type %d from peer=%d\n", __func__,
                 ToUnderlying(qc.llmqType), peer.GetId());
        ret.m_error = MisbehavingError{100};
        return ret;
    }
    auto type = qc.llmqType;

    // Verify that quorumHash is part of the active chain and that it's the first block in the DKG interval
    const CBlockIndex* pQuorumBaseBlockIndex;
    {
        LOCK(cs_main);
        pQuorumBaseBlockIndex = m_chainstate.m_blockman.LookupBlockIndex(qc.quorumHash);
        if (pQuorumBaseBlockIndex == nullptr) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- unknown block %s in commitment, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            // can't really punish the node here, as we might simply be the one that is on the wrong chain or not
            // fully synced
            return ret;
        }
        if (m_chainstate.m_chain.Tip()->GetAncestor(pQuorumBaseBlockIndex->nHeight) != pQuorumBaseBlockIndex) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- block %s not in active chain, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            // same, can't punish
            return ret;
        }
        if (int quorumHeight = pQuorumBaseBlockIndex->nHeight - (pQuorumBaseBlockIndex->nHeight % llmq_params_opt->dkgInterval) + int(qc.quorumIndex);
                quorumHeight != pQuorumBaseBlockIndex->nHeight) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- block %s is not the first block in the DKG interval, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            ret.m_error = MisbehavingError{100};
            return ret;
        }
        if (pQuorumBaseBlockIndex->nHeight < (m_chainstate.m_chain.Height() - llmq_params_opt->dkgInterval)) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- block %s is too old, peer=%d\n", __func__,
                     qc.quorumHash.ToString(), peer.GetId());
            // TODO: enable punishment in some future version when all/most nodes are running with this fix
            // ret.m_error = MisbehavingError{100};
            return ret;
        }
        if (HasMinedCommitment(type, qc.quorumHash)) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- commitment for quorum hash[%s], type[%d], quorumIndex[%d] is already mined, peer=%d\n",
                     __func__, qc.quorumHash.ToString(), ToUnderlying(type), qc.quorumIndex, peer.GetId());
            // NOTE: do not punish here
            return ret;
        }
    }

    {
        // Check if we already got a better one locally
        // We do this before verifying the commitment to avoid DoS
        LOCK(minableCommitmentsCs);
        auto k = std::make_pair(type, qc.quorumHash);
        auto it = minableCommitmentsByQuorum.find(k);
        if (it != minableCommitmentsByQuorum.end()) {
            auto jt = minableCommitments.find(it->second);
            if (jt != minableCommitments.end() && jt->second.CountSigners() >= qc.CountSigners()) {
                return ret;
            }
        }
    }

    if (!qc.Verify(m_dmnman, pQuorumBaseBlockIndex, /*checkSigs=*/true)) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- commitment for quorum %s:%d is not valid quorumIndex[%d] nversion[%d], peer=%d\n",
                 __func__, qc.quorumHash.ToString(),
                 ToUnderlying(qc.llmqType), qc.quorumIndex, qc.nVersion, peer.GetId());
        ret.m_error = MisbehavingError{100};
        return ret;
    }

    LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- received commitment for quorum %s:%d, validMembers=%d, signers=%d, peer=%d\n", __func__,
             qc.quorumHash.ToString(), ToUnderlying(qc.llmqType), qc.CountValidMembers(), qc.CountSigners(), peer.GetId());

    ret.m_inventory = AddMineableCommitment(qc);
    return ret;
}

bool CQuorumBlockProcessor::ProcessBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindex, BlockValidationState& state, bool fJustCheck, bool fBLSChecks)
{
    AssertLockHeld(cs_main);

    const auto blockHash = pindex->GetBlockHash();

    if (!DeploymentActiveAt(*pindex, Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
        m_evoDb.Write(DB_BEST_BLOCK_UPGRADE, blockHash);
        return true;
    }

    std::multimap<Consensus::LLMQType, CFinalCommitment> qcs;
    if (!GetCommitmentsFromBlock(block, pindex, qcs, state)) {
        return false;
    }

    // The following checks make sure that there is always a (possibly null) commitment while in the mining phase
    // until the first non-null commitment has been mined. After the non-null commitment, no other commitments are
    // allowed, including null commitments.
    // Note: must only check quorums that were enabled at the _previous_ block height to match mining logic
    for (const Consensus::LLMQParams& params : GetEnabledQuorumParams(pindex->pprev)) {
        // skip these checks when replaying blocks after the crash
        if (m_chainstate.m_chain.Tip() == nullptr) {
            break;
        }

        const size_t numCommitmentsRequired = GetNumCommitmentsRequired(params, pindex->nHeight);
        const auto numCommitmentsInNewBlock = qcs.count(params.type);

        if (numCommitmentsRequired < numCommitmentsInNewBlock) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-not-allowed");
        }

        if (numCommitmentsRequired > numCommitmentsInNewBlock) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-missing");
        }
    }

    for (const auto& [_, qc] : qcs) {
        if (!ProcessCommitment(pindex->nHeight, blockHash, qc, state, fJustCheck, fBLSChecks)) {
            LogPrintf("[ProcessBlock] failed h[%d] llmqType[%d] version[%d] quorumIndex[%d] quorumHash[%s]\n", pindex->nHeight, ToUnderlying(qc.llmqType), qc.nVersion, qc.quorumIndex, qc.quorumHash.ToString());
            return false;
        }
    }

    m_evoDb.Write(DB_BEST_BLOCK_UPGRADE, blockHash);

    return true;
}

// We store a mapping from minedHeight->quorumHeight in the DB
// minedHeight is inversed so that entries are traversable in reversed order
static std::tuple<std::string, Consensus::LLMQType, uint32_t> BuildInversedHeightKey(Consensus::LLMQType llmqType, int nMinedHeight)
{
    // nMinedHeight must be converted to big endian to make it comparable when serialized
    return std::make_tuple(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT, llmqType, htobe32(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

bool CQuorumBlockProcessor::ProcessCommitment(int nHeight, const uint256& blockHash, const CFinalCommitment& qc, BlockValidationState& state, bool fJustCheck, bool fBLSChecks)
{
    AssertLockHeld(cs_main);

    const auto& llmq_params_opt = Params().GetLLMQ(qc.llmqType);
    if (!llmq_params_opt.has_value()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- invalid commitment type %d\n", __func__, ToUnderlying(qc.llmqType));
        return false;
    }
    const auto& llmq_params = llmq_params_opt.value();

    uint256 quorumHash = GetQuorumBlockHash(llmq_params, m_chainstate.m_chain, nHeight, qc.quorumIndex);

    LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s fJustCheck[%d] processing commitment from block.\n", __func__,
             nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString(), fJustCheck);

    // skip `bad-qc-block` checks below when replaying blocks after the crash
    if (m_chainstate.m_chain.Tip() == nullptr) {
        quorumHash = qc.quorumHash;
    }

    if (quorumHash.IsNull()) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s quorumHash is null.\n", __func__,
                 nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-block");
    }
    if (quorumHash != qc.quorumHash) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, qc.quorumHash=%s signers=%s, validMembers=%d, quorumPublicKey=%s non equal quorumHash.\n", __func__,
                 nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-block");
    }

    if (qc.IsNull()) {
        if (!qc.VerifyNull()) {
            LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%dqc verifynull failed.\n", __func__,
                     nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers());
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-invalid-null");
        }
        return true;
    }

    if (HasMinedCommitment(llmq_params.type, quorumHash)) {
        // should not happen as it's already handled in ProcessBlock
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-dup");
    }

    if (!IsMiningPhase(llmq_params, m_chainstate.m_chain, nHeight)) {
        // should not happen as it's already handled in ProcessBlock
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-height");
    }

    const auto* pQuorumBaseBlockIndex = m_chainstate.m_blockman.LookupBlockIndex(qc.quorumHash);

    if (!qc.Verify(m_dmnman, pQuorumBaseBlockIndex, /*checkSigs=*/fBLSChecks)) {
        LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d, type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s qc verify failed.\n", __func__,
                 nHeight, ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-invalid");
    }

    if (fJustCheck) {
        return true;
    }

    // Store commitment in DB
    auto cacheKey = std::make_pair(llmq_params.type, quorumHash);
    m_evoDb.Write(std::make_pair(DB_MINED_COMMITMENT, cacheKey), std::make_pair(qc, blockHash));

    m_evoDb.Write(BuildInversedHeightKey(llmq_params.type, nHeight), pQuorumBaseBlockIndex->nHeight);

    // Only once this commitment is fully recorded, so the cache can never be
    // repopulated from a half-updated view.
    DropQcHashesCache();

    {
        LOCK(minableCommitmentsCs);
        mapHasMinedCommitmentCache[qc.llmqType].erase(qc.quorumHash);
        minableCommitmentsByQuorum.erase(cacheKey);
        minableCommitments.erase(::SerializeHash(qc));
    }

    LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s -- processed commitment from block. type=%d, quorumIndex=%d, quorumHash=%s, signers=%s, validMembers=%d, quorumPublicKey=%s\n", __func__,
             ToUnderlying(qc.llmqType), qc.quorumIndex, quorumHash.ToString(), qc.CountSigners(), qc.CountValidMembers(), qc.quorumPublicKey.ToString());

    return true;
}

void CQuorumBlockProcessor::DropQcHashesCache()
{
    LOCK(m_qc_hashes_cache_mutex);
    m_quorums_cached.clear();
    m_qc_hashes_cached.clear();
}

std::optional<QcHashMap> CQuorumBlockProcessor::GetQcHashes(const CBlockIndex* pindexPrev) const
{
    auto quorums = GetMinedAndActiveCommitmentsUntilBlock(pindexPrev);

    LOCK(m_qc_hashes_cache_mutex);
    if (quorums == m_quorums_cached) {
        return m_qc_hashes_cached;
    }

    // Quorums set is different, reset cached values
    m_quorums_cached.clear();
    m_qc_hashes_cached.clear();

    for (const auto& [llmqType, vecBlockIndexes] : quorums) {
        auto& vec_hashes = m_qc_hashes_cached[llmqType];
        vec_hashes.reserve(vecBlockIndexes.size());
        for (const auto& blockIndex : vecBlockIndexes) {
            uint256 dummyHash;
            CFinalCommitmentPtr pqc = GetMinedCommitment(llmqType, blockIndex->GetBlockHash(), dummyHash);
            if (pqc == nullptr) {
                // this should never happen
                return std::nullopt;
            }
            vec_hashes.emplace_back(::SerializeHash(*pqc));
        }
    }
    m_quorums_cached = std::move(quorums);
    return m_qc_hashes_cached;
}

bool CQuorumBlockProcessor::UndoBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindex)
{
    AssertLockHeld(cs_main);

    std::multimap<Consensus::LLMQType, CFinalCommitment> qcs;
    if (BlockValidationState dummy; !GetCommitmentsFromBlock(block, pindex, qcs, dummy)) {
        return false;
    }

    for (auto& [_, qc2] : qcs) {
        auto& qc = qc2; // cannot capture structured binding into lambda
        if (qc.IsNull()) {
            continue;
        }

        m_evoDb.Erase(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(qc.llmqType, qc.quorumHash)));

        const auto& llmq_params_opt = Params().GetLLMQ(qc.llmqType);
        assert(llmq_params_opt.has_value());

        m_evoDb.Erase(BuildInversedHeightKey(qc.llmqType, pindex->nHeight));

        // Only once this commitment is fully unrecorded; see ProcessCommitment.
        DropQcHashesCache();

        WITH_LOCK(minableCommitmentsCs, mapHasMinedCommitmentCache[qc.llmqType].erase(qc.quorumHash));

        // if a reorg happened, we should allow to mine this commitment later
        AddMineableCommitment(qc);
    }

    m_evoDb.Write(DB_BEST_BLOCK_UPGRADE, pindex->pprev->GetBlockHash());

    return true;
}

bool CQuorumBlockProcessor::GetCommitmentsFromBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindex, std::multimap<Consensus::LLMQType, CFinalCommitment>& ret, BlockValidationState& state)
{
    AssertLockHeld(cs_main);

    const auto& consensus = Params().GetConsensus();

    ret.clear();

    for (const auto& tx : block.vtx) {
        if (tx->nType == TRANSACTION_QUORUM_COMMITMENT) {
            const auto opt_qc = GetTxPayload<CFinalCommitmentTxPayload>(*tx);
            if (!opt_qc) {
                // should not happen as it was verified before processing the block
                LogPrint(BCLog::LLMQ, "CQuorumBlockProcessor::%s height=%d GetTxPayload fails\n", __func__, pindex->nHeight);
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-payload");
            }
            auto& qc = *opt_qc;

            const auto& llmq_params_opt = Params().GetLLMQ(qc.commitment.llmqType);
            if (!llmq_params_opt.has_value()) {
                // should not happen as it was verified before processing the block
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-commitment-type");
            }

            // only allow one commitment per type and per block
            if (ret.count(qc.commitment.llmqType) != 0) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-dup");
            }

            ret.emplace(qc.commitment.llmqType, std::move(qc.commitment));
        }
    }

    if (pindex->nHeight < consensus.DIP0003Height && !ret.empty()) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-premature");
    }

    return true;
}

bool CQuorumBlockProcessor::IsMiningPhase(const Consensus::LLMQParams& llmqParams, const CChain& active_chain, int nHeight)
{
    AssertLockHeld(cs_main);

    // Note: This function can be called for new blocks
    assert(nHeight <= active_chain.Height() + 1);

    int quorumCycleStartHeight = nHeight - (nHeight % llmqParams.dkgInterval);
    int quorumCycleMiningStartHeight = quorumCycleStartHeight + llmqParams.dkgMiningWindowStart;
    int quorumCycleMiningEndHeight = quorumCycleStartHeight + llmqParams.dkgMiningWindowEnd;

    if (nHeight >= quorumCycleMiningStartHeight && nHeight <= quorumCycleMiningEndHeight) {
        LogPrint(BCLog::LLMQ, "[IsMiningPhase] nHeight[%d] llmqType[%d] quorumCycleStartHeight[%d] -- mining[%d-%d]\n", nHeight, ToUnderlying(llmqParams.type), quorumCycleStartHeight, quorumCycleMiningStartHeight, quorumCycleMiningEndHeight);
        return true;
    }
    LogPrint(BCLog::LLMQ, "[IsMiningPhase] nHeight[%d] llmqType[%d] quorumCycleStartHeight[%d] -- NOT mining[%d-%d]\n", nHeight, ToUnderlying(llmqParams.type), quorumCycleStartHeight, quorumCycleMiningStartHeight, quorumCycleMiningEndHeight);

    return false;
}

size_t CQuorumBlockProcessor::GetNumCommitmentsRequired(const Consensus::LLMQParams& llmqParams, int nHeight) const
{
    AssertLockHeld(cs_main);

    if (!IsMiningPhase(llmqParams, m_chainstate.m_chain, nHeight)) return 0;

    // Note: This function can be called for new blocks
    assert(nHeight <= m_chainstate.m_chain.Height() + 1);

    // One quorum per type and cycle, based on the cycle's first block (quorumIndex 0).
    const uint256 quorumHash = GetQuorumBlockHash(llmqParams, m_chainstate.m_chain, nHeight, /*quorumIndex=*/0);
    return (!quorumHash.IsNull() && !HasMinedCommitment(llmqParams.type, quorumHash)) ? 1 : 0;
}

// WARNING: This method returns uint256() on the first block of the DKG interval (because the block hash is not known yet)
uint256 CQuorumBlockProcessor::GetQuorumBlockHash(const Consensus::LLMQParams& llmqParams, const CChain& active_chain, int nHeight, int quorumIndex)
{
    AssertLockHeld(cs_main);

    int quorumStartHeight = nHeight - (nHeight % llmqParams.dkgInterval) + quorumIndex;

    uint256 quorumBlockHash;
    if (!GetBlockHash(active_chain, quorumBlockHash, quorumStartHeight)) {
        LogPrint(BCLog::LLMQ, "[GetQuorumBlockHash] llmqType[%d] h[%d] qi[%d] quorumStartHeight[%d] quorumHash[EMPTY]\n", ToUnderlying(llmqParams.type), nHeight, quorumIndex, quorumStartHeight);
        return {};
    }

    LogPrint(BCLog::LLMQ, "[GetQuorumBlockHash] llmqType[%d] h[%d] qi[%d] quorumStartHeight[%d] quorumHash[%s]\n", ToUnderlying(llmqParams.type), nHeight, quorumIndex, quorumStartHeight, quorumBlockHash.ToString());
    return quorumBlockHash;
}

bool CQuorumBlockProcessor::HasMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash) const
{
    bool fExists;
    {
        // Defence-in-depth: this map is only pre-seeded by InitQuorumsCache() with the LLMQ types
        // from the chain's consensus params. operator[] with any other type would insert a
        // default-constructed, zero-capacity cache and abort in its constructor, so treat an
        // unregistered type as "no mined commitment" rather than indexing the map.
        LOCK(minableCommitmentsCs);
        auto it = mapHasMinedCommitmentCache.find(llmqType);
        if (it == mapHasMinedCommitmentCache.end()) {
            return false;
        }
        if (it->second.get(quorumHash, fExists)) {
            return fExists;
        }
    }

    fExists = m_evoDb.Exists(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(llmqType, quorumHash)));

    {
        LOCK(minableCommitmentsCs);
        // The key set is fixed at construction, so this can only miss if the type was unregistered,
        // which the check above already returned on.
        if (auto it = mapHasMinedCommitmentCache.find(llmqType); it != mapHasMinedCommitmentCache.end()) {
            it->second.insert(quorumHash, fExists);
        }
    }

    return fExists;
}

CFinalCommitmentPtr CQuorumBlockProcessor::GetMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash, uint256& retMinedBlockHash) const
{
    auto key = std::make_pair(DB_MINED_COMMITMENT, std::make_pair(llmqType, quorumHash));
    std::pair<CFinalCommitment, uint256> p;
    if (!m_evoDb.Read(key, p)) {
        return nullptr;
    }
    retMinedBlockHash = p.second;
    return std::make_unique<CFinalCommitment>(p.first);
}

// The returned quorums are in reversed order, so the most recent one is at index 0
std::vector<const CBlockIndex*> CQuorumBlockProcessor::GetMinedCommitmentsUntilBlock(Consensus::LLMQType llmqType, gsl::not_null<const CBlockIndex*> pindex, size_t maxCount) const
{
    AssertLockNotHeld(m_evoDb.cs);
    LOCK(m_evoDb.cs);

    auto dbIt = m_evoDb.GetCurTransaction().NewIteratorUniquePtr();

    auto firstKey = BuildInversedHeightKey(llmqType, pindex->nHeight);
    auto lastKey = BuildInversedHeightKey(llmqType, 0);

    dbIt->Seek(firstKey);

    std::vector<const CBlockIndex*> ret;
    ret.reserve(maxCount);

    while (dbIt->Valid() && ret.size() < maxCount) {
        decltype(firstKey) curKey;
        int quorumHeight;
        if (!dbIt->GetKey(curKey) || curKey >= lastKey) {
            break;
        }
        if (std::get<0>(curKey) != DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT || std::get<1>(curKey) != llmqType) {
            break;
        }

        if (uint32_t nMinedHeight = std::numeric_limits<uint32_t>::max() - be32toh(std::get<2>(curKey));
                nMinedHeight > static_cast<uint32_t>(pindex->nHeight)) {
            break;
        }

        if (!dbIt->GetValue(quorumHeight)) {
            break;
        }

        const auto* pQuorumBaseBlockIndex = pindex->GetAncestor(quorumHeight);
        assert(pQuorumBaseBlockIndex);
        ret.emplace_back(pQuorumBaseBlockIndex);

        dbIt->Next();
    }

    return ret;
}

// The returned quorums are in reversed order, so the most recent one is at index 0
std::map<Consensus::LLMQType, std::vector<const CBlockIndex*>> CQuorumBlockProcessor::GetMinedAndActiveCommitmentsUntilBlock(gsl::not_null<const CBlockIndex*> pindex) const
{
    std::map<Consensus::LLMQType, std::vector<const CBlockIndex*>> ret;

    for (const auto& params : Params().GetConsensus().llmqs) {
        auto& v = ret[params.type];
        v.reserve(params.signingActiveQuorumCount);
        auto commitments = GetMinedCommitmentsUntilBlock(params.type, pindex, params.signingActiveQuorumCount);
        std::copy(commitments.begin(), commitments.end(), std::back_inserter(v));
    }

    return ret;
}

bool CQuorumBlockProcessor::HasMineableCommitment(const uint256& hash) const
{
    LOCK(minableCommitmentsCs);
    return minableCommitments.count(hash) != 0;
}

std::optional<CInv> CQuorumBlockProcessor::AddMineableCommitment(const CFinalCommitment& fqc)
{
    const uint256 commitmentHash = ::SerializeHash(fqc);

    const bool relay = [&]() {
        LOCK(minableCommitmentsCs);

        auto k = std::make_pair(fqc.llmqType, fqc.quorumHash);
        auto [itInserted, successfullyInserted] = minableCommitmentsByQuorum.try_emplace(k, commitmentHash);
        if (successfullyInserted) {
            minableCommitments.try_emplace(commitmentHash, fqc);
            return true;
        } else {
            auto& insertedQuorumHash = itInserted->second;
            const int oldSignerCount = minableCommitments.at(insertedQuorumHash).CountSigners();
            if (fqc.CountSigners() > oldSignerCount) {
                // New commitment has more signers, so override the known one.
                // Erase by the OLD hash first: repointing before erasing removed
                // a key that had not been inserted yet, so the superseded
                // commitment stayed in the map and was still served by
                // GetMineableCommitmentByHash. The signer count is taken by
                // value rather than through a reference into the map, so the
                // erase cannot leave a dangling read behind it.
                minableCommitments.erase(insertedQuorumHash);
                insertedQuorumHash = commitmentHash;
                minableCommitments.try_emplace(commitmentHash, fqc);
                return true;
            }
        }
        return false;
    }();

    return relay ? std::make_optional(CInv{MSG_QUORUM_FINAL_COMMITMENT, commitmentHash}) : std::nullopt;
}

bool CQuorumBlockProcessor::GetMineableCommitmentByHash(const uint256& commitmentHash, llmq::CFinalCommitment& ret) const
{
    LOCK(minableCommitmentsCs);
    auto it = minableCommitments.find(commitmentHash);
    if (it == minableCommitments.end()) {
        return false;
    }
    ret = it->second;
    return true;
}

// Will return nullopt if no commitment should be mined
// Will return a null commitment if no mineable commitment is known and none was mined yet
std::optional<std::vector<CFinalCommitment>> CQuorumBlockProcessor::GetMineableCommitments(const Consensus::LLMQParams& llmqParams, int nHeight) const
{
    AssertLockHeld(cs_main);

    std::vector<CFinalCommitment> ret;

    if (GetNumCommitmentsRequired(llmqParams, nHeight) == 0) {
        // no commitment required
        return std::nullopt;
    }

    // Note: This function can be called for new blocks
    assert(nHeight <= m_chainstate.m_chain.Height() + 1);
    const auto *const pindex = m_chainstate.m_chain.Height() < nHeight ? m_chainstate.m_chain.Tip() : m_chainstate.m_chain.Tip()->GetAncestor(nHeight);

    const bool basic_bls_enabled{DeploymentActiveAfter(pindex, Params().GetConsensus(), Consensus::DEPLOYMENT_V19)};

    // One quorum per type and cycle, based on the cycle's first block (quorumIndex 0).
    const uint256 quorumHash = GetQuorumBlockHash(llmqParams, m_chainstate.m_chain, nHeight, /*quorumIndex=*/0);
    if (quorumHash.IsNull() || HasMinedCommitment(llmqParams.type, quorumHash)) {
        return std::nullopt;
    }

    CFinalCommitment cf;
    {
        LOCK(minableCommitmentsCs);

        auto k = std::make_pair(llmqParams.type, quorumHash);
        if (auto it = minableCommitmentsByQuorum.find(k); it == minableCommitmentsByQuorum.end()) {
            // null commitment required
            cf = CFinalCommitment(llmqParams, quorumHash);
            cf.nVersion = CFinalCommitment::GetVersion(/*is_rotation_enabled=*/false, basic_bls_enabled);
            LogPrint(BCLog::LLMQ, "GetMineableCommitments cf height[%d] content: { created nversion[%d] quorumIndex[%d] }\n", nHeight, cf.nVersion, cf.quorumIndex);
        } else {
            cf = minableCommitments.at(it->second);
            LogPrint(BCLog::LLMQ, "GetMineableCommitments cf height[%d] content: { cached nversion[%d] quorumIndex[%d] }\n", nHeight, cf.nVersion, cf.quorumIndex);
        }
    }

    return std::vector<CFinalCommitment>{std::move(cf)};
}

bool CQuorumBlockProcessor::GetMineableCommitmentsTx(const Consensus::LLMQParams& llmqParams, int nHeight, std::vector<CTransactionRef>& ret) const
{
    AssertLockHeld(cs_main);
    std::optional<std::vector<CFinalCommitment>> qcs = GetMineableCommitments(llmqParams, nHeight);
    if (!qcs.has_value()) {
        return false;
    }

    for (const auto& f : qcs.value()) {
        CFinalCommitmentTxPayload qc;
        qc.nHeight = nHeight;
        qc.commitment = f;
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_QUORUM_COMMITMENT;
        SetTxPayload(tx, qc);
        ret.push_back(MakeTransactionRef(tx));
    }

    return true;
}

} // namespace llmq
