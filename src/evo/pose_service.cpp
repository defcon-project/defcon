// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <llmq/commitment.h>
#include <llmq/options.h>
#include <llmq/quorums.h>
#include <llmq/signing.h>
#include <node/blockstorage.h>
#include <primitives/transaction.h>
#include <sync.h>
#include <validation.h>

uint256 CPoSeServiceCommitment::GetRequestId() const
{
    return ::SerializeHash(std::make_pair(std::string{"dslcommitment"}, nEpoch));
}

bool CPoSeServiceCommitment::Verify(const llmq::CQuorumManager& qman, const uint256& msgHash,
                                    TxValidationState& state) const
{
    const auto quorum = qman.GetQuorum(llmqType, quorumHash);
    if (!quorum) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-missing-quorum");
    }

    const uint256 signHash = llmq::BuildSignHash(llmqType, quorum->qc->quorumHash, GetRequestId(), msgHash);
    if (!quorumSig.VerifyInsecure(quorum->qc->quorumPublicKey, signHash)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-invalid-sig");
    }
    return true;
}

// CPoSeServiceCommitment::ToJson and CPoSeServiceCommitmentTxPayload::ToJson
// are defined inline in the header, so core_write.cpp (libbitcoin_common) can
// resolve them without pulling in the server library -- the pattern every
// other special-transaction payload follows.

bool CheckPoSeServiceCommitmentTx(const ChainstateManager& chainman, const llmq::CQuorumManager& qman,
                                  const CTransaction& tx, const CBlockIndex* pindexPrev,
                                  TxValidationState& state)
{
    if (!tx.IsSpecialTxVersion() || tx.nType != TRANSACTION_POSE_SERVICE_COMMITMENT) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-type");
    }

    const auto opt_payload = GetTxPayload<CPoSeServiceCommitmentTxPayload>(tx);
    if (!opt_payload) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-payload");
    }
    const auto& payload = *opt_payload;
    if (payload.nVersion == 0 || payload.nVersion > CPoSeServiceCommitmentTxPayload::CURRENT_VERSION) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-version");
    }
    const auto& c = payload.commitment;
    if (c.nVersion == 0 || c.nVersion > CPoSeServiceCommitment::CURRENT_VERSION) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-commitment-version");
    }

    const auto& consensus = Params().GetConsensus();
    const int height = pindexPrev->nHeight + 1;

    // The type ships dormant, and the first commitment needs one whole epoch
    // observed after activation before it can close.
    if (consensus.nDSLEpochInterval <= 0 ||
        height < consensus.nDSLActivationHeight ||
        height - consensus.nDSLEpochInterval < consensus.nDSLActivationHeight) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-commitment-early");
    }

    // Exactly one commitment per epoch, only at an epoch-boundary height, and
    // it closes the observation epoch that ended at this boundary.
    if (height % consensus.nDSLEpochInterval != 0) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-not-epoch-boundary");
    }
    if (c.nEpoch != static_cast<uint32_t>(height / consensus.nDSLEpochInterval) - 1) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-epoch");
    }
    // The epoch base is the first block of the observed epoch: the block the
    // sentinel selection, the challenge nonces and the canonical bit order were
    // all keyed on while the epoch was being watched. Binding to it stops
    // replay and pins the list the bitfield indexes.
    const CBlockIndex* pindexEpochBase = pindexPrev->GetAncestor(height - consensus.nDSLEpochInterval);
    if (pindexEpochBase == nullptr || c.epochBlockHash != pindexEpochBase->GetBlockHash()) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-epoch-hash");
    }
    // The attesting quorum is the ChainLock quorum resolved at this height.
    if (c.llmqType != llmq::GetChainLocksLLMQType(consensus, height)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-quorum-type");
    }

    const CBlockIndex* pindexQuorum = WITH_LOCK(::cs_main,
        return chainman.m_blockman.LookupBlockIndex(c.quorumHash));
    if (!pindexQuorum || pindexQuorum != pindexPrev->GetAncestor(pindexQuorum->nHeight)) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-dsl-quorum-hash");
    }

    // Message the quorum signed: the transaction with the signature zeroed.
    CMutableTransaction tx_copy(tx);
    auto payload_copy = payload;
    payload_copy.commitment.quorumSig = CBLSSignature();
    SetTxPayload(tx_copy, payload_copy);
    const uint256 msgHash = tx_copy.GetHash();

    return c.Verify(qman, msgHash, state);
}
