// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <evo/dmnstate.h>
#include <evo/evodb.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <llmq/commitment.h>
#include <llmq/utils.h>

#include <base58.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <script/standard.h>
#include <validation.h>
#include <validationinterface.h>
#include <univalue.h>
#include <messagesigner.h>
#include <uint256.h>

#include <functional>
#include <optional>
#include <memory>

static const std::string DB_LIST_SNAPSHOT = "dmn_S4";
static const std::string DB_LIST_DIFF = "dmn_D4";
static const std::string DB_LIST_REPAIRED = "dmn_R1";

uint64_t CDeterministicMN::GetInternalId() const
{
    // can't get it if it wasn't set yet
    assert(internalId != std::numeric_limits<uint64_t>::max());
    return internalId;
}

std::string CDeterministicMN::ToString() const
{
    return strprintf("CDeterministicMN(proTxHash=%s, collateralOutpoint=%s, nOperatorReward=%f, state=%s", proTxHash.ToString(), collateralOutpoint.ToStringShort(), (double)nOperatorReward / 100, pdmnState->ToString());
}

UniValue CDeterministicMN::ToJson() const
{
    UniValue obj;
    obj.setObject();

    obj.pushKV("type", std::string(GetMnType(nType).description));
    obj.pushKV("proTxHash", proTxHash.ToString());
    obj.pushKV("collateralHash", collateralOutpoint.hash.ToString());
    obj.pushKV("collateralIndex", (int)collateralOutpoint.n);

    uint256 tmpHashBlock;
    CTransactionRef collateralTx = GetTransaction(/* block_index */ nullptr,  /* mempool */ nullptr, collateralOutpoint.hash, Params().GetConsensus(), tmpHashBlock);
    if (collateralTx) {
        CTxDestination dest;
        if (ExtractDestination(collateralTx->vout[collateralOutpoint.n].scriptPubKey, dest)) {
            obj.pushKV("collateralAddress", EncodeDestination(dest));
        }
    }

    obj.pushKV("operatorReward", (double)nOperatorReward / 100);
    obj.pushKV("state", pdmnState->ToJson(nType));
    return obj;
}

bool CDeterministicMNList::IsMNValid(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return false;
    }
    return IsMNValid(**p);
}

bool CDeterministicMNList::IsMNPoSeBanned(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return false;
    }
    return IsMNPoSeBanned(**p);
}

bool CDeterministicMNList::IsMNValid(const CDeterministicMN& dmn)
{
    return !IsMNPoSeBanned(dmn);
}

bool CDeterministicMNList::IsMNPoSeBanned(const CDeterministicMN& dmn)
{
    return dmn.pdmnState->IsBanned();
}

CDeterministicMNCPtr CDeterministicMNList::GetMN(const uint256& proTxHash) const
{
    auto p = mnMap.find(proTxHash);
    if (p == nullptr) {
        return nullptr;
    }
    return *p;
}

CDeterministicMNCPtr CDeterministicMNList::GetValidMN(const uint256& proTxHash) const
{
    auto dmn = GetMN(proTxHash);
    if (dmn && !IsMNValid(*dmn)) {
        return nullptr;
    }
    return dmn;
}

CDeterministicMNCPtr CDeterministicMNList::GetMNByOperatorKey(const CBLSPublicKey& pubKey) const
{
    const auto it = ranges::find_if(mnMap,
                              [&pubKey](const auto& p){return p.second->pdmnState->pubKeyOperator.Get() == pubKey;});
    if (it == mnMap.end()) {
        return nullptr;
    }
    return it->second;
}

CDeterministicMNCPtr CDeterministicMNList::GetMNByCollateral(const COutPoint& collateralOutpoint) const
{
    return GetUniquePropertyMN(collateralOutpoint);
}

CDeterministicMNCPtr CDeterministicMNList::GetValidMNByCollateral(const COutPoint& collateralOutpoint) const
{
    auto dmn = GetMNByCollateral(collateralOutpoint);
    if (dmn && !IsMNValid(*dmn)) {
        return nullptr;
    }
    return dmn;
}

CDeterministicMNCPtr CDeterministicMNList::GetMNByService(const CService& service) const
{
    return GetUniquePropertyMN(service);
}

CDeterministicMNCPtr CDeterministicMNList::GetMNByInternalId(uint64_t internalId) const
{
    auto proTxHash = mnInternalIdMap.find(internalId);
    if (!proTxHash) {
        return nullptr;
    }
    return GetMN(*proTxHash);
}

static int CompareByLastPaid_GetHeight(const CDeterministicMN& dmn)
{
    int height = dmn.pdmnState->nLastPaidHeight;
    if (dmn.pdmnState->nPoSeRevivedHeight != -1 && dmn.pdmnState->nPoSeRevivedHeight > height) {
        height = dmn.pdmnState->nPoSeRevivedHeight;
    } else if (height == 0) {
        height = dmn.pdmnState->nRegisteredHeight;
    }
    return height;
}

static bool CompareByLastPaid(const CDeterministicMN& _a, const CDeterministicMN& _b)
{
    int ah = CompareByLastPaid_GetHeight(_a);
    int bh = CompareByLastPaid_GetHeight(_b);
    if (ah == bh) {
        return _a.proTxHash < _b.proTxHash;
    } else {
        return ah < bh;
    }
}
static bool CompareByLastPaid(const CDeterministicMN* _a, const CDeterministicMN* _b)
{
    return CompareByLastPaid(*_a, *_b);
}

CDeterministicMNCPtr CDeterministicMNList::GetMNPayee(gsl::not_null<const CBlockIndex*> pindexPrev) const
{
    if (mnMap.size() == 0) {
        return nullptr;
    }

    const bool isMNRewardReallocation{DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_MN_RR)};
    // Until MNRewardReallocation, a type whose payment weight is above one is
    // paid that many blocks in a row; nConsecutivePayments tracks where in its
    // run the current payee stands. No such type is registrable today.
    CDeterministicMNCPtr best = nullptr;
    if (!isMNRewardReallocation) {
        ForEachMNShared(true, [&](const CDeterministicMNCPtr& dmn) {
            if (dmn->pdmnState->nLastPaidHeight == nHeight) {
                // We found the last MN payee. If its type grants more than one
                // payout slot, it is paid again until its run is exhausted.
                const auto payment_weight = GetEffectivePaymentWeight(*dmn);
                if (payment_weight > 1 && dmn->pdmnState->nConsecutivePayments < payment_weight) {
                    best = dmn;
                }
            }
        });

        if (best != nullptr) return best;

        // Note: If the last payee holds a single payout slot, or was removed
        // from the mnList meanwhile, classic payee selection proceeds.
    }

    ForEachMNShared(true, [&](const CDeterministicMNCPtr& dmn) {
        if (best == nullptr || CompareByLastPaid(dmn.get(), best.get())) {
            best = dmn;
        }
    });

    return best;
}

std::vector<CDeterministicMNCPtr> CDeterministicMNList::GetProjectedMNPayees(gsl::not_null<const CBlockIndex* const> pindexPrev, int nCount) const
{
    if (nCount < 0 ) {
        return {};
    }
    const bool isMNRewardReallocation = DeploymentActiveAfter(pindexPrev, Params().GetConsensus(),
                                                              Consensus::DEPLOYMENT_MN_RR);
    const auto weighted_count = isMNRewardReallocation ? GetValidMNsCount() : GetValidPaymentWeightedMNsCount();
    nCount = std::min(nCount, int(weighted_count));

    std::vector<CDeterministicMNCPtr> result;
    result.reserve(weighted_count);

    int remaining_weighted_payments{0};
    CDeterministicMNCPtr mn_to_be_skipped{nullptr};
    if (!isMNRewardReallocation) {
        ForEachMNShared(true, [&](const CDeterministicMNCPtr& dmn) {
            if (dmn->pdmnState->nLastPaidHeight == nHeight) {
                // We found the last MN payee. If its type grants more than one
                // payout slot and its run is not exhausted, the remaining slots
                // come first in the projection.
                const auto payment_weight = GetEffectivePaymentWeight(*dmn);
                if (payment_weight > 1 && dmn->pdmnState->nConsecutivePayments < payment_weight) {
                    remaining_weighted_payments = payment_weight - dmn->pdmnState->nConsecutivePayments;
                    for ([[maybe_unused]] auto _ : irange::range(remaining_weighted_payments)) {
                        result.emplace_back(dmn);
                        mn_to_be_skipped = dmn;
                    }
                }
            }
        });
    }

    ForEachMNShared(true, [&](const CDeterministicMNCPtr& dmn) {
        if (dmn == mn_to_be_skipped) return;
        for ([[maybe_unused]] auto _ : irange::range(isMNRewardReallocation ? 1 : GetEffectivePaymentWeight(*dmn))) {
            result.emplace_back(dmn);
        }
    });

    if (mn_to_be_skipped != nullptr) {
        // a payee mid-run keeps its already-paid slots at the end of the list
        for ([[maybe_unused]] auto _ : irange::range(mn_to_be_skipped->pdmnState->nConsecutivePayments)) {
            result.emplace_back(mn_to_be_skipped);
        }
    }

    std::sort(result.begin() + remaining_weighted_payments, result.end(), [&](const CDeterministicMNCPtr& a, const CDeterministicMNCPtr& b) {
        return CompareByLastPaid(a.get(), b.get());
    });

    result.resize(nCount);

    return result;
}

std::vector<CDeterministicMNCPtr> CDeterministicMNList::CalculateQuorum(size_t maxSize, const uint256& modifier) const
{
    auto scores = CalculateScores(modifier);

    // sort is descending order
    std::sort(scores.rbegin(), scores.rend(), [](const std::pair<arith_uint256, CDeterministicMNCPtr>& a, const std::pair<arith_uint256, CDeterministicMNCPtr>& b) {
        if (a.first == b.first) {
            // this should actually never happen, but we should stay compatible with how the non-deterministic MNs did the sorting
            return a.second->collateralOutpoint < b.second->collateralOutpoint;
        }
        return a.first < b.first;
    });

    // take top maxSize entries and return it
    std::vector<CDeterministicMNCPtr> result;
    result.resize(std::min(maxSize, scores.size()));
    for (size_t i = 0; i < result.size(); i++) {
        result[i] = std::move(scores[i].second);
    }
    return result;
}

std::vector<std::pair<arith_uint256, CDeterministicMNCPtr>> CDeterministicMNList::CalculateScores(const uint256& modifier) const
{
    std::vector<std::pair<arith_uint256, CDeterministicMNCPtr>> scores;
    scores.reserve(GetAllMNsCount());
    ForEachMNShared(true, [&](const CDeterministicMNCPtr& dmn) {
        if (dmn->pdmnState->confirmedHash.IsNull()) {
            // we only take confirmed MNs into account to avoid hash grinding on the ProRegTxHash to sneak MNs into a
            // future quorums
            return;
        }
        // calculate sha256(sha256(proTxHash, confirmedHash), modifier) per MN
        // Please note that this is not a double-sha256 but a single-sha256
        // The first part is already precalculated (confirmedHashWithProRegTxHash)
        // TODO When https://github.com/bitcoin/bitcoin/pull/13191 gets backported, implement something that is similar but for single-sha256
        uint256 h;
        CSHA256 sha256;
        sha256.Write(dmn->pdmnState->confirmedHashWithProRegTxHash.begin(), dmn->pdmnState->confirmedHashWithProRegTxHash.size());
        sha256.Write(modifier.begin(), modifier.size());
        sha256.Finalize(h.begin());

        scores.emplace_back(UintToArith256(h), dmn);
    });

    return scores;
}

int CDeterministicMNList::CalcMaxPoSePenalty() const
{
    // Maximum PoSe penalty is dynamic and equals the number of registered MNs
    // It's however at least 100.
    // This means that the max penalty is usually equal to a full payment cycle
    return std::max(100, (int)GetAllMNsCount());
}

int CDeterministicMNList::CalcPenalty(int percent) const
{
    assert(percent > 0);
    return (CalcMaxPoSePenalty() * percent) / 100;
}

void CDeterministicMNList::PoSePunish(const uint256& proTxHash, int penalty, bool debugLogs)
{
    assert(penalty > 0);

    auto dmn = GetMN(proTxHash);
    if (!dmn) {
        throw(std::runtime_error(strprintf("%s: Can't find a masternode with proTxHash=%s", __func__, proTxHash.ToString())));
    }

    int maxPenalty = CalcMaxPoSePenalty();

    auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
    newState->nPoSePenalty += penalty;
    newState->nPoSePenalty = std::min(maxPenalty, newState->nPoSePenalty);

    if (debugLogs && dmn->pdmnState->nPoSePenalty != maxPenalty) {
        LogPrintf("CDeterministicMNList::%s -- punished MN %s, penalty %d->%d (max=%d)\n",
                  __func__, proTxHash.ToString(), dmn->pdmnState->nPoSePenalty, newState->nPoSePenalty, maxPenalty);
    }

    if (newState->nPoSePenalty >= maxPenalty && !newState->IsBanned()) {
        newState->BanIfNotBanned(nHeight);
        if (debugLogs) {
            LogPrintf("CDeterministicMNList::%s -- banned MN %s at height %d\n",
                      __func__, proTxHash.ToString(), nHeight);
        }
    }
    UpdateMN(proTxHash, newState);
}

