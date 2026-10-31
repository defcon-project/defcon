// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <consensus/validation.h>
#include <init.h>
#include <primitives/block.h>
#include <scheduler.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>

#include <atomic>
#include <chrono>
#include <thread>

BOOST_FIXTURE_TEST_SUITE(validationinterface_tests, ChainTestingSetup)

struct TestSubscriberNoop final : public CValidationInterface {
    void BlockChecked(const CBlock&, const BlockValidationState&) override {}
};

BOOST_AUTO_TEST_CASE(unregister_validation_interface_race)
{
    std::atomic<bool> generate{true};

    // Start thread to generate notifications
    std::thread gen{[&] {
        const CBlock block_dummy;
        BlockValidationState state_dummy;
        while (generate) {
            GetMainSignals().BlockChecked(block_dummy, state_dummy);
        }
    }};

    // Start thread to consume notifications
    std::thread sub{[&] {
        // keep going for about 1 sec, which is 250k iterations
        for (int i = 0; i < 250000; i++) {
            auto sub = std::make_shared<TestSubscriberNoop>();
            RegisterSharedValidationInterface(sub);
            UnregisterSharedValidationInterface(sub);
        }
        // tell the other thread we are done
        generate = false;
    }};

    gen.join();
    sub.join();
    BOOST_CHECK(!generate);
}

// ActivateBestChain waits for the queue when more than ten callbacks are pending, and the scheduler
// runs them. A shutdown that stops the scheduler while the block import thread still runs can
// therefore wait for that thread forever. The verdict is the order itself, not a timeout: the
// scheduler has stopped and the import has not ended. That cannot happen when the import thread is
// joined first, however slow the machine is; the correct order always ends the test.
BOOST_FIXTURE_TEST_CASE(block_import_ends_before_the_scheduler_stops, TestingSetup)
{
    constexpr size_t QUEUED{11};
    CScheduler& scheduler{*m_node.scheduler};
    std::atomic<bool> shutdown_started{false}, shutdown_finished{false};
    std::atomic<bool> import_done{false}, barrier_reached{false};

    // The scheduler thread has run a callback, so it counts as servicing from here on.
    SyncWithValidationInterfaceQueue();
    BOOST_REQUIRE(scheduler.AreThreadsServicingQueue());

    m_node.chainman->m_load_block = std::thread([&] {
        while (!shutdown_started) std::this_thread::yield();
        // A shutdown in the wrong order stops the idle scheduler now; in the right order it cannot.
        const auto give_up{std::chrono::steady_clock::now() + std::chrono::milliseconds{500}};
        while (scheduler.AreThreadsServicingQueue() && std::chrono::steady_clock::now() < give_up) {
            UninterruptibleSleep(std::chrono::milliseconds{1});
        }
        // Keep the queue over the limit until ActivateBestChain has added its own callback, so the
        // barrier is really reached: while this one runs, only the others are pending.
        CallFunctionInValidationInterfaceQueue([&] {
            const auto limit{std::chrono::steady_clock::now() + std::chrono::seconds{60}};
            while (GetMainSignals().CallbacksPending() <= QUEUED && std::chrono::steady_clock::now() < limit) {
                UninterruptibleSleep(std::chrono::milliseconds{1});
            }
            barrier_reached = GetMainSignals().CallbacksPending() > QUEUED;
        });
        for (size_t i{0}; i < QUEUED; ++i) {
            CallFunctionInValidationInterfaceQueue([] {});
        }
        BlockValidationState state;
        m_node.chainman->ActiveChainstate().ActivateBestChain(state, nullptr);
        import_done = true;
    });

    std::thread shutdown{[&] {
        shutdown_started = true;
        StopBlockImportAndScheduler(m_node);
        shutdown_finished = true;
    }};

    bool stopped_during_import{false};
    while (!shutdown_finished) {
        // In this order: the import cannot have ended after the scheduler was seen stopped and
        // before import_done was read, unless the scheduler really stopped first.
        if (!scheduler.AreThreadsServicingQueue() && !import_done) {
            stopped_during_import = true;
            // Run what the stopped scheduler left behind, so a failure ends the test and not the run.
            GetMainSignals().FlushBackgroundCallbacks();
        }
        UninterruptibleSleep(std::chrono::milliseconds{1});
    }
    shutdown.join();

    BOOST_CHECK(!stopped_during_import);
    BOOST_CHECK(import_done);
    BOOST_CHECK(barrier_reached);
    BOOST_CHECK(!m_node.chainman->m_load_block.joinable());
    BOOST_CHECK(!scheduler.AreThreadsServicingQueue());
}

class TestInterface : public CValidationInterface
{
public:
    TestInterface(std::function<void()> on_call = nullptr, std::function<void()> on_destroy = nullptr)
        : m_on_call(std::move(on_call)), m_on_destroy(std::move(on_destroy))
    {
    }
    virtual ~TestInterface()
    {
        if (m_on_destroy) m_on_destroy();
    }
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override
    {
        if (m_on_call) m_on_call();
    }
    static void Call()
    {
        CBlock block;
        BlockValidationState state;
        GetMainSignals().BlockChecked(block, state);
    }
    std::function<void()> m_on_call;
    std::function<void()> m_on_destroy;
};

// Regression test to ensure UnregisterAllValidationInterfaces calls don't
// destroy a validation interface while it is being called. Bug:
// https://github.com/bitcoin/bitcoin/pull/18551
BOOST_AUTO_TEST_CASE(unregister_all_during_call)
{
    bool destroyed = false;
    RegisterSharedValidationInterface(std::make_shared<TestInterface>(
        [&] {
            // First call should decrements reference count 2 -> 1
            UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
            // Second call should not decrement reference count 1 -> 0
            UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
        },
        [&] { destroyed = true; }));
    TestInterface::Call();
    BOOST_CHECK(destroyed);
}

BOOST_AUTO_TEST_SUITE_END()
