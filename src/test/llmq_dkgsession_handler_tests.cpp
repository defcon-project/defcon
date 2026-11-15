// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The phase handler thread replaces a DKG session while the network threads serve its messages
// (CDKGSessionManager::GetContribution and the like, under the handler's cs_phase_qhash). The
// replacement used to free the old session without that lock, so a request served at that moment
// used a destroyed session and its mutex. These cases check that a session is published under the
// lock on every path, that the previous one is not served afterwards, and that publishing waits for
// a request in progress.
//
// DEFCON_DKG_UNLOCKED_CONTROL=1 (for a sanitizer run only): the coordinated and the stress case also
// replace the session the way it used to be done, without the lock, and are expected to fail
// (coordinated) or to be reported by ThreadSanitizer (stress).

#include <llmq/context.h>
#include <llmq/dkgsession.h>
#include <llmq/dkgsessionhandler.h>
#include <llmq/dkgsessionmgr.h>
#include <spork.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

using llmq::CDKGSession;
using llmq::CDKGSessionHandler;
using llmq::CDKGSessionManager;

struct DKGSessionHandlerTestAccess {
    static CDKGSessionHandler& FirstHandler(CDKGSessionManager& manager)
    {
        BOOST_REQUIRE(!manager.dkgSessionHandlers.empty());
        return manager.dkgSessionHandlers.begin()->second;
    }
    static void SetPhase(CDKGSessionHandler& handler, llmq::QuorumPhase phase) EXCLUSIVE_LOCKS_REQUIRED(!handler.cs_phase_qhash)
    {
        LOCK(handler.cs_phase_qhash);
        handler.phase = phase;
    }
    static std::unique_ptr<CDKGSession> FreshSession(const CDKGSessionHandler& handler)
    {
        return std::make_unique<CDKGSession>(nullptr, handler.params, handler.blsWorker, handler.m_dmnman,
                                             handler.dkgManager, handler.dkgDebugManager, handler.m_mn_metaman,
                                             handler.m_mn_activeman, handler.m_sporkman);
    }
    static void AddContribution(CDKGSession& session, const uint256& hash)
    {
        LOCK(session.invCs);
        session.contributions.emplace(hash, llmq::CDKGContribution{});
    }
    static const CDKGSession* Current(const CDKGSessionHandler& handler) { return handler.curSession.get(); }
    static CDKGSession& CurrentRef(CDKGSessionHandler& handler) { return *handler.curSession; }
    static bool Publish(CDKGSessionHandler& handler, std::unique_ptr<CDKGSession> session,
                        const std::function<bool(CDKGSession&)>& prepare) EXCLUSIVE_LOCKS_REQUIRED(!handler.cs_phase_qhash)
    {
        return handler.PublishSession(std::move(session), prepare);
    }
    //! The replacement as it used to be done: no lock, the old session freed at once.
    static void ReplaceUnlocked(CDKGSessionHandler& handler, std::unique_ptr<CDKGSession> session)
    {
        handler.curSession = std::move(session);
    }
    static Mutex& PhaseLock(CDKGSessionHandler& handler) LOCK_RETURNED(handler.cs_phase_qhash) { return handler.cs_phase_qhash; }
    static bool ServeLocked(const CDKGSessionHandler& handler, const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(handler.cs_phase_qhash)
    {
        llmq::CDKGContribution ret;
        return handler.GetContribution(hash, ret);
    }
};

using Access = DKGSessionHandlerTestAccess;

namespace {
bool UnlockedControl()
{
    const char* v = std::getenv("DEFCON_DKG_UNLOCKED_CONTROL");
    return v != nullptr && std::string{v} == "1";
}

struct DKGHandlerSetup : public TestingSetup {
    CDKGSessionManager& manager;
    CDKGSessionHandler& handler;

    DKGHandlerSetup() :
        manager{*Assert(Assert(m_node.llmq_ctx)->qdkgsman)},
        handler{Access::FirstHandler(manager)}
    {
        // The manager serves nothing while the DKG spork is off. The fixture runs on the main network's parameters,
        // where every spork is fixed and this one is on (CSporkManager::GetSporkValue).
        BOOST_REQUIRE(Assert(m_node.sporkman.get())->IsSporkActive(SPORK_17_QUORUM_DKG_ENABLED));
        // The phase in which GetContribution serves and the phase thread replaces the session.
        Access::SetPhase(handler, llmq::QuorumPhase::Initialized);
    }