void CDeterministicMNList::DecreaseScores()
{
    std::vector<CDeterministicMNCPtr> toDecrease;
    toDecrease.reserve(GetAllMNsCount() / 10);
    // only iterate and decrease for valid ones (not PoSe banned yet)
    // if a MN ever reaches the maximum, it stays in PoSe banned state until revived
    ForEachMNShared(true /* onlyValid */, [&toDecrease](auto& dmn) {
        // There is no reason to check if this MN is banned here since onlyValid=true will only run on non-banned MNs
        if (dmn->pdmnState->nPoSePenalty > 0) {
            toDecrease.emplace_back(dmn);
        }
    });

    for (const auto& proTxHash : toDecrease) {
        PoSeDecrease(*proTxHash);
    }
}

void CDeterministicMNList::PoSeDecrease(const CDeterministicMN& dmn)
{
    assert(dmn.pdmnState->nPoSePenalty > 0 && !dmn.pdmnState->IsBanned());

    auto newState = std::make_shared<CDeterministicMNState>(*dmn.pdmnState);
    newState->nPoSePenalty--;
    UpdateMN(dmn, newState);
}

CDeterministicMNListDiff CDeterministicMNList::BuildDiff(const CDeterministicMNList& to) const
{
    CDeterministicMNListDiff diffRet;

    to.ForEachMNShared(false, [this, &diffRet](const CDeterministicMNCPtr& toPtr) {
        auto fromPtr = GetMN(toPtr->proTxHash);
        if (fromPtr == nullptr) {
            diffRet.addedMNs.emplace_back(toPtr);
        } else if (fromPtr != toPtr || fromPtr->pdmnState != toPtr->pdmnState) {
            CDeterministicMNStateDiff stateDiff(*fromPtr->pdmnState, *toPtr->pdmnState);
            if (stateDiff.fields) {
                diffRet.updatedMNs.emplace(toPtr->GetInternalId(), std::move(stateDiff));
            }
        }
    });
    ForEachMN(false, [&](auto& fromPtr) {
        auto toPtr = to.GetMN(fromPtr.proTxHash);
        if (toPtr == nullptr) {
            diffRet.removedMns.emplace(fromPtr.GetInternalId());
        }
    });

    // added MNs need to be sorted by internalId so that these are added in correct order when the diff is applied later
    // otherwise internalIds will not match with the original list
    std::sort(diffRet.addedMNs.begin(), diffRet.addedMNs.end(), [](const CDeterministicMNCPtr& a, const CDeterministicMNCPtr& b) {
        return a->GetInternalId() < b->GetInternalId();
    });

    return diffRet;
}

CDeterministicMNList CDeterministicMNList::ApplyDiff(gsl::not_null<const CBlockIndex*> pindex, const CDeterministicMNListDiff& diff) const
{
    CDeterministicMNList result = *this;
    result.blockHash = pindex->GetBlockHash();
    result.nHeight = pindex->nHeight;

    for (const auto& id : diff.removedMns) {
        auto dmn = result.GetMNByInternalId(id);
        if (!dmn) {
            throw std::runtime_error(strprintf("%s: can't find a removed masternode, id=%d", __func__, id));
        }
        result.RemoveMN(dmn->proTxHash);
    }
    for (const auto& dmn : diff.addedMNs) {
        result.AddMN(dmn);
    }
    for (const auto& p : diff.updatedMNs) {
        auto dmn = result.GetMNByInternalId(p.first);
        if (!dmn) {
            throw std::runtime_error(strprintf("%s: can't find an updated masternode, id=%d", __func__, p.first));
        }
        result.UpdateMN(*dmn, p.second);
    }

    return result;
}

void CDeterministicMNList::AddMN(const CDeterministicMNCPtr& dmn, bool fBumpTotalCount)
{
    assert(dmn != nullptr);

    if (mnMap.find(dmn->proTxHash)) {
        throw(std::runtime_error(strprintf("%s: Can't add a masternode with a duplicate proTxHash=%s", __func__, dmn->proTxHash.ToString())));
    }
    if (mnInternalIdMap.find(dmn->GetInternalId())) {
        throw(std::runtime_error(strprintf("%s: Can't add a masternode with a duplicate internalId=%d", __func__, dmn->GetInternalId())));
    }

    // All mnUniquePropertyMap's updates must be atomic.
    // Using this temporary map as a checkpoint to roll back to in case of any issues.
    decltype(mnUniquePropertyMap) mnUniquePropertyMapSaved = mnUniquePropertyMap;

    if (!AddUniqueProperty(*dmn, dmn->collateralOutpoint)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't add a masternode %s with a duplicate collateralOutpoint=%s", __func__,
                dmn->proTxHash.ToString(), dmn->collateralOutpoint.ToStringShort())));
    }
    if (dmn->pdmnState->addr != CService() && !AddUniqueProperty(*dmn, dmn->pdmnState->addr)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't add a masternode %s with a duplicate address=%s", __func__,
                                           dmn->proTxHash.ToString(), dmn->pdmnState->addr.ToStringAddrPort())));
    }
    if (!AddUniqueProperty(*dmn, dmn->pdmnState->keyIDOwner)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't add a masternode %s with a duplicate keyIDOwner=%s", __func__,
                dmn->proTxHash.ToString(), EncodeDestination(PKHash(dmn->pdmnState->keyIDOwner)))));
    }
    if (dmn->pdmnState->pubKeyOperator != CBLSLazyPublicKey() && !AddUniqueProperty(*dmn, dmn->pdmnState->pubKeyOperator)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't add a masternode %s with a duplicate pubKeyOperator=%s", __func__,
                dmn->proTxHash.ToString(), dmn->pdmnState->pubKeyOperator.ToString())));
    }

    mnMap = mnMap.set(dmn->proTxHash, dmn);
    mnInternalIdMap = mnInternalIdMap.set(dmn->GetInternalId(), dmn->proTxHash);
    if (fBumpTotalCount) {
        // nTotalRegisteredCount acts more like a checkpoint, not as a limit,
        nTotalRegisteredCount = std::max(dmn->GetInternalId() + 1, (uint64_t)nTotalRegisteredCount);
    }
}

void CDeterministicMNList::UpdateMN(const CDeterministicMN& oldDmn, const std::shared_ptr<const CDeterministicMNState>& pdmnState)
{
    auto dmn = std::make_shared<CDeterministicMN>(oldDmn);
    auto oldState = dmn->pdmnState;

    // All mnUniquePropertyMap's updates must be atomic.
    // Using this temporary map as a checkpoint to roll back to in case of any issues.
    decltype(mnUniquePropertyMap) mnUniquePropertyMapSaved = mnUniquePropertyMap;

    if (!UpdateUniqueProperty(*dmn, oldState->addr, pdmnState->addr)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't update a masternode %s with a duplicate address=%s", __func__,
                                           oldDmn.proTxHash.ToString(), pdmnState->addr.ToStringAddrPort())));
    }
    if (!UpdateUniqueProperty(*dmn, oldState->keyIDOwner, pdmnState->keyIDOwner)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't update a masternode %s with a duplicate keyIDOwner=%s", __func__,
                oldDmn.proTxHash.ToString(), EncodeDestination(PKHash(pdmnState->keyIDOwner)))));
    }
    if (!UpdateUniqueProperty(*dmn, oldState->pubKeyOperator, pdmnState->pubKeyOperator)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't update a masternode %s with a duplicate pubKeyOperator=%s", __func__,
                oldDmn.proTxHash.ToString(), pdmnState->pubKeyOperator.ToString())));
    }
    dmn->pdmnState = pdmnState;
    mnMap = mnMap.set(oldDmn.proTxHash, dmn);
}

void CDeterministicMNList::UpdateMN(const uint256& proTxHash, const std::shared_ptr<const CDeterministicMNState>& pdmnState)
{
    auto oldDmn = mnMap.find(proTxHash);
    if (!oldDmn) {
        throw(std::runtime_error(strprintf("%s: Can't find a masternode with proTxHash=%s", __func__, proTxHash.ToString())));
    }
    UpdateMN(**oldDmn, pdmnState);
}

void CDeterministicMNList::UpdateMN(const CDeterministicMN& oldDmn, const CDeterministicMNStateDiff& stateDiff)
{
    auto oldState = oldDmn.pdmnState;
    auto newState = std::make_shared<CDeterministicMNState>(*oldState);
    stateDiff.ApplyToState(*newState);
    UpdateMN(oldDmn, newState);
}

void CDeterministicMNList::RemoveMN(const uint256& proTxHash)
{
    auto dmn = GetMN(proTxHash);
    if (!dmn) {
        throw(std::runtime_error(strprintf("%s: Can't find a masternode with proTxHash=%s", __func__, proTxHash.ToString())));
    }

    // All mnUniquePropertyMap's updates must be atomic.
    // Using this temporary map as a checkpoint to roll back to in case of any issues.
    decltype(mnUniquePropertyMap) mnUniquePropertyMapSaved = mnUniquePropertyMap;

    if (!DeleteUniqueProperty(*dmn, dmn->collateralOutpoint)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't delete a masternode %s with a collateralOutpoint=%s", __func__,
                proTxHash.ToString(), dmn->collateralOutpoint.ToStringShort())));
    }
    if (dmn->pdmnState->addr != CService() && !DeleteUniqueProperty(*dmn, dmn->pdmnState->addr)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't delete a masternode %s with a address=%s", __func__,
                                           proTxHash.ToString(), dmn->pdmnState->addr.ToStringAddrPort())));
    }
    if (!DeleteUniqueProperty(*dmn, dmn->pdmnState->keyIDOwner)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't delete a masternode %s with a keyIDOwner=%s", __func__,
                proTxHash.ToString(), EncodeDestination(PKHash(dmn->pdmnState->keyIDOwner)))));
    }
    if (dmn->pdmnState->pubKeyOperator != CBLSLazyPublicKey() &&
        !DeleteUniqueProperty(*dmn, dmn->pdmnState->pubKeyOperator)) {
        mnUniquePropertyMap = mnUniquePropertyMapSaved;
        throw(std::runtime_error(strprintf("%s: Can't delete a masternode %s with a pubKeyOperator=%s", __func__,
                proTxHash.ToString(), dmn->pdmnState->pubKeyOperator.ToString())));
    }

    mnMap = mnMap.erase(proTxHash);
    mnInternalIdMap = mnInternalIdMap.erase(dmn->GetInternalId());
}

bool CDeterministicMNManager::ProcessBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindex,
                                           BlockValidationState& state, const CCoinsViewCache& view,
                                           llmq::CQuorumSnapshotManager& qsnapman, bool fJustCheck,
                                           std::optional<MNListUpdates>& updatesRet)
{
    AssertLockHeld(cs_main);

    const auto& consensusParams = Params().GetConsensus();
    if (!DeploymentActiveAt(*pindex, consensusParams, Consensus::DEPLOYMENT_DIP0003)) {
        return true;
    }

    CDeterministicMNList oldList, newList;
    CDeterministicMNListDiff diff;

    int nHeight = pindex->nHeight;

    try {
        if (!BuildNewListFromBlock(block, pindex->pprev, state, view, newList, qsnapman, true)) {
            // pass the state returned by the function above
            return false;
        }

        if (fJustCheck) {
            return true;
        }

        newList.SetBlockHash(pindex->GetBlockHash());

        LOCK(cs);

        oldList = GetListForBlockInternal(pindex->pprev);
        diff = oldList.BuildDiff(newList);

        m_evoDb.Write(std::make_pair(DB_LIST_DIFF, newList.GetBlockHash()), diff);
        if ((nHeight % DISK_SNAPSHOT_PERIOD) == 0 || pindex->pprev == m_initial_snapshot_index) {
            m_evoDb.Write(std::make_pair(DB_LIST_SNAPSHOT, newList.GetBlockHash()), newList);
            mnListsCache.emplace(newList.GetBlockHash(), newList);
            LogPrintf("CDeterministicMNManager::%s -- Wrote snapshot. nHeight=%d, mapCurMNs.allMNsCount=%d\n",
                __func__, nHeight, newList.GetAllMNsCount());
        }

        diff.nHeight = pindex->nHeight;
        mnListDiffsCache.emplace(pindex->GetBlockHash(), diff);
    } catch (const std::exception& e) {
        LogPrintf("CDeterministicMNManager::%s -- internal error: %s\n", __func__, e.what());
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "failed-dmn-block");
    }

    if (diff.HasChanges()) {
        // Designated so the fix is visible: the positional form passed newList
        // into old_list and oldList into new_list, so every listener -- the
        // GUI, governance, coinjoin -- saw the two lists swapped. (dash#7154)
        updatesRet = {.old_list = oldList, .new_list = newList, .diff = diff};
    }

    if (nHeight == consensusParams.DIP0003EnforcementHeight) {
        if (!consensusParams.DIP0003EnforcementHash.IsNull() && consensusParams.DIP0003EnforcementHash != pindex->GetBlockHash()) {
            LogPrintf("CDeterministicMNManager::%s -- DIP3 enforcement block has wrong hash: hash=%s, expected=%s, nHeight=%d\n", __func__,
                    pindex->GetBlockHash().ToString(), consensusParams.DIP0003EnforcementHash.ToString(), nHeight);
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-dip3-enf-block");
        }
        LogPrintf("CDeterministicMNManager::%s -- DIP3 is enforced now. nHeight=%d\n", __func__, nHeight);
    }
    int current = to_cleanup.load();
    while (nHeight > current && !to_cleanup.compare_exchange_weak(current, nHeight)) {
        // Loop continues if compare_exchange_weak failed (another thread changed it) (current is updated to the new value in to_cleanup)
    }
    return true;
}

