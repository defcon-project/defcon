// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <bls/bls.h>
#include <chainparams.h>
#include <llmq/context.h>
#include <llmq/instantsend.h>
#include <llmq/signing.h>
#include <net.h>
#include <net_processing.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

namespace llmq {
// Access the post-BLS verification seam without constructing a signing quorum.
struct InstantSendQueueTest {
    static CInstantSendLockPtr MakeLock(uint64_t id)
    {
        auto lock = std::make_shared<CInstantSendLock>();
        lock->txid = ArithToUint256(arith_uint256{id});
        lock->inputs.emplace_back(lock->txid, 0);
        lock->cycleHash = Params().GenesisBlock().GetHash();
        return lock;
    }

    static void Seed(CInstantSendManager& manager, bool no_tx, size_t count)
    {
        LOCK(manager.cs_pendingLocks);
        auto& queue = no_tx ? manager.pendingNoTxInstantSendLocks : manager.pendingInstantSendLocks;
        for (size_t i = 0; i < count; ++i) {
            auto lock = MakeLock(i + 1);
            queue.emplace(lock->txid, std::make_pair(NodeId{1}, lock));
        }
    }

    static void Retry(CInstantSendManager& manager, const CTransactionRef& tx)
    {
        manager.AddNonLockedTx(tx, nullptr);
    }

    static bool HasPending(const CInstantSendManager& manager, const uint256& hash)
    {
        LOCK(manager.cs_pendingLocks);
        return manager.pendingInstantSendLocks.count(hash) != 0;
    }

    static size_t Size(const CInstantSendManager& manager, bool no_tx)
    {
        LOCK(manager.cs_pendingLocks);
        return no_tx ? manager.pendingNoTxInstantSendLocks.size() : manager.pendingInstantSendLocks.size();
    }

    static PeerMsgRet Receive(CInstantSendManager& manager, const CNode& peer, PeerManager& peerman,
                              const CInstantSendLockPtr& lock)
    {
        return manager.ProcessMessageInstantSendLock(peer, peerman, lock);
    }

    static uint256 RecoverOwn(CInstantSendManager& manager, const CInstantSendLockPtr& lock)
    {
        const auto id = lock->GetRequestId();
        {
            LOCK(manager.cs_creating);
            const auto [it, inserted] = manager.creatingInstantSendLocks.emplace(id, *lock);
            BOOST_REQUIRE(inserted);
            manager.txToCreatingInstantSendLocks.emplace(lock->txid, &it->second);
        }
        const CRecoveredSig recovered{manager.GetLLMQType(), lock->cycleHash, id, lock->txid, lock->sig};
        manager.HandleNewInstantSendLockRecoveredSig(recovered);
        {
            LOCK(manager.cs_creating);
            BOOST_CHECK_EQUAL(manager.creatingInstantSendLocks.count(id), 0U);
            BOOST_CHECK_EQUAL(manager.txToCreatingInstantSendLocks.count(lock->txid), 0U);
        }
        return SerializeHash(*lock);
    }

    static void SeedDuplicate(CInstantSendManager& manager, const CTransactionRef& tx)
    {
        auto lock = MakeLock(2001);
        lock->txid = tx->GetHash();
        const auto hash = SerializeHash(*lock);
        LOCK(manager.cs_pendingLocks);
        manager.pendingInstantSendLocks.emplace(hash, std::make_pair(NodeId{1}, lock));
        manager.pendingNoTxInstantSendLocks.emplace(hash, std::make_pair(NodeId{1}, lock));
    }

    static void Verified(CInstantSendManager& manager, PeerManager& peerman, const CInstantSendLockPtr& lock)
    {
        manager.ProcessInstantSendLock(NodeId{1}, peerman, SerializeHash(*lock), lock);
    }

    //! Run locks from one source through the batch verification (the current active set, no penalty), as the worker does.
    //! `first`, if given, receives the hash of the lock the batch visits first.
    static std::unordered_set<uint256, StaticSaltedHasher> VerifyBatch(CInstantSendManager& manager, PeerManager& peerman,
                                                                       const std::vector<CInstantSendLockPtr>& locks,
                                                                       uint256* first = nullptr)
    {
        std::unordered_map<uint256, std::pair<NodeId, CInstantSendLockPtr>, StaticSaltedHasher> pend;
        for (const auto& lock : locks) {
            pend.emplace(SerializeHash(*lock), std::make_pair(NodeId{1}, lock));
        }
        if (first != nullptr) *first = pend.begin()->first;
        const auto llmq_params = Params().GetLLMQ(manager.GetLLMQType());
        BOOST_REQUIRE(llmq_params);
        return manager.ProcessPendingInstantSendLocks(*llmq_params, peerman, /*signOffset=*/0, pend, /*ban=*/false);
    }

