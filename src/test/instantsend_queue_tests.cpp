// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <chainparams.h>
#include <llmq/context.h>
#include <llmq/instantsend.h>
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

BOOST_AUTO_TEST_SUITE_END()