bool CDeterministicMNManager::UndoBlock(gsl::not_null<const CBlockIndex*> pindex, std::optional<MNListUpdates>& updatesRet)
{
    int nHeight = pindex->nHeight;
    uint256 blockHash = pindex->GetBlockHash();

    CDeterministicMNList curList;
    CDeterministicMNList prevList;
    CDeterministicMNListDiff diff;
    {
        LOCK(cs);
        m_evoDb.Read(std::make_pair(DB_LIST_DIFF, blockHash), diff);

        if (diff.HasChanges()) {
            // need to call this before erasing
            curList = GetListForBlockInternal(pindex);
            prevList = GetListForBlockInternal(pindex->pprev);
        }

        mnListsCache.erase(blockHash);
        mnListDiffsCache.erase(blockHash);
    }

    if (diff.HasChanges()) {
        auto inversedDiff = curList.BuildDiff(prevList);
        updatesRet = {.old_list = curList, .new_list = prevList, .diff = inversedDiff};
    }

    const auto& consensusParams = Params().GetConsensus();
    if (nHeight == consensusParams.DIP0003EnforcementHeight) {
        LogPrintf("CDeterministicMNManager::%s -- DIP3 is not enforced anymore. nHeight=%d\n", __func__, nHeight);
    }

    return true;
}

void CDeterministicMNManager::UpdatedBlockTip(gsl::not_null<const CBlockIndex*> pindex)
{
    LOCK(cs);

    tipIndex = pindex;
}

bool CDeterministicMNManager::BuildNewListFromBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindexPrev,
                                                    BlockValidationState& state, const CCoinsViewCache& view,
                                                    CDeterministicMNList& mnListRet,
                                                    llmq::CQuorumSnapshotManager& qsnapman, bool debugLogs)
{
    return RebuildListFromBlock(block, pindexPrev, GetListForBlock(pindexPrev), state, view, mnListRet, qsnapman,
                                debugLogs);
}

bool CDeterministicMNManager::RebuildListFromBlock(const CBlock& block, gsl::not_null<const CBlockIndex*> pindexPrev,
                                                   const CDeterministicMNList& prevList, BlockValidationState& state,
                                                   const CCoinsViewCache& view, CDeterministicMNList& mnListRet,
                                                   llmq::CQuorumSnapshotManager& qsnapman, bool debugLogs)
{
    // prevList is either the default-constructed empty list or the list that
    // belongs to pindexPrev. Anything else would build on the wrong base and
    // produce a diff that verifies against nothing.
    assert(prevList == CDeterministicMNList() || prevList.GetBlockHash() == pindexPrev->GetBlockHash());

    int nHeight = pindexPrev->nHeight + 1;
    // Cross-scheme operator keys only become expressible once basic BLS is available, which is what
    // V19 activates. Before it the rule is unreachable, so gating here cannot invalidate history.
    const bool is_v19_deployed{DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_V19)};

    CDeterministicMNList oldList = prevList;
    CDeterministicMNList newList = oldList;
    newList.SetBlockHash(uint256()); // we can't know the final block hash, so better not return a (invalid) block hash
    newList.SetHeight(nHeight);

    auto payee = oldList.GetMNPayee(pindexPrev);

    // we iterate the oldList here and update the newList
    // this is only valid as long these have not diverged at this point, which is the case as long as we don't add
    // code above this loop that modifies newList
    oldList.ForEachMN(false, [&pindexPrev, &newList](auto& dmn) {
        if (!dmn.pdmnState->confirmedHash.IsNull()) {
            // already confirmed
            return;
        }
        // this works on the previous block, so confirmation will happen one block after nMasternodeMinimumConfirmations
        // has been reached, but the block hash will then point to the block at nMasternodeMinimumConfirmations
        int nConfirmations = pindexPrev->nHeight - dmn.pdmnState->nRegisteredHeight;
        if (nConfirmations >= Params().GetConsensus().nMasternodeMinimumConfirmations) {
            auto newState = std::make_shared<CDeterministicMNState>(*dmn.pdmnState);
            newState->UpdateConfirmedHash(dmn.proTxHash, pindexPrev->GetBlockHash());
            newList.UpdateMN(dmn.proTxHash, newState);
        }
    });

    newList.DecreaseScores();

    const bool isMNRewardReallocation{DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_MN_RR)};

    // we skip the coinbase
    for (int i = 1; i < (int)block.vtx.size(); i++) {
        const CTransaction& tx = *block.vtx[i];

        if (!tx.IsSpecialTxVersion()) {
            // only interested in special TXs
            continue;
        }

        if (tx.nType == TRANSACTION_PROVIDER_REGISTER) {
            const auto opt_proTx = GetTxPayload<CProRegTx>(tx);
            if (!opt_proTx) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-payload");
            }
            auto& proTx = *opt_proTx;

            auto dmn = std::make_shared<CDeterministicMN>(newList.GetTotalRegisteredCount(), proTx.nType);
            dmn->proTxHash = tx.GetHash();

            // collateralOutpoint is either pointing to an external collateral or to the ProRegTx itself
            if (proTx.collateralOutpoint.hash.IsNull()) {
                dmn->collateralOutpoint = COutPoint(tx.GetHash(), proTx.collateralOutpoint.n);
            } else {
                dmn->collateralOutpoint = proTx.collateralOutpoint;
            }

            // Complain about spent collaterals only when we process the tip.
            // This is safe because blocks below the tip were verified when they
            // were connected initially, and replaying one of them for a repair
            // sees a UTXO view in which the collateral has long been spent.
            if (!view.GetBestBlock().IsNull()) {
                Coin coin;
                CAmount expectedCollateral = GetMnType(proTx.nType).collat_amount;
                if (!proTx.collateralOutpoint.hash.IsNull() && (!view.GetCoin(dmn->collateralOutpoint, coin) || coin.IsSpent() || coin.out.nValue != expectedCollateral)) {
                    // should actually never get to this point as CheckProRegTx should have handled this case.
                    // We do this additional check nevertheless to be 100% sure
                    return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-collateral");
                }
            }

            auto replacedDmn = newList.GetMNByCollateral(dmn->collateralOutpoint);
            if (replacedDmn != nullptr) {
                // This might only happen with a ProRegTx that refers an external collateral
                // In that case the new ProRegTx will replace the old one. This means the old one is removed
                // and the new one is added like a completely fresh one, which is also at the bottom of the payment list
                newList.RemoveMN(replacedDmn->proTxHash);
                if (debugLogs) {
                    LogPrintf("CDeterministicMNManager::%s -- MN %s removed from list because collateral was used for a new ProRegTx. collateralOutpoint=%s, nHeight=%d, mapCurMNs.allMNsCount=%d\n",
                              __func__, replacedDmn->proTxHash.ToString(), dmn->collateralOutpoint.ToStringShort(), nHeight, newList.GetAllMNsCount());
                }
            }

            if (newList.HasUniqueProperty(proTx.addr)) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-dup-addr");
            }
            if (newList.HasUniqueProperty(proTx.keyIDOwner) || newList.HasUniqueProperty(proTx.pubKeyOperator)) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-dup-key");
            }

            dmn->nOperatorReward = proTx.nOperatorReward;

            auto dmnState = std::make_shared<CDeterministicMNState>(proTx);
            dmnState->nRegisteredHeight = nHeight;
            if (proTx.addr == CService()) {
                // start in banned pdmnState as we need to wait for a ProUpServTx
                dmnState->BanIfNotBanned(nHeight);
            }
            dmn->pdmnState = dmnState;

            // CheckProRegTx ran against pindexPrev, so transactions in this same block are invisible
            // to each other and two of them could claim one operator key under different encodings.
            // Re-probe the list as rebuilt so far. AddMN() reports a duplicate by throwing, which
            // would escape block assembly, so reject cleanly here instead.
            if (is_v19_deployed &&
                newList.HasOperatorKeyUnderAnyScheme(dmn->pdmnState->pubKeyOperator.Get(), /*self=*/uint256())) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-dup-key");
            }

            newList.AddMN(dmn);

            if (debugLogs) {
                LogPrintf("CDeterministicMNManager::%s -- MN %s added at height %d: %s\n",
                    __func__, tx.GetHash().ToString(), nHeight, proTx.ToString());
            }
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_SERVICE) {
            const auto opt_proTx = GetTxPayload<CProUpServTx>(tx);
            if (!opt_proTx) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-payload");
            }

            if (newList.HasUniqueProperty(opt_proTx->addr) && newList.GetUniquePropertyMN(opt_proTx->addr)->proTxHash != opt_proTx->proTxHash) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-dup-addr");
            }

            auto dmn = newList.GetMN(opt_proTx->proTxHash);
            if (!dmn) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-hash");
            }
            if (opt_proTx->nType != dmn->nType) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-type-mismatch");
            }
            if (!IsValidMnType(opt_proTx->nType)) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-type");
            }

            auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
            newState->addr = opt_proTx->addr;
            newState->scriptOperatorPayout = opt_proTx->scriptOperatorPayout;
            if (opt_proTx->nType == MnType::Compute) {
                newState->computeDescriptor = opt_proTx->computeDescriptor;
            }
            if (newState->IsBanned()) {
                // only revive when all keys are set
                if (newState->pubKeyOperator != CBLSLazyPublicKey() && !newState->keyIDVoting.IsNull() &&
                    !newState->keyIDOwner.IsNull()) {
                    newState->Revive(nHeight);
                    if (debugLogs) {
                        LogPrintf("CDeterministicMNManager::%s -- MN %s revived at height %d\n",
                            __func__, opt_proTx->proTxHash.ToString(), nHeight);
                    }
                }
            }

            newList.UpdateMN(opt_proTx->proTxHash, newState);
            if (debugLogs) {
                LogPrintf("CDeterministicMNManager::%s -- MN %s updated at height %d: %s\n",
                    __func__, opt_proTx->proTxHash.ToString(), nHeight, opt_proTx->ToString());
            }
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_REGISTRAR) {
            const auto opt_proTx = GetTxPayload<CProUpRegTx>(tx);
            if (!opt_proTx) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-payload");
            }

            auto dmn = newList.GetMN(opt_proTx->proTxHash);
            if (!dmn) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
            if (newState->pubKeyOperator != opt_proTx->pubKeyOperator) {
                // reset all operator related fields and put MN into PoSe-banned state in case the operator key changes
                newState->ResetOperatorFields();
                newState->BanIfNotBanned(nHeight);
                // we update pubKeyOperator here, make sure state version matches
                newState->nVersion = opt_proTx->nVersion;
                newState->pubKeyOperator = opt_proTx->pubKeyOperator;
            }
            newState->keyIDVoting = opt_proTx->keyIDVoting;
            newState->scriptPayout = opt_proTx->scriptPayout;

            // As in the registration path: CheckProUpRegTx ran against pindexPrev, so a second
            // transaction in this same block claiming the same key under the other encoding is
            // invisible to it. Same key-change scoping as that check -- an update keeping its own key
            // cannot create a duplicate. UpdateMN() reports duplicates by throwing, which would
            // escape block assembly, so reject cleanly here instead.
            if (is_v19_deployed && !(opt_proTx->pubKeyOperator == dmn->pdmnState->pubKeyOperator) &&
                newList.HasOperatorKeyUnderAnyScheme(opt_proTx->pubKeyOperator.Get(),
                                                     /*self=*/opt_proTx->proTxHash)) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-dup-key");
            }

            newList.UpdateMN(opt_proTx->proTxHash, newState);

            if (debugLogs) {
                LogPrintf("CDeterministicMNManager::%s -- MN %s updated at height %d: %s\n",
                    __func__, opt_proTx->proTxHash.ToString(), nHeight, opt_proTx->ToString());
            }
        } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_REVOKE) {
            const auto opt_proTx = GetTxPayload<CProUpRevTx>(tx);
            if (!opt_proTx) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-payload");
            }

            auto dmn = newList.GetMN(opt_proTx->proTxHash);
            if (!dmn) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-protx-hash");
            }
            auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
            newState->ResetOperatorFields();
            newState->BanIfNotBanned(nHeight);
            newState->nRevocationReason = opt_proTx->nReason;

            newList.UpdateMN(opt_proTx->proTxHash, newState);

            if (debugLogs) {
                LogPrintf("CDeterministicMNManager::%s -- MN %s revoked operator key at height %d: %s\n",
                    __func__, opt_proTx->proTxHash.ToString(), nHeight, opt_proTx->ToString());
            }
        } else if (tx.nType == TRANSACTION_QUORUM_COMMITMENT) {
            const auto opt_qc = GetTxPayload<llmq::CFinalCommitmentTxPayload>(tx);
            if (!opt_qc) {
                return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-payload");
            }
            if (!opt_qc->commitment.IsNull()) {
                const auto& llmq_params_opt = Params().GetLLMQ(opt_qc->commitment.llmqType);
                if (!llmq_params_opt.has_value()) {
                    return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-commitment-type");
                }
                int qcnHeight = int(opt_qc->nHeight);
                int quorumHeight = qcnHeight - (qcnHeight % llmq_params_opt->dkgInterval) + int(opt_qc->commitment.quorumIndex);
                auto pQuorumBaseBlockIndex = pindexPrev->GetAncestor(quorumHeight);
                if (!pQuorumBaseBlockIndex || pQuorumBaseBlockIndex->GetBlockHash() != opt_qc->commitment.quorumHash) {
                    // we should actually never get into this case as validation should have caught it...but let's be sure
                    return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-qc-quorum-hash");
                }

                HandleQuorumCommitment(opt_qc->commitment, pQuorumBaseBlockIndex, newList, qsnapman, debugLogs);
            }
        }
    }

    // we skip the coinbase
    for (int i = 1; i < (int)block.vtx.size(); i++) {
        const CTransaction& tx = *block.vtx[i];

        // check if any existing MN collateral is spent by this transaction
        for (const auto& in : tx.vin) {
            auto dmn = newList.GetMNByCollateral(in.prevout);
            if (dmn && dmn->collateralOutpoint == in.prevout) {
                newList.RemoveMN(dmn->proTxHash);

                if (debugLogs) {
                    LogPrintf("CDeterministicMNManager::%s -- MN %s removed from list because collateral was spent. collateralOutpoint=%s, nHeight=%d, mapCurMNs.allMNsCount=%d\n",
                              __func__, dmn->proTxHash.ToString(), dmn->collateralOutpoint.ToStringShort(), nHeight, newList.GetAllMNsCount());
                }
            }
        }
    }

    // The payee for the current block was determined by the previous block's list, but it might have disappeared in the
    // current block. We still pay that MN one last time, however.
    if (payee && newList.HasMN(payee->proTxHash)) {
        auto dmn = newList.GetMN(payee->proTxHash);
        // HasMN has reported that GetMN should succeed, enforce that.
        assert(dmn);
        auto newState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
        newState->nLastPaidHeight = nHeight;
        // Until MNRewardReallocation, a type whose payment weight is above one
        // is paid that many blocks in a row; count where in its run this payee is.
        // Note: If the payee wasn't found in the current block that's fine
        if (newList.GetEffectivePaymentWeight(*dmn) > 1 && !isMNRewardReallocation) {
            ++newState->nConsecutivePayments;
            if (debugLogs) {
                LogPrint(BCLog::MNPAYMENTS, "CDeterministicMNManager::%s -- MN %s holds multiple payout slots, bumping nConsecutivePayments to %d\n",
                          __func__, dmn->proTxHash.ToString(), newState->nConsecutivePayments);
            }
        }
        newList.UpdateMN(payee->proTxHash, newState);
        if (debugLogs) {
            dmn = newList.GetMN(payee->proTxHash);
            // Since the previous GetMN query returned a value, after an update, querying the same
            // hash *must* give us a result. If it doesn't, that would be a potential logic bug.
            assert(dmn);
            LogPrint(BCLog::MNPAYMENTS, "CDeterministicMNManager::%s -- MN %s, nConsecutivePayments=%d\n",
                      __func__, dmn->proTxHash.ToString(), dmn->pdmnState->nConsecutivePayments);
        }
    }

    // reset nConsecutivePayments on non-paid weighted-payout masternodes
    auto newList2 = newList;
    newList2.ForEachMN(false, [&](auto& dmn) {
        // type weight rather than effective: a stale run counter must clear
        // even while the node's certificate is lapsed
        if (GetMnType(dmn.nType).payment_weight <= 1) return;
        if (payee != nullptr && dmn.proTxHash == payee->proTxHash && !isMNRewardReallocation) return;
        if (dmn.pdmnState->nConsecutivePayments == 0) return;
        if (debugLogs) {
            LogPrint(BCLog::MNPAYMENTS, "CDeterministicMNManager::%s -- MN %s, reset nConsecutivePayments %d->0\n",
                      __func__, dmn.proTxHash.ToString(), dmn.pdmnState->nConsecutivePayments);
        }
        auto newState = std::make_shared<CDeterministicMNState>(*dmn.pdmnState);
        newState->nConsecutivePayments = 0;
        newList.UpdateMN(dmn.proTxHash, newState);
    });

    mnListRet = std::move(newList);

    return true;
}

