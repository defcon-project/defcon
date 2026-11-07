// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <dbwrapper.h>
#include <node/storage_error.h>
#include <primitives/block.h>
#include <scheduler.h>
#include <shutdown.h>
#include <test/util/setup_common.h>
#include <util/thread.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
// Restore the default (rethrow) when a test is done with its handler.
struct HandlerReset {
    ~HandlerReset() { util::SetThreadExceptionHandler({}); }
};

// A validation subscriber whose callback fails once with a storage error, as one that reads the damaged
// database would (the peer manager's UpdatedBlockTip, a wallet's ChainStateFlushed).
struct FailingOnceSubscriber : public CValidationInterface {
    int calls{0};
    bool fail_next{true};

protected:
    void ChainStateFlushed(const CBlockLocator&) override
    {
        ++calls;
        if (fail_next) {
            fail_next = false;
            throw dbwrapper_error("list diff missing");
        }
    }
};

struct CountingSubscriber : public CValidationInterface {
    int calls{0};

protected:
    void ChainStateFlushed(const CBlockLocator&) override { ++calls; }
};

// A subscriber whose BlockConnected fails with a storage error, as the masternode and InstantSend managers'
// (CDSNotificationInterface, registered before the wallet) would on a damaged database.
struct FailingBlockSubscriber : public CValidationInterface {
    int blocks{0};

protected:
    void BlockConnected(const std::shared_ptr<const CBlock>&, const CBlockIndex*) override
    {
        ++blocks;
        throw dbwrapper_error("list diff missing");
    }
};

// A subscriber that, like the wallet, processes the connected blocks and records the chain state flushes.
struct WalletLikeSubscriber : public CValidationInterface {
    int blocks{0};
    int flushes{0};
    int blocks_seen_at_flush{-1};

protected:
    void BlockConnected(const std::shared_ptr<const CBlock>&, const CBlockIndex*) override { ++blocks; }
    void ChainStateFlushed(const CBlockLocator&) override
    {
        ++flushes;
        blocks_seen_at_flush = blocks;
    }
};