    bool Served(const uint256& hash)
    {
        llmq::CDKGContribution ret;
        return manager.GetContribution(hash, ret);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(llmq_dkgsession_handler_tests, DKGHandlerSetup)

BOOST_AUTO_TEST_CASE(a_contribution_of_the_current_session_is_served)
{
    const uint256 hash{uint256S("a1")};
    BOOST_CHECK(!Served(hash));
    Access::AddContribution(Access::CurrentRef(handler), hash);
    BOOST_CHECK(Served(hash));
}

BOOST_AUTO_TEST_CASE(a_session_that_fails_to_prepare_still_replaces_the_old_one)
{
    const uint256 hash{uint256S("b1")};
    Access::AddContribution(Access::CurrentRef(handler), hash);
    BOOST_REQUIRE(Served(hash));
    const CDKGSession* old{Access::Current(handler)};
    BOOST_CHECK(!Access::Publish(handler, Access::FreshSession(handler), [](CDKGSession&) { return false; }));
    BOOST_CHECK(Access::Current(handler) != old);
    BOOST_CHECK(!Served(hash));
}

BOOST_AUTO_TEST_CASE(a_session_whose_preparation_throws_still_replaces_the_old_one)
{
    const uint256 hash{uint256S("c1")};
    Access::AddContribution(Access::CurrentRef(handler), hash);
    BOOST_REQUIRE(Served(hash));
    const CDKGSession* old{Access::Current(handler)};
    // The same exception reaches the caller (the phase thread's handler), after the swap.
    BOOST_CHECK_THROW(Access::Publish(handler, Access::FreshSession(handler),
                                      [](CDKGSession&) -> bool { throw std::runtime_error("history unavailable"); }),
                      std::runtime_error);
    BOOST_CHECK(Access::Current(handler) != old);
    BOOST_CHECK(!Served(hash));
}

BOOST_AUTO_TEST_CASE(publishing_waits_for_a_request_in_progress)
{
    const bool control{UnlockedControl()};
    const uint256 hash{uint256S("d1")};
    Access::AddContribution(Access::CurrentRef(handler), hash);
    BOOST_REQUIRE(Served(hash));
    const CDKGSession* old{Access::Current(handler)};

    std::promise<void> prepared;
    std::future<void> prepared_future{prepared.get_future()};
    std::thread publisher;
    bool replaced_under_the_lock{false};
    {
        // A request in progress: the manager holds this lock while it serves.
        LOCK(Access::PhaseLock(handler));
        publisher = std::thread([&] {
            auto fresh{Access::FreshSession(handler)};
            if (control) {
                prepared.set_value();
                Access::ReplaceUnlocked(handler, std::move(fresh));
                return;
            }
            Access::Publish(handler, std::move(fresh), [&](CDKGSession&) {
                prepared.set_value();
                return true;
            });
        });
        prepared_future.wait();
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        replaced_under_the_lock = Access::Current(handler) != old;
        BOOST_CHECK_MESSAGE(!replaced_under_the_lock, "the session was replaced while a request held the lock");
        // Still served, from the old session, which is alive (the getter itself: this thread holds the lock).
        if (!replaced_under_the_lock) BOOST_CHECK(Access::ServeLocked(handler, hash));
    }
    publisher.join();
    BOOST_CHECK(Access::Current(handler) != old);
    BOOST_CHECK(!Served(hash));
}

BOOST_AUTO_TEST_CASE(publishing_and_serving_at_once)
{
    // Proves nothing alone; under ThreadSanitizer it is the run that would report the race.
    const bool control{UnlockedControl()};
    const uint256 hash{uint256S("e1")};
    constexpr int rounds{500};
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load()) {
            Served(hash);
        }
    });
    for (int i = 0; i < rounds; ++i) {
        auto fresh{Access::FreshSession(handler)};
        Access::AddContribution(*fresh, hash);
        if (control) {
            Access::ReplaceUnlocked(handler, std::move(fresh));
        } else {
            Access::Publish(handler, std::move(fresh), [](CDKGSession&) { return true; });
        }
    }
    stop = true;
    reader.join();
    BOOST_CHECK(Served(hash));
}

BOOST_AUTO_TEST_SUITE_END()