void CDeterministicMNManager::HandleQuorumCommitment(const llmq::CFinalCommitment& qc,
                                                     gsl::not_null<const CBlockIndex*> pQuorumBaseBlockIndex,
                                                     CDeterministicMNList& mnList,
                                                     llmq::CQuorumSnapshotManager& qsnapman, bool debugLogs)
{
    // The commitment has already been validated at this point, so it's safe to use members of it

    auto members = llmq::utils::GetAllQuorumMembers(qc.llmqType, *this, qsnapman, pQuorumBaseBlockIndex);

    for (size_t i = 0; i < members.size(); i++) {
        if (!mnList.HasMN(members[i]->proTxHash)) {
            continue;
        }
        if (!qc.validMembers[i]) {
            // punish MN for failed DKG participation
            // The idea is to immediately ban a MN when it fails 2 DKG sessions with only a few blocks in-between
            // If there were enough blocks between failures, the MN has a chance to recover as he reduces his penalty by 1 for every block
            // If it however fails 3 times in the timespan of a single payment cycle, it should definitely get banned
            mnList.PoSePunish(members[i]->proTxHash, mnList.CalcPenalty(66), debugLogs);
        }
    }
}

CDeterministicMNList CDeterministicMNManager::GetListForBlockInternal(gsl::not_null<const CBlockIndex*> pindex)
{
    CDeterministicMNList snapshot;

    if (!DeploymentActiveAt(*pindex, Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
        return snapshot;
    }

    AssertLockHeld(cs);

    std::list<const CBlockIndex*> listDiffIndexes;

    while (true) {
        // try using cache before reading from disk
        auto itLists = mnListsCache.find(pindex->GetBlockHash());
        if (itLists != mnListsCache.end()) {
            snapshot = itLists->second;
            break;
        }

        if (m_evoDb.Read(std::make_pair(DB_LIST_SNAPSHOT, pindex->GetBlockHash()), snapshot)) {
            mnListsCache.emplace(pindex->GetBlockHash(), snapshot);
            break;
        }

        // no snapshot found yet, check diffs
        auto itDiffs = mnListDiffsCache.find(pindex->GetBlockHash());
        if (itDiffs != mnListDiffsCache.end()) {
            listDiffIndexes.emplace_front(pindex);
            pindex = pindex->pprev;
            continue;
        }

        CDeterministicMNListDiff diff;
        if (!m_evoDb.Read(std::make_pair(DB_LIST_DIFF, pindex->GetBlockHash()), diff)) {
            // no snapshot and no diff on disk means that it's the initial snapshot
            m_initial_snapshot_index = pindex;
            snapshot = CDeterministicMNList(pindex->GetBlockHash(), pindex->nHeight, 0);
            mnListsCache.emplace(pindex->GetBlockHash(), snapshot);
            LogPrintf("CDeterministicMNManager::%s -- initial snapshot. blockHash=%s nHeight=%d\n",
                    __func__, snapshot.GetBlockHash().ToString(), snapshot.GetHeight());
            break;
        }

        diff.nHeight = pindex->nHeight;
        mnListDiffsCache.emplace(pindex->GetBlockHash(), std::move(diff));
        listDiffIndexes.emplace_front(pindex);
        pindex = pindex->pprev;
    }

    for (const auto& diffIndex : listDiffIndexes) {
        const auto& diff = mnListDiffsCache.at(diffIndex->GetBlockHash());
        if (diff.HasChanges()) {
            snapshot = snapshot.ApplyDiff(diffIndex, diff);
        } else {
            snapshot.SetBlockHash(diffIndex->GetBlockHash());
            snapshot.SetHeight(diffIndex->nHeight);
        }
    }

    if (tipIndex) {
        // always keep a snapshot for the tip
        if (snapshot.GetBlockHash() == tipIndex->GetBlockHash()) {
            mnListsCache.emplace(snapshot.GetBlockHash(), snapshot);
        } else {
            // keep snapshots for yet alive quorums
            if (ranges::any_of(Params().GetConsensus().llmqs,
                               [&snapshot, this](const auto& params) EXCLUSIVE_LOCKS_REQUIRED(cs) {
                                   AssertLockHeld(cs);
                                   return (snapshot.GetHeight() % params.dkgInterval == 0) &&
                                          (snapshot.GetHeight() + params.dkgInterval * (params.keepOldConnections + 1) >=
                                           tipIndex->nHeight);
                               })) {
                mnListsCache.emplace(snapshot.GetBlockHash(), snapshot);
            }
        }
    }

    assert(snapshot.GetHeight() != -1);
    return snapshot;
}

CDeterministicMNList CDeterministicMNManager::GetListAtChainTip()
{
    LOCK(cs);
    if (!tipIndex) {
        return {};
    }
    return GetListForBlockInternal(tipIndex);
}

bool CDeterministicMNManager::IsProTxWithCollateral(const CTransactionRef& tx, uint32_t n)
{
    if (!tx->IsSpecialTxVersion() || tx->nType != TRANSACTION_PROVIDER_REGISTER) {
        return false;
    }
    const auto opt_proTx = GetTxPayload<CProRegTx>(*tx);
    if (!opt_proTx) {
        return false;
    }
    auto& proTx = *opt_proTx;

    if (!proTx.collateralOutpoint.hash.IsNull()) {
        return false;
    }
    if (proTx.collateralOutpoint.n >= tx->vout.size() || proTx.collateralOutpoint.n != n) {
        return false;
    }


    if (const CAmount expectedCollateral = GetMnType(proTx.nType).collat_amount; tx->vout[n].nValue != expectedCollateral) {
        return false;
    }
    return true;
}

void CDeterministicMNManager::CleanupCache(int nHeight)
{
    AssertLockHeld(cs);

    std::vector<uint256> toDeleteLists;
    std::vector<uint256> toDeleteDiffs;
    for (const auto& p : mnListsCache) {
        if (p.second.GetHeight() + LIST_DIFFS_CACHE_SIZE < nHeight) {
            // too old, drop it
            toDeleteLists.emplace_back(p.first);
            continue;
        }
        if (tipIndex != nullptr && p.first == tipIndex->GetBlockHash()) {
            // it's a snapshot for the tip, keep it
            continue;
        }
        bool fQuorumCache = ranges::any_of(Params().GetConsensus().llmqs, [&nHeight, &p](const auto& params){
            return (p.second.GetHeight() % params.dkgInterval == 0) &&
                   (p.second.GetHeight() + params.dkgInterval * (params.keepOldConnections + 1) >= nHeight);
        });
        if (fQuorumCache) {
            // at least one quorum could be using it, keep it
            continue;
        }
        // none of the above, drop it
        toDeleteLists.emplace_back(p.first);
    }
    for (const auto& h : toDeleteLists) {
        mnListsCache.erase(h);
    }
    for (const auto& p : mnListDiffsCache) {
        if (p.second.nHeight + LIST_DIFFS_CACHE_SIZE < nHeight) {
            toDeleteDiffs.emplace_back(p.first);
        }
    }
    for (const auto& h : toDeleteDiffs) {
        mnListDiffsCache.erase(h);
    }

}

[[nodiscard]] static bool EraseOldDBData(CDBWrapper& db, const std::vector<std::string>& db_key_prefixes)
{
    bool erased{false};
    for(const auto& db_key_prefix : db_key_prefixes) {
        CDBBatch batch{db};
        std::unique_ptr<CDBIterator> it{db.NewIterator()};
        std::pair firstKey{db_key_prefix, uint256()};
        it->Seek(firstKey);
        while (it->Valid()) {
            decltype(firstKey) curKey;
            if (!it->GetKey(curKey) || std::get<0>(curKey) != db_key_prefix) {
                break;
            }
            batch.Erase(curKey);
            erased = true;
            it->Next();
        }
        if (erased) {
            LogPrintf("CDeterministicMNManager::%s -- updating db...\n", __func__);
            db.WriteBatch(batch);
            LogPrintf("CDeterministicMNManager::%s -- done cleaning old data for %s\n", __func__, db_key_prefix);
        }
    }
    return erased;
}

bool CDeterministicMNManager::MigrateDBIfNeeded()
{
    static const std::string DB_OLD_LIST_SNAPSHOT = "dmn_S";
    static const std::string DB_OLD_LIST_DIFF = "dmn_D";
    static const std::string DB_OLD_BEST_BLOCK = "b_b2";
    static const std::string DB_OLD_BEST_BLOCK2 = "b_b3";
    const auto& consensusParams = Params().GetConsensus();

    LOCK(cs_main);

    LogPrintf("CDeterministicMNManager::%s -- upgrading DB to migrate MN type\n", __func__);

    if (m_chainstate.m_chain.Tip() == nullptr) {
        // should have no records
        LogPrintf("CDeterministicMNManager::%s -- Chain empty. evoDB:%d.\n", __func__, m_evoDb.IsEmpty());
        return m_evoDb.IsEmpty();
    }

    if (m_evoDb.GetRawDB().Exists(EVODB_BEST_BLOCK) || m_evoDb.GetRawDB().Exists(DB_OLD_BEST_BLOCK2)) {
        if (EraseOldDBData(m_evoDb.GetRawDB(), {DB_OLD_LIST_DIFF, DB_OLD_LIST_SNAPSHOT})) {
            // we messed up, make sure this time we actually drop old data
            LogPrintf("CDeterministicMNManager::%s -- migration already done. cleaned old data.\n", __func__);
            m_evoDb.GetRawDB().CompactFull();
            LogPrintf("CDeterministicMNManager::%s -- done compacting database\n", __func__);
            // flush it to disk
            if (!m_evoDb.CommitRootTransaction()) {
                LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
                return false;
            }
        } else {
            LogPrintf("CDeterministicMNManager::%s -- migration already done. skipping.\n", __func__);
        }
        return true;
    }

    // Removing the old EVODB_BEST_BLOCK value early results in older version to crash immediately, even if the upgrade
    // process is cancelled in-between. But if the new version sees that the old EVODB_BEST_BLOCK is already removed,
    // then we must assume that the upgrade process was already running before but was interrupted.
    if (m_chainstate.m_chain.Height() > 1 && !m_evoDb.GetRawDB().Exists(DB_OLD_BEST_BLOCK)) {
        LogPrintf("CDeterministicMNManager::%s -- previous migration attempt failed.\n", __func__);
        return false;
    }
    m_evoDb.GetRawDB().Erase(DB_OLD_BEST_BLOCK);

    if (!DeploymentActiveAt(*m_chainstate.m_chain.Tip(), consensusParams, Consensus::DEPLOYMENT_DIP0003)) {
        // not reached DIP3 height yet, so no upgrade needed
        LogPrintf("CDeterministicMNManager::%s -- migration not needed. dip3 not reached\n", __func__);
        auto dbTx = m_evoDb.BeginTransaction();
        m_evoDb.WriteBestBlock(m_chainstate.m_chain.Tip()->GetBlockHash());
        dbTx->Commit();
        if (!m_evoDb.CommitRootTransaction()) {
            LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
            return false;
        }
        return true;
    }

    if (DeploymentActiveAt(*m_chainstate.m_chain.Tip(), consensusParams, Consensus::DEPLOYMENT_V19)) {
        // too late
        LogPrintf("CDeterministicMNManager::%s -- migration is not possible\n", __func__);
        return false;
    }


    CDBBatch batch(m_evoDb.GetRawDB());

    for (const auto nHeight : irange::range(Params().GetConsensus().DIP0003Height, m_chainstate.m_chain.Height() + 1)) {
        auto pindex = m_chainstate.m_chain[nHeight];
        // Unserialise CDeterministicMNListDiff using MN_OLD_FORMAT and set it's type to the default value TYPE_REGULAR_MASTERNODE
        // It will be later written with format MN_CURRENT_FORMAT which includes the type field and MN state bls version.
        CDataStream diff_data(SER_DISK, CLIENT_VERSION);
        if (!m_evoDb.GetRawDB().ReadDataStream(std::make_pair(DB_OLD_LIST_DIFF, pindex->GetBlockHash()), diff_data)) {
            LogPrintf("CDeterministicMNManager::%s -- missing CDeterministicMNListDiff at height %d\n", __func__, nHeight);
            return false;
        }
        CDeterministicMNListDiff mndiff;
        mndiff.Unserialize(diff_data, CDeterministicMN::MN_OLD_FORMAT);
        batch.Write(std::make_pair(DB_LIST_DIFF, pindex->GetBlockHash()), mndiff);
        CDataStream snapshot_data(SER_DISK, CLIENT_VERSION);
        if (!m_evoDb.GetRawDB().ReadDataStream(std::make_pair(DB_OLD_LIST_SNAPSHOT, pindex->GetBlockHash()), snapshot_data)) {
            // it's ok, we write snapshots every DISK_SNAPSHOT_PERIOD blocks only
            continue;
        }
        CDeterministicMNList mnList;
        mnList.Unserialize(snapshot_data, CDeterministicMN::MN_OLD_FORMAT);
        batch.Write(std::make_pair(DB_LIST_SNAPSHOT, pindex->GetBlockHash()), mnList);
        m_evoDb.GetRawDB().WriteBatch(batch);
        batch.Clear();
        LogPrintf("CDeterministicMNManager::%s -- wrote snapshot at height %d\n", __func__, nHeight);
    }

    m_evoDb.GetRawDB().WriteBatch(batch);

    // Writing EVODB_BEST_BLOCK (which is b_b4 now) marks the DB as upgraded
    auto dbTx = m_evoDb.BeginTransaction();
    m_evoDb.WriteBestBlock(m_chainstate.m_chain.Tip()->GetBlockHash());
    dbTx->Commit();

    LogPrintf("CDeterministicMNManager::%s -- done migrating\n", __func__);

    if (EraseOldDBData(m_evoDb.GetRawDB(), {DB_OLD_LIST_DIFF, DB_OLD_LIST_SNAPSHOT})) {
        LogPrintf("CDeterministicMNManager::%s -- done cleaning old data\n", __func__);
    }

    m_evoDb.GetRawDB().CompactFull();

    LogPrintf("CDeterministicMNManager::%s -- done compacting database\n", __func__);

    // flush it to disk
    if (!m_evoDb.CommitRootTransaction()) {
        LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
        return false;
    }

    return true;
}

bool CDeterministicMNManager::MigrateDBIfNeeded2()
{
    static const std::string DB_OLD_LIST_SNAPSHOT = "dmn_S2";
    static const std::string DB_OLD_LIST_DIFF = "dmn_D2";
    static const std::string DB_OLD_BEST_BLOCK = "b_b3";
    const auto& consensusParams = Params().GetConsensus();

    LOCK(cs_main);

    LogPrintf("CDeterministicMNManager::%s -- upgrading DB to migrate MN state bls version\n", __func__);

    if (m_chainstate.m_chain.Tip() == nullptr) {
        // should have no records
        LogPrintf("CDeterministicMNManager::%s -- Chain empty. evoDB:%d.\n", __func__, m_evoDb.IsEmpty());
        return m_evoDb.IsEmpty();
    }

    if (m_evoDb.GetRawDB().Exists(EVODB_BEST_BLOCK)) {
        if (EraseOldDBData(m_evoDb.GetRawDB(), {DB_OLD_LIST_DIFF, DB_OLD_LIST_SNAPSHOT})) {
            // we messed up, make sure this time we actually drop old data
            LogPrintf("CDeterministicMNManager::%s -- migration already done. cleaned old data.\n", __func__);
            m_evoDb.GetRawDB().CompactFull();
            LogPrintf("CDeterministicMNManager::%s -- done compacting database\n", __func__);
            // flush it to disk
            if (!m_evoDb.CommitRootTransaction()) {
                LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
                return false;
            }
        } else {
            LogPrintf("CDeterministicMNManager::%s -- migration already done. skipping.\n", __func__);
        }
        return true;
    }

    // Removing the old EVODB_BEST_BLOCK value early results in older version to crash immediately, even if the upgrade
    // process is cancelled in-between. But if the new version sees that the old EVODB_BEST_BLOCK is already removed,
    // then we must assume that the upgrade process was already running before but was interrupted.
    if (m_chainstate.m_chain.Height() > 1 && !m_evoDb.GetRawDB().Exists(DB_OLD_BEST_BLOCK)) {
        LogPrintf("CDeterministicMNManager::%s -- previous migration attempt failed.\n", __func__);
        return false;
    }
    m_evoDb.GetRawDB().Erase(DB_OLD_BEST_BLOCK);

    if (!DeploymentActiveAt(*m_chainstate.m_chain.Tip(), consensusParams, Consensus::DEPLOYMENT_DIP0003)) {
        // not reached DIP3 height yet, so no upgrade needed
        LogPrintf("CDeterministicMNManager::%s -- migration not needed. dip3 not reached\n", __func__);
        auto dbTx = m_evoDb.BeginTransaction();
        m_evoDb.WriteBestBlock(m_chainstate.m_chain.Tip()->GetBlockHash());
        dbTx->Commit();
        if (!m_evoDb.CommitRootTransaction()) {
            LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
            return false;
        }
        return true;
    }

    if (DeploymentActiveAt(*m_chainstate.m_chain.Tip(), consensusParams, Consensus::DEPLOYMENT_V19)) {
        // too late
        LogPrintf("CDeterministicMNManager::%s -- migration is not possible\n", __func__);
        return false;
    }

    CDBBatch batch(m_evoDb.GetRawDB());

    for (const auto nHeight : irange::range(Params().GetConsensus().DIP0003Height, m_chainstate.m_chain.Height() + 1)) {
        auto pindex = m_chainstate.m_chain[nHeight];
        // Unserialise CDeterministicMNListDiff using MN_TYPE_FORMAT and set MN state bls version to LEGACY_BLS_VERSION.
        // It will be later written with format MN_CURRENT_FORMAT which includes the type field.
        CDataStream diff_data(SER_DISK, CLIENT_VERSION);
        if (!m_evoDb.GetRawDB().ReadDataStream(std::make_pair(DB_OLD_LIST_DIFF, pindex->GetBlockHash()), diff_data)) {
            LogPrintf("CDeterministicMNManager::%s -- missing CDeterministicMNListDiff at height %d\n", __func__, nHeight);
            return false;
        }
        CDeterministicMNListDiff mndiff;
        mndiff.Unserialize(diff_data, CDeterministicMN::MN_TYPE_FORMAT);
        batch.Write(std::make_pair(DB_LIST_DIFF, pindex->GetBlockHash()), mndiff);
        CDataStream snapshot_data(SER_DISK, CLIENT_VERSION);
        if (!m_evoDb.GetRawDB().ReadDataStream(std::make_pair(DB_OLD_LIST_SNAPSHOT, pindex->GetBlockHash()), snapshot_data)) {
            // it's ok, we write snapshots every DISK_SNAPSHOT_PERIOD blocks only
            continue;
        }
        CDeterministicMNList mnList;
        mnList.Unserialize(snapshot_data, CDeterministicMN::MN_TYPE_FORMAT);
        batch.Write(std::make_pair(DB_LIST_SNAPSHOT, pindex->GetBlockHash()), mnList);
        m_evoDb.GetRawDB().WriteBatch(batch);
        batch.Clear();
        LogPrintf("CDeterministicMNManager::%s -- wrote snapshot at height %d\n", __func__, nHeight);
    }

    m_evoDb.GetRawDB().WriteBatch(batch);

    // Writing EVODB_BEST_BLOCK (which is b_b4 now) marks the DB as upgraded
    auto dbTx = m_evoDb.BeginTransaction();
    m_evoDb.WriteBestBlock(m_chainstate.m_chain.Tip()->GetBlockHash());
    dbTx->Commit();

    LogPrintf("CDeterministicMNManager::%s -- done migrating\n", __func__);

    if (EraseOldDBData(m_evoDb.GetRawDB(), {DB_OLD_LIST_DIFF, DB_OLD_LIST_SNAPSHOT})) {
        LogPrintf("CDeterministicMNManager::%s -- done cleaning old data\n", __func__);
    }

    m_evoDb.GetRawDB().CompactFull();

    LogPrintf("CDeterministicMNManager::%s -- done compacting database\n", __func__);

    // flush it to disk
    if (!m_evoDb.CommitRootTransaction()) {
        LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
        return false;
    }

    return true;
}

bool CDeterministicMNManager::MigrateDBIfNeeded3()
{
    static const std::string DB_OLD_LIST_SNAPSHOT = "dmn_S3";
    static const std::string DB_OLD_LIST_DIFF = "dmn_D3";
    static const std::string DB_OLD_BEST_BLOCK = "b_b4";

    LOCK(cs_main);

    LogPrintf("CDeterministicMNManager::%s -- upgrading DB to store compute service descriptors\n", __func__);

    if (m_chainstate.m_chain.Tip() == nullptr) {
        // should have no records
        LogPrintf("CDeterministicMNManager::%s -- Chain empty. evoDB:%d.\n", __func__, m_evoDb.IsEmpty());
        return m_evoDb.IsEmpty();
    }

    if (m_evoDb.GetRawDB().Exists(EVODB_BEST_BLOCK)) {
        if (EraseOldDBData(m_evoDb.GetRawDB(), {DB_OLD_LIST_DIFF, DB_OLD_LIST_SNAPSHOT})) {
            // we messed up, make sure this time we actually drop old data
            LogPrintf("CDeterministicMNManager::%s -- migration already done. cleaned old data.\n", __func__);
            m_evoDb.GetRawDB().CompactFull();
            LogPrintf("CDeterministicMNManager::%s -- done compacting database\n", __func__);
            // flush it to disk
            if (!m_evoDb.CommitRootTransaction()) {
                LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
                return false;
            }
        } else {
            LogPrintf("CDeterministicMNManager::%s -- migration already done. skipping.\n", __func__);
        }
        return true;
    }

    // Removing the old EVODB_BEST_BLOCK value early results in older version to crash immediately, even if the upgrade
    // process is cancelled in-between. But if the new version sees that the old EVODB_BEST_BLOCK is already removed,
    // then we must assume that the upgrade process was already running before but was interrupted.
    if (m_chainstate.m_chain.Height() > 1 && !m_evoDb.GetRawDB().Exists(DB_OLD_BEST_BLOCK)) {
        LogPrintf("CDeterministicMNManager::%s -- previous migration attempt failed.\n", __func__);
        return false;
    }
    m_evoDb.GetRawDB().Erase(DB_OLD_BEST_BLOCK);

    if (!DeploymentActiveAt(*m_chainstate.m_chain.Tip(), Params().GetConsensus(), Consensus::DEPLOYMENT_DIP0003)) {
        // not reached DIP3 height yet, so no upgrade needed
        LogPrintf("CDeterministicMNManager::%s -- migration not needed. dip3 not reached\n", __func__);
        auto dbTx = m_evoDb.BeginTransaction();
        m_evoDb.WriteBestBlock(m_chainstate.m_chain.Tip()->GetBlockHash());
        dbTx->Commit();
        if (!m_evoDb.CommitRootTransaction()) {
            LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
            return false;
        }
        return true;
    }

    CDBBatch batch(m_evoDb.GetRawDB());

    for (const auto nHeight : irange::range(Params().GetConsensus().DIP0003Height, m_chainstate.m_chain.Height() + 1)) {
        auto pindex = m_chainstate.m_chain[nHeight];
        // Re-serialize with the current format. The descriptor deserializes
        // to its default for every masternode: none could exist before this
        // format, so the rewrite is value-preserving by construction.
        CDataStream diff_data(SER_DISK, CLIENT_VERSION);
        if (!m_evoDb.GetRawDB().ReadDataStream(std::make_pair(DB_OLD_LIST_DIFF, pindex->GetBlockHash()), diff_data)) {
            LogPrintf("CDeterministicMNManager::%s -- missing CDeterministicMNListDiff at height %d\n", __func__, nHeight);
            return false;
        }
        CDeterministicMNListDiff mndiff;
        mndiff.Unserialize(diff_data, CDeterministicMN::MN_VERSION_FORMAT);
        batch.Write(std::make_pair(DB_LIST_DIFF, pindex->GetBlockHash()), mndiff);
        CDataStream snapshot_data(SER_DISK, CLIENT_VERSION);
        if (!m_evoDb.GetRawDB().ReadDataStream(std::make_pair(DB_OLD_LIST_SNAPSHOT, pindex->GetBlockHash()), snapshot_data)) {
            // it's ok, we write snapshots every DISK_SNAPSHOT_PERIOD blocks only
            continue;
        }
        CDeterministicMNList mnList;
        mnList.Unserialize(snapshot_data, CDeterministicMN::MN_VERSION_FORMAT);
        batch.Write(std::make_pair(DB_LIST_SNAPSHOT, pindex->GetBlockHash()), mnList);
        m_evoDb.GetRawDB().WriteBatch(batch);
        batch.Clear();
        LogPrintf("CDeterministicMNManager::%s -- wrote snapshot at height %d\n", __func__, nHeight);
    }

    m_evoDb.GetRawDB().WriteBatch(batch);

    // Writing EVODB_BEST_BLOCK (which is b_b5 now) marks the DB as upgraded
    auto dbTx = m_evoDb.BeginTransaction();
    m_evoDb.WriteBestBlock(m_chainstate.m_chain.Tip()->GetBlockHash());
    dbTx->Commit();

    LogPrintf("CDeterministicMNManager::%s -- done migrating\n", __func__);

    if (EraseOldDBData(m_evoDb.GetRawDB(), {DB_OLD_LIST_DIFF, DB_OLD_LIST_SNAPSHOT})) {
        LogPrintf("CDeterministicMNManager::%s -- done cleaning old data\n", __func__);
    }

    m_evoDb.GetRawDB().CompactFull();

    LogPrintf("CDeterministicMNManager::%s -- done compacting database\n", __func__);

    // flush it to disk
    if (!m_evoDb.CommitRootTransaction()) {
        LogPrintf("CDeterministicMNManager::%s -- failed to commit to evoDB\n", __func__);
        return false;
    }

    return true;
}

template <typename ProTx>
static bool CheckService(const ProTx& proTx, TxValidationState& state)
{
    if (!proTx.addr.IsValid()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-ipaddr");
    }
    if (Params().RequireRoutableExternalIP() && !proTx.addr.IsRoutable()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-ipaddr");
    }

    // TODO: use real args here
    static int mainnetDefaultPort = CreateChainParams(ArgsManager{}, CBaseChainParams::MAIN)->GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (proTx.addr.GetPort() != mainnetDefaultPort) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-ipaddr-port");
        }
    } else if (proTx.addr.GetPort() == mainnetDefaultPort) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-ipaddr-port");
    }

    if (!proTx.addr.IsIPv4()) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-ipaddr");
    }

    return true;
}