    //! Queue a lock from peer 1 in the received queue, as ProcessMessageInstantSendLock does.
    static void Queue(CInstantSendManager& manager, const CInstantSendLockPtr& lock)
    {
        LOCK(manager.cs_pendingLocks);
        manager.pendingInstantSendLocks.emplace(SerializeHash(*lock), std::make_pair(NodeId{1}, lock));
    }

    //! One step of the worker: take a batch from the received queue and verify and process it.
    static bool WorkerStep(CInstantSendManager& manager, PeerManager& peerman)
    {
        return manager.ProcessPendingInstantSendLocks(peerman);
    }

    static bool HasNoTx(const CInstantSendManager& manager, const uint256& hash)
    {
        LOCK(manager.cs_pendingLocks);
        return manager.pendingNoTxInstantSendLocks.count(hash) != 0;
    }

    //! Store a recovered sig for the lock's request id and txid. The signing manager stores only verified ones
    //! (ProcessRecoveredSig is called after verification); here it is written directly.
    static void CoverWithRecoveredSig(const CInstantSendManager& manager, CSigningManager& sigman, PeerManager& peerman,
                                      const CInstantSendLockPtr& lock)
    {
        const auto llmq_type = manager.GetLLMQType();
        sigman.ProcessRecoveredSig(std::make_shared<const CRecoveredSig>(llmq_type, lock->cycleHash, lock->GetRequestId(),
                                                                         lock->txid, lock->sig),
                                   peerman);
        BOOST_REQUIRE(sigman.HasRecoveredSig(llmq_type, lock->GetRequestId(), lock->txid));
    }
};
} // namespace llmq

BOOST_FIXTURE_TEST_SUITE(instantsend_queue_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(received_queue_exact_and_over_limit)
{
    auto& manager = *m_node.llmq_ctx->isman;
    llmq::InstantSendQueueTest::Seed(manager, false, 1023);
    CNode peer{1, nullptr, CAddress{CService{}, NODE_NONE}, 0, 0, CAddress{}, std::string{},
               ConnectionType::INBOUND, false};
    BOOST_CHECK(llmq::InstantSendQueueTest::Receive(manager, peer, *m_node.peerman,
                                                   llmq::InstantSendQueueTest::MakeLock(2001)));
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, false), 1024U);
    BOOST_CHECK(llmq::InstantSendQueueTest::Receive(manager, peer, *m_node.peerman,
                                                   llmq::InstantSendQueueTest::MakeLock(2002)));
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, false), 1024U);
}

BOOST_AUTO_TEST_CASE(verified_no_tx_queue_exact_and_over_limit)
{
    auto& manager = *m_node.llmq_ctx->isman;
    llmq::InstantSendQueueTest::Seed(manager, true, 1023);
    llmq::InstantSendQueueTest::Verified(manager, *m_node.peerman, llmq::InstantSendQueueTest::MakeLock(2001));
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, true), 1024U);
    llmq::InstantSendQueueTest::Verified(manager, *m_node.peerman, llmq::InstantSendQueueTest::MakeLock(2002));
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, true), 1024U);
}

BOOST_AUTO_TEST_CASE(verified_lock_moves_to_full_received_queue)
{
    auto& manager = *m_node.llmq_ctx->isman;
    llmq::InstantSendQueueTest::Seed(manager, false, 1024);
    CMutableTransaction mutable_tx;
    mutable_tx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    mutable_tx.vout.emplace_back(1, CScript{});
    const auto tx = MakeTransactionRef(mutable_tx);
    auto lock = llmq::InstantSendQueueTest::MakeLock(2001);
    lock->txid = tx->GetHash();
    llmq::InstantSendQueueTest::Verified(manager, *m_node.peerman, lock);
    BOOST_REQUIRE_EQUAL(llmq::InstantSendQueueTest::Size(manager, true), 1U);
    llmq::InstantSendQueueTest::Retry(manager, tx);
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, false), 1024U);
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, true), 0U);
    BOOST_CHECK(llmq::InstantSendQueueTest::HasPending(manager, SerializeHash(*lock)));
}