// A subscriber whose callback fails once with an error nobody handles (a wallet's descriptor cache write that
// failed, say), which must still end the node as it did before.
struct ThrowingOnceSubscriber : public CValidationInterface {
    int calls{0};
    bool fail_next{true};

protected:
    void ChainStateFlushed(const CBlockLocator&) override
    {
        ++calls;
        if (fail_next) {
            fail_next = false;
            throw std::runtime_error("descriptor cache write failed");
        }
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(util_thread_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(trace_thread_rethrows_without_a_handler)
{
    HandlerReset reset;
    util::SetThreadExceptionHandler({});
    BOOST_CHECK_THROW(util::TraceThread("test", [] { throw std::runtime_error("boom"); }), std::runtime_error);
    BOOST_CHECK_NO_THROW(util::TraceThread("test", [] {}));
}

BOOST_AUTO_TEST_CASE(trace_thread_ends_quietly_when_the_handler_accepts)
{
    HandlerReset reset;
    std::string seen_thread;
    std::string seen_what;
    util::SetThreadExceptionHandler([&](const char* thread_name, std::exception_ptr exception) {
        seen_thread = thread_name;
        try {
            std::rethrow_exception(exception);
        } catch (const dbwrapper_error& e) {
            seen_what = e.what();
            return true;
        } catch (...) {
            return false;
        }
    });
    // Accepted: the thread function ends; the exception does not leave TraceThread.
    BOOST_CHECK_NO_THROW(util::TraceThread("isman", [] { throw dbwrapper_error("list diff missing"); }));
    BOOST_CHECK_EQUAL(seen_thread, "isman");
    BOOST_CHECK_EQUAL(seen_what, "list diff missing");
    // Declined: rethrown as before.
    BOOST_CHECK_THROW(util::TraceThread("isman", [] { throw std::runtime_error("other"); }), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(storage_error_handler_stops_the_node_for_database_errors_only)
{
    // The shutdown request writes to the shutdown token pipe, which the node's init sets up.
    BOOST_REQUIRE(InitShutdownState());
    BOOST_REQUIRE(!ShutdownRequested());

    // Not a storage error: left to the caller, no shutdown.
    BOOST_CHECK(!HandleStorageError(std::make_exception_ptr(std::runtime_error("other")), "a test"));
    BOOST_CHECK(!HandleStorageError(nullptr, "a test"));
    BOOST_CHECK(!ShutdownRequested());

    // A storage error: handled, and the node is asked to shut down in order.
    BOOST_CHECK(HandleStorageError(std::make_exception_ptr(dbwrapper_error("diff missing")), "a test"));
    BOOST_CHECK(ShutdownRequested());
    AbortShutdown();

    // The thread handler is the same gate.
    BOOST_CHECK(!HandleStorageErrorOnThread("isman", std::make_exception_ptr(std::runtime_error("other"))));
    BOOST_CHECK(!ShutdownRequested());
    BOOST_CHECK(HandleStorageErrorOnThread("isman", std::make_exception_ptr(dbwrapper_error("diff missing"))));
    BOOST_CHECK(ShutdownRequested());
    AbortShutdown();
}

// A storage error in a scheduled task must not end the thread serving the queue: validation waits on that
// queue (SyncWithValidationInterfaceQueue) and the shutdown drains it. The task after the failing one still runs,
// and the shutdown was requested.
BOOST_AUTO_TEST_CASE(scheduler_keeps_serving_after_a_storage_error)
{
    BOOST_REQUIRE(InitShutdownState());
    BOOST_REQUIRE(!ShutdownRequested());
    CScheduler scheduler;
    std::thread thread([&] { RunHandlingStorageErrors([&] { scheduler.serviceQueue(); }, "a test scheduler"); });
    std::promise<void> after;
    const auto now = std::chrono::steady_clock::now();
    scheduler.schedule([] { throw dbwrapper_error("list diff missing"); }, now);
    scheduler.schedule([&] { after.set_value(); }, now + std::chrono::milliseconds{1});
    BOOST_CHECK(after.get_future().wait_for(std::chrono::seconds{30}) == std::future_status::ready);
    BOOST_CHECK(ShutdownRequested());
    scheduler.stop();
    thread.join();
    AbortShutdown();
}

// The shutdown drains the validation callbacks itself, outside any thread boundary (PrepareShutdown). A storage
// error in a drained callback is handled and the draining goes on: every other callback runs, none stays pending.
// An error of another kind still propagates.
BOOST_AUTO_TEST_CASE(shutdown_drain_handles_storage_errors)
{
    BOOST_REQUIRE(InitShutdownState());
    BOOST_REQUIRE(!ShutdownRequested());
    CScheduler scheduler; // no service thread: EmptyQueue runs the callbacks on this thread, as PrepareShutdown does
    SingleThreadedSchedulerClient client{scheduler};
    int ran{0};
    client.AddToProcessQueue([] { throw dbwrapper_error("list diff missing"); });
    client.AddToProcessQueue([&] { ++ran; });
    client.AddToProcessQueue([] { throw dbwrapper_error("list diff missing again"); });
    client.AddToProcessQueue([&] { ++ran; });
    BOOST_CHECK_NO_THROW(RunHandlingStorageErrors([&] { client.EmptyQueue(); }, "a test drain"));
    BOOST_CHECK_EQUAL(ran, 2);
    BOOST_CHECK_EQUAL(client.CallbacksPending(), 0U);
    BOOST_CHECK(ShutdownRequested());
    AbortShutdown();

    BOOST_CHECK_THROW(RunHandlingStorageErrors([] { throw std::runtime_error("other"); }, "a test"), std::runtime_error);
    BOOST_CHECK(!ShutdownRequested());
}

// A subscriber whose drained callback leaves by a storage error must be gone from the signal list once it has
// unregistered: the handled shutdown drains the callbacks twice, and the peer manager is destroyed in between
// (PrepareShutdown), so Iterate gives the entry's execution count back on the exception path too. No background
// scheduler thread here: the callbacks are drained on this thread, as PrepareShutdown does.
BOOST_AUTO_TEST_CASE(unregistered_subscriber_is_not_called_after_a_storage_error)
{
    BOOST_REQUIRE(InitShutdownState());
    BOOST_REQUIRE(!ShutdownRequested());
    CScheduler scheduler;
    GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);
    FailingOnceSubscriber failing;
    CountingSubscriber counting;
    // Raw-pointer registrations, as the node registers the peer manager: the list does not keep them alive.
    RegisterValidationInterface(&failing);
    RegisterValidationInterface(&counting);

    // First event: the failing subscriber's callback leaves by a storage error; the drain handles it and goes
    // on, and the subscriber after it still sees this event.
    GetMainSignals().ChainStateFlushed(CBlockLocator{});
    RunHandlingStorageErrors([] { GetMainSignals().FlushBackgroundCallbacks(); }, "a test drain");
    BOOST_CHECK_EQUAL(failing.calls, 1);
    BOOST_CHECK_EQUAL(counting.calls, 1);
    BOOST_CHECK(ShutdownRequested());
    BOOST_CHECK_EQUAL(GetMainSignals().CallbacksPending(), 0U);

    // The failing subscriber unregisters, as the peer manager does before it is destroyed.
    UnregisterValidationInterface(&failing);

    // Second event and drain (the forced flush's ChainStateFlushed): the unregistered subscriber is not called,
    // the remaining one is.
    GetMainSignals().ChainStateFlushed(CBlockLocator{});
    RunHandlingStorageErrors([] { GetMainSignals().FlushBackgroundCallbacks(); }, "a test drain");
    BOOST_CHECK_EQUAL(failing.calls, 1);
    BOOST_CHECK_EQUAL(counting.calls, 2);
    BOOST_CHECK_EQUAL(GetMainSignals().CallbacksPending(), 0U);

    UnregisterValidationInterface(&counting);
    GetMainSignals().UnregisterBackgroundSignalScheduler();
    AbortShutdown();
}

// A storage error in one subscriber's callback must not cost the subscribers after it the event. The wallet is
// registered after the masternode and InstantSend managers (init.cpp); had it missed a BlockConnected while still
// receiving the ChainStateFlushed of the shutdown's forced flush, it would have recorded a locator past a block it
// never processed, and the next start would not have rescanned it (on the base the exception ended the process
// instead, which left the locator behind the block). Two failing subscribers: the second error is logged, the drain
// still finishes, and the subscriber after them has seen the block when the flush arrives.
BOOST_AUTO_TEST_CASE(later_subscribers_still_see_the_event_after_a_storage_error)
{
    BOOST_REQUIRE(InitShutdownState());
    BOOST_REQUIRE(!ShutdownRequested());
    CScheduler scheduler;
    GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);
    FailingBlockSubscriber failing_first;
    FailingBlockSubscriber failing_second;
    WalletLikeSubscriber wallet;
    RegisterValidationInterface(&failing_first);
    RegisterValidationInterface(&failing_second);
    RegisterValidationInterface(&wallet);

    const auto block = std::make_shared<const CBlock>();
    CBlockIndex index;
    index.nHeight = 1;
    GetMainSignals().BlockConnected(block, &index);
    RunHandlingStorageErrors([] { GetMainSignals().FlushBackgroundCallbacks(); }, "a test drain");
    BOOST_CHECK_EQUAL(failing_first.blocks, 1);
    BOOST_CHECK_EQUAL(failing_second.blocks, 1);
    BOOST_CHECK_EQUAL(wallet.blocks, 1);
    BOOST_CHECK(ShutdownRequested());
    BOOST_CHECK_EQUAL(GetMainSignals().CallbacksPending(), 0U);

    // The shutdown's forced flush: the wallet records it having seen the block.
    GetMainSignals().ChainStateFlushed(CBlockLocator{});
    RunHandlingStorageErrors([] { GetMainSignals().FlushBackgroundCallbacks(); }, "a test drain");
    BOOST_CHECK_EQUAL(wallet.flushes, 1);
    BOOST_CHECK_EQUAL(wallet.blocks_seen_at_flush, 1);
    BOOST_CHECK_EQUAL(GetMainSignals().CallbacksPending(), 0U);

    UnregisterValidationInterface(&failing_first);
    UnregisterValidationInterface(&failing_second);
    UnregisterValidationInterface(&wallet);
    GetMainSignals().UnregisterBackgroundSignalScheduler();
    AbortShutdown();
}

// A storage error in one subscriber must not hide an error of another kind in a later (or an earlier) subscriber:
// the handler lets a storage error pass and goes on, so had the drain reported only the storage error, the unhandled
// one would have been swallowed and the node would have gone on as if nothing had happened. In either order the
// unhandled error leaves the drain (and ends the node at the thread boundary, as before), every subscriber still
// sees the event, and the queue is empty.
BOOST_AUTO_TEST_CASE(an_unhandled_error_is_not_hidden_by_a_storage_error)
{
    BOOST_REQUIRE(InitShutdownState());
    BOOST_REQUIRE(!ShutdownRequested());
    CScheduler scheduler;
    GetMainSignals().RegisterBackgroundSignalScheduler(scheduler);
    for (const bool storage_error_first : {true, false}) {
        FailingOnceSubscriber storage;
        ThrowingOnceSubscriber other;
        CountingSubscriber counting;
        if (storage_error_first) {
            RegisterValidationInterface(&storage);
            RegisterValidationInterface(&other);
        } else {
            RegisterValidationInterface(&other);
            RegisterValidationInterface(&storage);
        }
        RegisterValidationInterface(&counting);

        GetMainSignals().ChainStateFlushed(CBlockLocator{});
        BOOST_CHECK_THROW(RunHandlingStorageErrors([] { GetMainSignals().FlushBackgroundCallbacks(); }, "a test drain"),
                          std::runtime_error);
        BOOST_CHECK_EQUAL(storage.calls, 1);
        BOOST_CHECK_EQUAL(other.calls, 1);
        BOOST_CHECK_EQUAL(counting.calls, 1);
        BOOST_CHECK_EQUAL(GetMainSignals().CallbacksPending(), 0U);
        // The storage error gave way to the unhandled one, so the drain did not handle it (the node ends anyway).
        BOOST_CHECK(!ShutdownRequested());
        AbortShutdown(); // if it was requested after all, the cases after this one still start clean

        UnregisterValidationInterface(&storage);
        UnregisterValidationInterface(&other);
        UnregisterValidationInterface(&counting);
    }
    GetMainSignals().UnregisterBackgroundSignalScheduler();
}

BOOST_AUTO_TEST_SUITE_END()