template <typename ProTx>
static bool CheckHashSig(const ProTx& proTx, const PKHash& pkhash, TxValidationState& state)
{
    if (std::string strError; !CHashSigner::VerifyHash(::SerializeHash(proTx), ToKeyID(pkhash), proTx.vchSig, strError)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-sig");
    }
    return true;
}

template <typename ProTx>
static bool CheckStringSig(const ProTx& proTx, const PKHash& pkhash, TxValidationState& state)
{
    if (std::string strError; !CMessageSigner::VerifyMessage(ToKeyID(pkhash), proTx.vchSig, proTx.MakeSignString(), strError)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-sig");
    }
    return true;
}

template <typename ProTx>
static bool CheckHashSig(const ProTx& proTx, const CBLSPublicKey& pubKey, TxValidationState& state)
{
    if (!proTx.sig.VerifyInsecure(pubKey, ::SerializeHash(proTx))) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-sig");
    }
    return true;
}

template<typename ProTx>
static std::optional<ProTx> GetValidatedPayload(const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, TxValidationState& state)
{
    if (tx.nType != ProTx::SPECIALTX_TYPE) {
        state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-type");
        return std::nullopt;
    }

    auto opt_ptx = GetTxPayload<ProTx>(tx);
    if (!opt_ptx) {
        state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-payload");
        return std::nullopt;
    }
    const bool is_basic_scheme_active{DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_V19)};
    if (!opt_ptx->IsTriviallyValid(is_basic_scheme_active, state)) {
        // pass the state returned by the function above
        return std::nullopt;
    }
    return opt_ptx;
}