// A recovered signature has already been verified. Retain the node's own
// lock even when peer-supplied pending verifications fill the queue.
BOOST_AUTO_TEST_CASE(own_recovered_lock_displaces_pending_at_capacity)
{
    auto& manager = *m_node.llmq_ctx->isman;
    llmq::InstantSendQueueTest::Seed(manager, false, 1024);
    const auto hash = llmq::InstantSendQueueTest::RecoverOwn(manager, llmq::InstantSendQueueTest::MakeLock(2001));
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, false), 1024U);
    BOOST_CHECK(llmq::InstantSendQueueTest::HasPending(manager, hash));
}

// Re-encountering a lock already in both maps must not evict a different
// pending verification when the no-tx entry moves back for processing.
BOOST_AUTO_TEST_CASE(duplicate_verified_lock_preserves_full_received_queue)
{
    auto& manager = *m_node.llmq_ctx->isman;
    CMutableTransaction mutable_tx;
    mutable_tx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    mutable_tx.vout.emplace_back(1, CScript{});
    const auto tx = MakeTransactionRef(mutable_tx);
    llmq::InstantSendQueueTest::SeedDuplicate(manager, tx);
    llmq::InstantSendQueueTest::Seed(manager, false, 1023);
    llmq::InstantSendQueueTest::Retry(manager, tx);
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, false), 1024U);
    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, true), 0U);
}

// A lock the batch never hands to the verifier must not be processed. Their transactions are unknown here, so a
// processed lock would wait in the no-tx queue.
BOOST_AUTO_TEST_CASE(unverified_locks_are_not_processed)
{
    auto& manager = *m_node.llmq_ctx->isman;
    const bool legacy{bls::bls_legacy_scheme.load()};

    // An unreadable signature: the default object is invalid, as all-zero bytes off the wire are.
    const auto unreadable = llmq::InstantSendQueueTest::MakeLock(3001);
    BOOST_REQUIRE(!unreadable->sig.Get().IsValid());
    const auto unreadable_hash = SerializeHash(*unreadable);
    BOOST_CHECK(llmq::InstantSendQueueTest::VerifyBatch(manager, *m_node.peerman, {unreadable}).empty());
    BOOST_CHECK(!llmq::InstantSendQueueTest::HasNoTx(manager, unreadable_hash));
    BOOST_CHECK(!llmq::InstantSendQueueTest::HasPending(manager, unreadable_hash));

    // A readable signature naming a cycle block this node does not have.
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const auto unknown_cycle = llmq::InstantSendQueueTest::MakeLock(3002);
    unknown_cycle->sig.Set(sk.Sign(uint256::ONE, legacy), legacy);
    unknown_cycle->cycleHash = uint256::ONE;
    BOOST_REQUIRE(unknown_cycle->sig.Get().IsValid());
    const auto unknown_cycle_hash = SerializeHash(*unknown_cycle);
    BOOST_CHECK(llmq::InstantSendQueueTest::VerifyBatch(manager, *m_node.peerman, {unknown_cycle}).empty());
    BOOST_CHECK(!llmq::InstantSendQueueTest::HasNoTx(manager, unknown_cycle_hash));
    BOOST_CHECK(!llmq::InstantSendQueueTest::HasPending(manager, unknown_cycle_hash));

    // Both kinds from one source in one batch: neither is processed, whichever comes first.
    const auto pair_unreadable = llmq::InstantSendQueueTest::MakeLock(3003);
    const auto pair_unknown_cycle = llmq::InstantSendQueueTest::MakeLock(3004);
    pair_unknown_cycle->sig.Set(sk.Sign(uint256::ONE, legacy), legacy);
    pair_unknown_cycle->cycleHash = uint256::ONE;
    BOOST_CHECK(llmq::InstantSendQueueTest::VerifyBatch(manager, *m_node.peerman, {pair_unreadable, pair_unknown_cycle}).empty());
    BOOST_CHECK(!llmq::InstantSendQueueTest::HasNoTx(manager, SerializeHash(*pair_unreadable)));
    BOOST_CHECK(!llmq::InstantSendQueueTest::HasNoTx(manager, SerializeHash(*pair_unknown_cycle)));

    BOOST_CHECK_EQUAL(llmq::InstantSendQueueTest::Size(manager, true), 0U);
}