bool CheckProRegTx(CDeterministicMNManager& dmnman, const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, TxValidationState& state, const CCoinsViewCache& view, bool check_sigs)
{
    const auto opt_ptx = GetValidatedPayload<CProRegTx>(tx, pindexPrev, state);
    if (!opt_ptx) {
        // pass the state returned by the function above
        return false;
    }

    // It's allowed to set addr to 0, which will put the MN into PoSe-banned state and require a ProUpServTx to be issues later
    // If any of both is set, it must be valid however
    if (opt_ptx->addr != CService() && !CheckService(*opt_ptx, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (opt_ptx->nType == MnType::Compute &&
        !IsComputeTypeActive(pindexPrev->nHeight + 1, Params().GetConsensus())) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-compute-early");
    }

    CTxDestination collateralTxDest;
    const PKHash *keyForPayloadSig = nullptr;
    COutPoint collateralOutpoint;

    CAmount expectedCollateral = GetMnType(opt_ptx->nType).collat_amount;

    if (!opt_ptx->collateralOutpoint.hash.IsNull()) {
        Coin coin;
        if (!view.GetCoin(opt_ptx->collateralOutpoint, coin) || coin.IsSpent() || coin.out.nValue != expectedCollateral) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral");
        }

        if (!ExtractDestination(coin.out.scriptPubKey, collateralTxDest)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral-dest");
        }

        // Extract key from collateral. This only works for P2PK and P2PKH collaterals and will fail for P2SH.
        // Issuer of this ProRegTx must prove ownership with this key by signing the ProRegTx
        keyForPayloadSig = std::get_if<PKHash>(&collateralTxDest);
        if (!keyForPayloadSig) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral-pkh");
        }

        collateralOutpoint = opt_ptx->collateralOutpoint;
    } else {
        if (opt_ptx->collateralOutpoint.n >= tx.vout.size()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral-index");
        }
        if (tx.vout[opt_ptx->collateralOutpoint.n].nValue != expectedCollateral) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral");
        }

        if (!ExtractDestination(tx.vout[opt_ptx->collateralOutpoint.n].scriptPubKey, collateralTxDest)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral-dest");
        }

        collateralOutpoint = COutPoint(tx.GetHash(), opt_ptx->collateralOutpoint.n);
    }

    // don't allow reuse of collateral key for other keys (don't allow people to put the collateral key onto an online server)
    // this check applies to internal and external collateral, but internal collaterals are not necessarily a P2PKH
    if (collateralTxDest == CTxDestination(PKHash(opt_ptx->keyIDOwner)) || collateralTxDest == CTxDestination(PKHash(opt_ptx->keyIDVoting))) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral-reuse");
    }

    if (pindexPrev) {
        auto mnList = dmnman.GetListForBlock(pindexPrev);

        // only allow reusing of addresses when it's for the same collateral (which replaces the old MN)
        if (mnList.HasUniqueProperty(opt_ptx->addr) && mnList.GetUniquePropertyMN(opt_ptx->addr)->collateralOutpoint != collateralOutpoint) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-dup-addr");
        }

        // never allow duplicate keys, even if this ProTx would replace an existing MN
        if (mnList.HasUniqueProperty(opt_ptx->keyIDOwner) || mnList.HasUniqueProperty(opt_ptx->pubKeyOperator)) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-dup-key");
        }

        // The check above only sees the operator key under the encoding this payload happens to use,
        // so it misses a key an existing masternode holds under the other one. A ProRegTx never
        // proves ownership of the operator key, so that gap lets anyone claim a masternode's key.
        // Nothing is excluded here: a duplicate key is never allowed, even for a ProTx replacing an
        // existing masternode.
        if (DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_V19) &&
            mnList.HasOperatorKeyUnderAnyScheme(opt_ptx->pubKeyOperator.Get(), /*self=*/uint256())) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-dup-key");
        }

        if (!DeploymentDIP0003Enforced(pindexPrev->nHeight, Params().GetConsensus())) {
            if (opt_ptx->keyIDOwner != opt_ptx->keyIDVoting) {
                return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-key-not-same");
            }
        }
    }

    if (!CheckInputsHash(tx, *opt_ptx, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (keyForPayloadSig) {
        // collateral is not part of this ProRegTx, so we must verify ownership of the collateral
        if (check_sigs && !CheckStringSig(*opt_ptx, *keyForPayloadSig, state)) {
            // pass the state returned by the function above
            return false;
        }
    } else {
        // collateral is part of this ProRegTx, so we know the collateral is owned by the issuer
        if (!opt_ptx->vchSig.empty()) {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-sig");
        }
    }

    return true;
}

bool CheckProUpServTx(CDeterministicMNManager& dmnman, const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, TxValidationState& state, bool check_sigs)
{
    const auto opt_ptx = GetValidatedPayload<CProUpServTx>(tx, pindexPrev, state);
    if (!opt_ptx) {
        // pass the state returned by the function above
        return false;
    }

    if (!CheckService(*opt_ptx, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (opt_ptx->nType == MnType::Compute &&
        !IsComputeTypeActive(pindexPrev->nHeight + 1, Params().GetConsensus())) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-compute-early");
    }

    auto mnList = dmnman.GetListForBlock(pindexPrev);
    auto mn = mnList.GetMN(opt_ptx->proTxHash);
    if (!mn) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-hash");
    }

    // Mirror BuildNewListFromBlock: nType must match the registered MN, or a
    // mempool-accepted ProUpServTx makes block assembly fail. An out-of-range
    // nType is already rejected by IsTriviallyValid, and dmn->nType is always
    // in range, so no separate IsValidMnType check is reachable here.
    // (dash#7488)
    if (opt_ptx->nType != mn->nType) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-type-mismatch");
    }

    // don't allow updating to addresses already used by other MNs
    if (mnList.HasUniqueProperty(opt_ptx->addr) && mnList.GetUniquePropertyMN(opt_ptx->addr)->proTxHash != opt_ptx->proTxHash) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-dup-addr");
    }

    if (opt_ptx->scriptOperatorPayout != CScript()) {
        if (mn->nOperatorReward == 0) {
            // don't allow setting operator reward payee in case no operatorReward was set
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-payee");
        }
        if (!opt_ptx->scriptOperatorPayout.IsPayToPublicKeyHash() && !opt_ptx->scriptOperatorPayout.IsPayToScriptHash()) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-operator-payee");
        }
    }

    // we can only check the signature if pindexPrev != nullptr and the MN is known
    if (!CheckInputsHash(tx, *opt_ptx, state)) {
        // pass the state returned by the function above
        return false;
    }
    if (check_sigs && !CheckHashSig(*opt_ptx, mn->pdmnState->pubKeyOperator.Get(), state)) {
        // pass the state returned by the function above
        return false;
    }

    return true;
}

bool CheckProUpRegTx(CDeterministicMNManager& dmnman, const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, TxValidationState& state, const CCoinsViewCache& view, bool check_sigs)
{
    const auto opt_ptx = GetValidatedPayload<CProUpRegTx>(tx, pindexPrev, state);
    if (!opt_ptx) {
        // pass the state returned by the function above
        return false;
    }

    CTxDestination payoutDest;
    if (!ExtractDestination(opt_ptx->scriptPayout, payoutDest)) {
        // should not happen as we checked script types before
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-dest");
    }

    auto mnList = dmnman.GetListForBlock(pindexPrev);
    auto dmn = mnList.GetMN(opt_ptx->proTxHash);
    if (!dmn) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-hash");
    }

    // don't allow reuse of payee key for other keys (don't allow people to put the payee key onto an online server)
    if (payoutDest == CTxDestination(PKHash(dmn->pdmnState->keyIDOwner)) || payoutDest == CTxDestination(PKHash(opt_ptx->keyIDVoting))) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-payee-reuse");
    }

    Coin coin;
    if (!view.GetCoin(dmn->collateralOutpoint, coin) || coin.IsSpent()) {
        // this should never happen (there would be no dmn otherwise)
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-collateral");
    }

    // don't allow reuse of collateral key for other keys (don't allow people to put the collateral key onto an online server)
    CTxDestination collateralTxDest;
    if (!ExtractDestination(coin.out.scriptPubKey, collateralTxDest)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-collateral-dest");
    }
    if (collateralTxDest == CTxDestination(PKHash(dmn->pdmnState->keyIDOwner)) || collateralTxDest == CTxDestination(PKHash(opt_ptx->keyIDVoting))) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-collateral-reuse");
    }

    if (mnList.HasUniqueProperty(opt_ptx->pubKeyOperator)) {
        auto otherDmn = mnList.GetUniquePropertyMN(opt_ptx->pubKeyOperator);
        if (opt_ptx->proTxHash != otherDmn->proTxHash) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-dup-key");
        }
    }

    // As above, but for the key under its other encoding, which the check above cannot see. Only
    // when the key is actually changing: an update that keeps its own key cannot create a new
    // duplicate, and probing it anyway would let a cross-scheme pair formed before activation
    // permanently block the affected masternode's registrar updates unless it rotated its key --
    // making an old squat more harmful rather than less.
    if (DeploymentActiveAfter(pindexPrev, Params().GetConsensus(), Consensus::DEPLOYMENT_V19) &&
        !(opt_ptx->pubKeyOperator == dmn->pdmnState->pubKeyOperator) &&
        mnList.HasOperatorKeyUnderAnyScheme(opt_ptx->pubKeyOperator.Get(), /*self=*/opt_ptx->proTxHash)) {
        return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-dup-key");
    }

    if (!DeploymentDIP0003Enforced(pindexPrev->nHeight, Params().GetConsensus())) {
        if (dmn->pdmnState->keyIDOwner != opt_ptx->keyIDVoting) {
            return state.Invalid(TxValidationResult::TX_BAD_SPECIAL, "bad-protx-key-not-same");
        }
    }

    if (!CheckInputsHash(tx, *opt_ptx, state)) {
        // pass the state returned by the function above
        return false;
    }
    if (check_sigs && !CheckHashSig(*opt_ptx, PKHash(dmn->pdmnState->keyIDOwner), state)) {
        // pass the state returned by the function above
        return false;
    }

    return true;
}

bool CheckProUpRevTx(CDeterministicMNManager& dmnman, const CTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev, TxValidationState& state, bool check_sigs)
{
    const auto opt_ptx = GetValidatedPayload<CProUpRevTx>(tx, pindexPrev, state);
    if (!opt_ptx) {
        // pass the state returned by the function above
        return false;
    }

    auto mnList = dmnman.GetListForBlock(pindexPrev);
    auto dmn = mnList.GetMN(opt_ptx->proTxHash);
    if (!dmn)
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-hash");

    if (!CheckInputsHash(tx, *opt_ptx, state)) {
        // pass the state returned by the function above
        return false;
    }
    if (check_sigs && !CheckHashSig(*opt_ptx, dmn->pdmnState->pubKeyOperator.Get(), state)) {
        // pass the state returned by the function above
        return false;
    }

    return true;
}
//end

void CDeterministicMNManager::DoMaintenance() {
    LOCK(cs_cleanup);
    int loc_to_cleanup = to_cleanup.load();
    if (loc_to_cleanup <= did_cleanup) return;
    LOCK(cs);
    CleanupCache(loc_to_cleanup);
    did_cleanup = loc_to_cleanup;
}

CDeterministicMNManager::RecalcDiffsResult CDeterministicMNManager::RecalculateAndRepairDiffs(
    const CBlockIndex* start_index, const CBlockIndex* stop_index, ChainstateManager& chainman,
    BuildListFromBlockFunc build_list_func, bool repair)
{
    RecalcDiffsResult result;
    result.start_height = start_index->nHeight;
    result.stop_height = stop_index->nHeight;

    const auto& consensus_params = Params().GetConsensus();

    // Clamp start height to DIP0003 activation (no snapshots/diffs exist before this)
    if (start_index->nHeight < consensus_params.DIP0003Height) {
        start_index = stop_index->GetAncestor(consensus_params.DIP0003Height);
        if (!start_index) {
            result.verification_errors.push_back(strprintf("Stop height %d is below DIP0003 activation height %d",
                                                           stop_index->nHeight, consensus_params.DIP0003Height));
            return result;
        }
        LogPrintf("CDeterministicMNManager::%s -- Clamped start height from %d to DIP0003 activation height %d\n",
                  __func__, result.start_height, consensus_params.DIP0003Height);
        // Update result to reflect the clamped start height
        result.start_height = start_index->nHeight;
    }

    // Collect all snapshot blocks in the range
    std::vector<const CBlockIndex*> snapshot_blocks = CollectSnapshotBlocks(start_index, stop_index, consensus_params);

    if (snapshot_blocks.empty()) {
        result.verification_errors.push_back("Could not find starting snapshot");
        return result;
    }

    if (snapshot_blocks.size() < 2) {
        result.verification_errors.push_back(strprintf("Need at least 2 snapshots, found %d", snapshot_blocks.size()));
        return result;
    }

    LogPrintf("CDeterministicMNManager::%s -- Processing %d snapshot pairs between heights %d and %d\n", __func__,
              snapshot_blocks.size() - 1, result.start_height, result.stop_height);

    // Storage for recalculated diffs if we plan to repair
    std::vector<std::pair<uint256, CDeterministicMNListDiff>> recalculated_diffs;

    // Process each pair of consecutive snapshots
    for (size_t i = 0; i < snapshot_blocks.size() - 1; ++i) {
        const CBlockIndex* from_index = snapshot_blocks[i];
        const CBlockIndex* to_index = snapshot_blocks[i + 1];

        // Load the snapshots from disk
        CDeterministicMNList from_snapshot;
        CDeterministicMNList to_snapshot;

        bool has_from_snapshot = m_evoDb.Read(std::make_pair(DB_LIST_SNAPSHOT, from_index->GetBlockHash()), from_snapshot);
        bool has_to_snapshot = m_evoDb.Read(std::make_pair(DB_LIST_SNAPSHOT, to_index->GetBlockHash()), to_snapshot);

        // Handle missing snapshots
        if (!has_from_snapshot) {
            // The initial snapshot at DIP0003 activation might not exist in the database on nodes
            // that synced before the fix to explicitly write it. This is the only acceptable case.
            if (from_index->nHeight == consensus_params.DIP0003Height) {
                // Create an empty initial snapshot (matching what GetListForBlockInternal does)
                from_snapshot = CDeterministicMNList(from_index->GetBlockHash(), from_index->nHeight, 0);
                LogPrintf("CDeterministicMNManager::%s -- Using empty initial snapshot at DIP0003 height %d\n",
                          __func__, from_index->nHeight);
            } else {
                // Any other missing snapshot is critical corruption beyond our repair capability
                result.verification_errors.push_back(strprintf("CRITICAL: Snapshot missing at height %d. "
                    "This cannot be repaired by this tool - full reindex required.", from_index->nHeight));
                return result;
            }
        }

        if (!has_to_snapshot) {
            // Missing target snapshot is always critical - we cannot repair snapshots, only diffs
            result.verification_errors.push_back(strprintf("CRITICAL: Snapshot missing at height %d. "
                "This cannot be repaired by this tool - full reindex required.", to_index->nHeight));
            return result;
        }

        // Log progress periodically (every 100 snapshot pairs) to avoid spam
        if (i % 100 == 0) {
            LogPrintf("CDeterministicMNManager::%s -- Progress: verifying snapshot pair %d/%d (heights %d-%d)\n",
                      __func__, i + 1, snapshot_blocks.size() - 1, from_index->nHeight, to_index->nHeight);
        }

        // Verify this snapshot pair
        bool is_snapshot_pair_valid = VerifySnapshotPair(from_index, to_index, from_snapshot, to_snapshot, result);

        // If repair mode is enabled and verification failed, recalculate diffs from blockchain
        if (repair && !is_snapshot_pair_valid) {
            auto temp_diffs = RepairSnapshotPair(from_index, to_index, from_snapshot, to_snapshot, build_list_func, result);
            if (temp_diffs.empty()) {
                // RepairSnapshotPair failed - this is a critical error, cannot continue
                return result;
            }
            // Only commit diffs if recalculation verification passed
            recalculated_diffs.insert(recalculated_diffs.end(), temp_diffs.begin(), temp_diffs.end());
            result.diffs_recalculated += temp_diffs.size();
        }
    }

    // The pair loop stops at the last on-disk snapshot; the stretch from
    // there to the tip has no snapshot to close a comparison against, and it
    // used to go entirely unchecked while stop_height claimed the tip. Walk
    // it anyway: an unreadable or inapplicable diff there is the same
    // corruption class a failing pair is, and verified_through_height reports
    // exactly how far the full comparison reached.
    result.verified_through_height = snapshot_blocks.back()->nHeight;
    if (const CBlockIndex* tail_start = snapshot_blocks.back(); tail_start->nHeight < stop_index->nHeight) {
        CDeterministicMNList tail_list;
        if (!m_evoDb.Read(std::make_pair(DB_LIST_SNAPSHOT, tail_start->GetBlockHash()), tail_list)) {
            result.verification_errors.push_back(
                strprintf("Failed to read snapshot at height %d for the tail check", tail_start->nHeight));
        } else {
            try {
                for (int nHeight = tail_start->nHeight + 1; nHeight <= stop_index->nHeight; ++nHeight) {
                    const CBlockIndex* pIndex = stop_index->GetAncestor(nHeight);
                    CDeterministicMNListDiff diff;
                    if (!pIndex || !m_evoDb.Read(std::make_pair(DB_LIST_DIFF, pIndex->GetBlockHash()), diff)) {
                        result.verification_errors.push_back(
                            strprintf("Failed to read diff at height %d past the last snapshot", nHeight));
                        break;
                    }
                    diff.nHeight = nHeight;
                    tail_list = tail_list.ApplyDiff(pIndex, diff);
                }
            } catch (const std::exception& e) {
                result.verification_errors.push_back(strprintf(
                    "Exception applying diffs past the last snapshot at height %d: %s", tail_start->nHeight, e.what()));
            }
        }
    }

    if (repair) {
        if (!WriteRepairedDiffs(recalculated_diffs, result)) {
            return result;
        }
        if (result.diffs_recalculated > 0 && result.repair_errors.empty()) {
            // Prove the rewrite. The old flow reported the pre-repair
            // verification failures upward even when the repair succeeded, so
            // a successful repair was never marked complete and every restart
            // re-ran it. What the caller gets now is the verified state of
            // the database after the repair, pairs and tail alike.
            LogPrintf("CDeterministicMNManager::%s -- Re-verifying after repair...\n", __func__);
            RecalcDiffsResult recheck =
                RecalculateAndRepairDiffs(start_index, stop_index, chainman, build_list_func, /*repair=*/false);
            recheck.diffs_recalculated = result.diffs_recalculated;
            recheck.repair_errors = result.repair_errors;
            return recheck;
        }
    }

    return result;
}