// A lock whose request id and txid an already verified recovered sig covers is processed without a second check.
// A masternode's own lock takes this path through the same batch, so dropping it here would lose that lock.
BOOST_AUTO_TEST_CASE(covered_lock_is_still_processed)
{
    auto& manager = *m_node.llmq_ctx->isman;
    const bool legacy{bls::bls_legacy_scheme.load()};
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const auto lock = llmq::InstantSendQueueTest::MakeLock(3005);
    lock->sig.Set(sk.Sign(uint256::ONE, legacy), legacy);
    llmq::InstantSendQueueTest::CoverWithRecoveredSig(manager, *m_node.llmq_ctx->sigman, *m_node.peerman, lock);
    BOOST_CHECK(llmq::InstantSendQueueTest::VerifyBatch(manager, *m_node.peerman, {lock}).empty());
    BOOST_CHECK(llmq::InstantSendQueueTest::HasNoTx(manager, SerializeHash(*lock)));
}

// A lock verified before comes back from the no-tx queue under the peer it came from, so it can share a batch with an
// unreadable lock from that peer. It must still be processed, in either order; the unreadable one must not. With no
// quorum here, a covering recovered sig stands in for the earlier verification. The batch is a hash map, so the order
// it is visited in is not the order of insertion; both insertion orders are tried, on fresh pairs, until the batch
// has been seen visiting each kind first.
BOOST_AUTO_TEST_CASE(checked_lock_survives_bad_lock_from_same_source)
{
    using llmq::InstantSendQueueTest;
    auto& manager = *m_node.llmq_ctx->isman;
    const bool legacy{bls::bls_legacy_scheme.load()};
    CBLSSecretKey sk;
    sk.MakeNewKey();
    bool seen_good_first{false};
    bool seen_bad_first{false};
    for (uint64_t i = 0; i < 64 && !(seen_good_first && seen_bad_first); ++i) {
        const auto good = InstantSendQueueTest::MakeLock(4000 + 2 * i);
        good->sig.Set(sk.Sign(uint256::ONE, legacy), legacy);
        InstantSendQueueTest::CoverWithRecoveredSig(manager, *m_node.llmq_ctx->sigman, *m_node.peerman, good);
        const auto bad = InstantSendQueueTest::MakeLock(4001 + 2 * i);
        const std::vector<llmq::CInstantSendLockPtr> locks = (i % 2 == 0) ? std::vector{good, bad} : std::vector{bad, good};
        uint256 first;
        BOOST_CHECK(InstantSendQueueTest::VerifyBatch(manager, *m_node.peerman, locks, &first).empty());
        (first == SerializeHash(*good) ? seen_good_first : seen_bad_first) = true;
        BOOST_CHECK(InstantSendQueueTest::HasNoTx(manager, SerializeHash(*good)));
        BOOST_CHECK(!InstantSendQueueTest::HasNoTx(manager, SerializeHash(*bad)));
    }
    BOOST_CHECK(seen_good_first);
    BOOST_CHECK(seen_bad_first);
}

// The worker's own step takes the batch from the received queue and processes only the checked lock of it.
BOOST_AUTO_TEST_CASE(worker_step_processes_only_checked_locks)
{
    using llmq::InstantSendQueueTest;
    auto& manager = *m_node.llmq_ctx->isman;
    const bool legacy{bls::bls_legacy_scheme.load()};
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const auto good = InstantSendQueueTest::MakeLock(5001);
    good->sig.Set(sk.Sign(uint256::ONE, legacy), legacy);
    InstantSendQueueTest::CoverWithRecoveredSig(manager, *m_node.llmq_ctx->sigman, *m_node.peerman, good);
    const auto unreadable = InstantSendQueueTest::MakeLock(5002);
    const auto unknown_cycle = InstantSendQueueTest::MakeLock(5003);
    unknown_cycle->sig.Set(sk.Sign(uint256::ONE, legacy), legacy);
    unknown_cycle->cycleHash = uint256::ONE;
    for (const auto& lock : {good, unreadable, unknown_cycle}) {
        InstantSendQueueTest::Queue(manager, lock);
    }
    BOOST_REQUIRE_EQUAL(InstantSendQueueTest::Size(manager, false), 3U);

    BOOST_CHECK(!InstantSendQueueTest::WorkerStep(manager, *m_node.peerman));
    BOOST_CHECK_EQUAL(InstantSendQueueTest::Size(manager, false), 0U);
    BOOST_CHECK(InstantSendQueueTest::HasNoTx(manager, SerializeHash(*good)));
    BOOST_CHECK(!InstantSendQueueTest::HasNoTx(manager, SerializeHash(*unreadable)));
    BOOST_CHECK(!InstantSendQueueTest::HasNoTx(manager, SerializeHash(*unknown_cycle)));
    BOOST_CHECK_EQUAL(InstantSendQueueTest::Size(manager, true), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