bool CDeterministicMNManager::IsRepaired() const { return m_evoDb.Exists(DB_LIST_REPAIRED); }

void CDeterministicMNManager::CompleteRepair()
{
    auto dbTx = m_evoDb.BeginTransaction();
    m_evoDb.Write(DB_LIST_REPAIRED, 1);
    dbTx->Commit();
    // flush it to disk
    if (!m_evoDb.CommitRootTransaction()) {
        LogPrintf("CDeterministicMNManager::%s -- Failed to commit to evoDB\n", __func__);
        assert(false);
    }
}

std::vector<const CBlockIndex*> CDeterministicMNManager::CollectSnapshotBlocks(
    const CBlockIndex* start_index, const CBlockIndex* stop_index, const Consensus::Params& consensus_params)
{
    std::vector<const CBlockIndex*> snapshot_blocks;

    // Add the starting snapshot (find the snapshot at or before start)
    // Walk backwards to find a snapshot block (divisible by DISK_SNAPSHOT_PERIOD)
    // or the initial snapshot at DIP0003 activation height
    const CBlockIndex* snapshot_start_index = start_index;
    while (snapshot_start_index && snapshot_start_index->nHeight > consensus_params.DIP0003Height &&
           (snapshot_start_index->nHeight % DISK_SNAPSHOT_PERIOD) != 0) {
        snapshot_start_index = snapshot_start_index->pprev;
    }

    if (!snapshot_start_index) {
        return snapshot_blocks; // Empty vector indicates error
    }

    // Collect all snapshot blocks up to and including the stop block
    snapshot_blocks.push_back(snapshot_start_index);

    // Find all subsequent snapshot heights
    int current_snapshot_height = snapshot_start_index->nHeight;
    while (true) {
        // Calculate next snapshot height
        int next_snapshot_height;
        if (current_snapshot_height == consensus_params.DIP0003Height) {
            // If we're at DIP0003 activation (initial snapshot), next is at first regular interval
            next_snapshot_height = ((consensus_params.DIP0003Height / DISK_SNAPSHOT_PERIOD) + 1) * DISK_SNAPSHOT_PERIOD;
        } else {
            // Otherwise, add DISK_SNAPSHOT_PERIOD
            next_snapshot_height = current_snapshot_height + DISK_SNAPSHOT_PERIOD;
        }

        if (next_snapshot_height > stop_index->nHeight) {
            break;
        }

        const CBlockIndex* next_snapshot_index = stop_index->GetAncestor(next_snapshot_height);
        if (!next_snapshot_index) {
            break;
        }

        snapshot_blocks.push_back(next_snapshot_index);
        current_snapshot_height = next_snapshot_height;
    }

    return snapshot_blocks;
}

bool CDeterministicMNManager::VerifySnapshotPair(
    const CBlockIndex* from_index, const CBlockIndex* to_index, const CDeterministicMNList& from_snapshot,
    const CDeterministicMNList& to_snapshot, RecalcDiffsResult& result)
{
    // Verify this snapshot pair by applying all stored diffs sequentially
    CDeterministicMNList test_list = from_snapshot;

    try {
        for (int nHeight = from_index->nHeight + 1; nHeight <= to_index->nHeight; ++nHeight) {
            const CBlockIndex* pIndex = to_index->GetAncestor(nHeight);
            if (!pIndex) {
                result.verification_errors.push_back(strprintf("Failed to get ancestor at height %d", nHeight));
                return false;
            }

            CDeterministicMNListDiff diff;
            if (!m_evoDb.Read(std::make_pair(DB_LIST_DIFF, pIndex->GetBlockHash()), diff)) {
                result.verification_errors.push_back(strprintf("Failed to read diff at height %d", nHeight));
                return false;
            }

            diff.nHeight = nHeight;
            test_list = test_list.ApplyDiff(pIndex, diff);
        }
    } catch (const std::exception& e) {
        result.verification_errors.push_back(strprintf("Exception during verification: %s", e.what()));
        return false;
    }

    // Verify that applying all diffs results in the target snapshot
    bool is_snapshot_pair_valid = test_list.IsEqual(to_snapshot);

    if (is_snapshot_pair_valid) {
        result.snapshots_verified++;
    } else {
        result.verification_errors.push_back(
            strprintf("Verification failed between snapshots at heights %d and %d: "
                      "Applied diffs do not match target snapshot",
                      from_index->nHeight, to_index->nHeight));
    }

    return is_snapshot_pair_valid;
}

std::vector<std::pair<uint256, CDeterministicMNListDiff>> CDeterministicMNManager::RepairSnapshotPair(
    const CBlockIndex* from_index, const CBlockIndex* to_index, const CDeterministicMNList& from_snapshot,
    const CDeterministicMNList& to_snapshot, BuildListFromBlockFunc build_list_func, RecalcDiffsResult& result)
{
    CDeterministicMNList current_list = from_snapshot;
    // Temporary storage for recalculated diffs (one per block in this snapshot interval)
    std::vector<std::pair<uint256, CDeterministicMNListDiff>> temp_diffs;
    temp_diffs.reserve(to_index->nHeight - from_index->nHeight);

    LogPrintf("CDeterministicMNManager::%s -- Repairing: recalculating diffs between snapshots at heights %d and %d\n",
              __func__, from_index->nHeight, to_index->nHeight);

    try {
        for (int nHeight = from_index->nHeight + 1; nHeight <= to_index->nHeight; ++nHeight) {
            const CBlockIndex* pIndex = to_index->GetAncestor(nHeight);

            // Read the actual block from disk
            CBlock block;
            if (!ReadBlockFromDisk(block, pIndex, Params().GetConsensus())) {
                result.repair_errors.push_back(strprintf("CRITICAL: Failed to read block at height %d. "
                    "Cannot repair - full reindex required.", nHeight));
                return {}; // Critical error - cannot continue repair
            }

            // Use a dummy coins view to avoid UTXO lookups. At chain tip, coins from
            // historical blocks may already be spent. Since these blocks were fully
            // validated when originally connected, we don't need to re-verify coin
            // availability - we only need to extract special transactions.
            CCoinsView view_dummy;
            CCoinsViewCache view(&view_dummy);

            // Build the new list by processing this block's special transactions
            // Starting from current_list (our trusted state), not from corrupted diffs
            CDeterministicMNList next_list;
            BlockValidationState state;
            if (!build_list_func(block, pIndex->pprev, current_list, view, false, state, next_list)) {
                result.repair_errors.push_back(
                    strprintf("CRITICAL: Failed to build list for block at height %d: %s. "
                              "Cannot repair - full reindex required.", nHeight, state.ToString()));
                return {}; // Critical error - cannot continue repair
            }

            // Set the correct block hash
            next_list.SetBlockHash(pIndex->GetBlockHash());

            // Calculate the diff between current and next
            CDeterministicMNListDiff recalc_diff = current_list.BuildDiff(next_list);
            recalc_diff.nHeight = nHeight;
            // Store in temporary vector for this snapshot pair
            temp_diffs.emplace_back(pIndex->GetBlockHash(), recalc_diff);

            // Move forward
            current_list = std::move(next_list);
        }

        // Replay what is about to be written, rather than trusting the list it was
        // built from.
        //
        // The rebuilt list is not evidence about the diffs. BuildDiff records
        // internalIds and IsEqual deliberately ignores them -- they differ
        // legitimately between nodes -- so a rebuild that assigned different ones
        // satisfies IsEqual while producing diffs ApplyDiff cannot use, and the pair
        // then fails verification for ever with "can't find an updated masternode".
        // Observed on a live devnet: all three snapshot pairs reported a successful
        // recalculation, 1304 diffs were written, and verification returned the
        // identical three errors afterwards from a fresh process reading disk.
        //
        // A repair may only claim success on the round trip a later verification
        // will actually perform.
        CDeterministicMNList replay = from_snapshot;
        for (size_t i = 0; i < temp_diffs.size(); ++i) {
            const CBlockIndex* pReplayIndex = to_index->GetAncestor(from_index->nHeight + 1 + int(i));
            if (!pReplayIndex) {
                result.repair_errors.push_back(
                    strprintf("CRITICAL: Failed to get ancestor at height %d while replaying repaired "
                              "diffs. Cannot repair - full reindex required.",
                              from_index->nHeight + 1 + int(i)));
                return {};
            }
            CDeterministicMNListDiff replay_diff = temp_diffs[i].second;
            replay_diff.nHeight = pReplayIndex->nHeight;
            replay = replay.ApplyDiff(pReplayIndex, replay_diff);
        }

        if (replay.IsEqual(to_snapshot)) {
            LogPrintf("CDeterministicMNManager::%s -- Successfully recalculated %d diffs between heights %d and %d\n",
                      __func__, temp_diffs.size(), from_index->nHeight, to_index->nHeight);
            return temp_diffs; // Success - return recalculated diffs
        } else {
            result.repair_errors.push_back(
                strprintf("CRITICAL: Recalculation failed between snapshots at heights %d and %d: "
                          "Applied diffs do not match target snapshot. Cannot repair - full reindex required.",
                          from_index->nHeight, to_index->nHeight));
            return {}; // Failed verification - return empty vector
        }
    } catch (const std::exception& e) {
        result.repair_errors.push_back(strprintf("CRITICAL: Exception during recalculation: %s. "
                                                  "Cannot repair - full reindex required.", e.what()));
        return {}; // Exception - return empty vector
    }
}

bool CDeterministicMNManager::WriteRepairedDiffs(
    const std::vector<std::pair<uint256, CDeterministicMNListDiff>>& recalculated_diffs, RecalcDiffsResult& result)
{
    AssertLockNotHeld(cs);

    if (recalculated_diffs.empty()) {
        return true;
    }

    CDBBatch batch(m_evoDb.GetRawDB());
    const size_t BATCH_SIZE_THRESHOLD = 1 << 24; // 16MB
    size_t diffs_written = 0;

    LogPrintf("CDeterministicMNManager::%s -- Writing %d repaired diffs to database...\n",
              __func__, recalculated_diffs.size());

    for (const auto& [block_hash, diff] : recalculated_diffs) {
        batch.Write(std::make_pair(DB_LIST_DIFF, block_hash), diff);
        diffs_written++;

        // Write batch when it gets too large
        if (batch.SizeEstimate() >= BATCH_SIZE_THRESHOLD) {
            LogPrintf("CDeterministicMNManager::%s -- Flushing batch (%d diffs written so far)...\n",
                      __func__, diffs_written);
            if (!m_evoDb.GetRawDB().WriteBatch(batch)) {
                result.repair_errors.push_back(strprintf(
                    "Failed to write repaired diffs to the database (%d queued before the failure)", diffs_written));
                return false;
            }
            batch.Clear();
        }
    }

    // Write any remaining diffs in the batch
    if (batch.SizeEstimate() > 0) {
        LogPrintf("CDeterministicMNManager::%s -- Writing final batch...\n", __func__);
        if (!m_evoDb.GetRawDB().WriteBatch(batch)) {
            result.repair_errors.push_back(strprintf(
                "Failed to write repaired diffs to the database (%d queued before the failure)", diffs_written));
            return false;
        }
        batch.Clear();
    }

    // Clear caches for repaired diffs so next read gets fresh data from disk
    // Must clear both diff cache and list cache since lists were built from old diffs
    LOCK(cs);
    for (const auto& [block_hash, diff] : recalculated_diffs) {
        mnListDiffsCache.erase(block_hash);
        mnListsCache.erase(block_hash);
    }

    LogPrintf("CDeterministicMNManager::%s -- Successfully repaired %d diffs (caches cleared)\n", __func__,
              recalculated_diffs.size());
    return true;
}
