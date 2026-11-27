// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// SF1-C2: the report pre-verifier (dsl::ReportPreverifier), the receive queue's two lists and its gate in the wave
// window (CNode::PollMessage), the overtake policy, and a real PeerManager taking reports through the receive queue.

#include <arith_uint256.h>
#include <bls/bls.h>
#include <chain.h>
#include <chainparams.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/evodb.h>
#include <evo/pose_service_manager.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_preverify.h>
#include <evo/pose_service_sentinels.h>
#include <evo/pose_service_sigcache.h>
#include <key.h>
#include <masternode/meta.h>
#include <masternode/node.h>
#include <masternode/sync.h>
#include <net.h>
#include <net_processing.h>
#include <netfulfilledman.h>
#include <protocol.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <util/string.h>
#include <validation.h>
#include <validationinterface.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;

namespace {
using dsl::PerfMetric;
using dsl::PreverifyJob;
using dsl::ReportPreverifier;
using Offer = ReportPreverifier::OfferResult;

std::shared_ptr<PreverifyJob> Job(int64_t peer, uint64_t key)
{
    auto job = std::make_shared<PreverifyJob>();
    job->peer = peer;
    job->dup_key = ArithToUint256(arith_uint256{key});
    return job;
}

CNetMessage Message(const std::string& type, const std::vector<unsigned char>& payload = {})
{
    CNetMessage msg{CDataStream{MakeUCharSpan(payload), SER_NETWORK, PROTOCOL_VERSION}};
    msg.m_type = type;
    return msg;
}
std::vector<unsigned char> Inventory(const std::vector<uint32_t>& types)
{
    std::vector<CInv> inv;
    for (const uint32_t type : types) inv.emplace_back(type, ArithToUint256(arith_uint256{inv.size() + 1}));
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << inv;
    return {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()};
}

// Counter totals over every epoch bucket of the perf window.
struct Tally {
    std::array<uint64_t, static_cast<size_t>(PerfMetric::COUNT)> count{};
    uint64_t operator[](PerfMetric metric) const { return count[static_cast<size_t>(metric)]; }
    void Add(const dsl::PerfSnapshot& snapshot)
    {
        for (const auto& epoch : snapshot.epochs) {
            for (size_t i = 0; i < count.size(); ++i) count[i] += epoch.values[i].count;
        }
    }
};

/** The process-wide test delay (-dsltestprocessdelay) for as long as it lives: none again when it goes, also when a
 *  failed requirement ends the test early. */
class TestDelay
{
public:
    explicit TestDelay(std::chrono::milliseconds delay) { dsl::SetTestProcessDelay(delay); }
    ~TestDelay() { dsl::SetTestProcessDelay(std::chrono::milliseconds{0}); }
    TestDelay(const TestDelay&) = delete;
    TestDelay& operator=(const TestDelay&) = delete;
};

/** A point where a test stops another thread, reached through one of the product's test hooks (a plain function
 *  pointer). It is static (StopPoint::Of), so a thread that read the hook before the test took it away never reaches
 *  a gone object; and Done() lets the thread go and waits until none is inside any more. */
class StopPoint
{
public:
    template <int N>
    static StopPoint& Of()
    {
        static StopPoint point;
        return point;
    }
    void Reset() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_reached = m_released = m_signalled = false;
    }
    //! The stopped thread: tells it is here, and waits until it is released.
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WAIT_LOCK(m_mutex, lock);
        ++m_inside;
        m_reached = true;
        m_cv.notify_all();
        m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_released; });
        --m_inside;
        m_cv.notify_all();
    }
    //! The stopped thread, later: tells that it passed a second point.
    void Signal() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_signalled = true;
        m_cv.notify_all();
    }
    bool WaitReached(std::chrono::seconds timeout) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WAIT_LOCK(m_mutex, lock);
        return m_cv.wait_for(lock, timeout, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_reached; });
    }
    bool WaitSignalled(std::chrono::seconds timeout) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WAIT_LOCK(m_mutex, lock);
        return m_cv.wait_for(lock, timeout, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_signalled; });
    }
    void Release() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_released = true;
        m_cv.notify_all();
    }
    //! Releases, and returns once no thread is inside Stop(). Call it after the hook is taken away.
    void Done() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        WAIT_LOCK(m_mutex, lock);
        m_released = true;
        m_cv.notify_all();
        m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_inside == 0; });
    }

private:
    StopPoint() = default;
    Mutex m_mutex;
    std::condition_variable m_cv;
    bool m_reached GUARDED_BY(m_mutex){false};
    bool m_released GUARDED_BY(m_mutex){false};
    bool m_signalled GUARDED_BY(m_mutex){false};
    int m_inside GUARDED_BY(m_mutex){0};
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(dsl_preverify_tests, BasicTestingSetup)

// ---- the pre-verifier on its own ----

BOOST_AUTO_TEST_CASE(jobs_are_served_one_peer_at_a_time_in_turn)
{
    std::vector<int64_t> served;
    ReportPreverifier verifier{[&](PreverifyJob& job) { served.push_back(job.peer); }, /*threaded=*/false};
    verifier.SetLimits(8, 64);
    uint64_t key{0};
    for (int64_t peer : {1, 1, 1, 2, 2, 3}) BOOST_CHECK(verifier.Offer(Job(peer, ++key)) == Offer::QUEUED);
    while (verifier.RunOne()) {}
    // One job of each line in turn, each line in the order of its offers -- not the order of the offers overall.
    BOOST_CHECK((served == std::vector<int64_t>{1, 2, 3, 1, 2, 1}));
}

BOOST_AUTO_TEST_CASE(a_waiting_peer_is_served_before_the_peers_that_came_after_it)
{
    std::vector<int64_t> served;
    ReportPreverifier verifier{[&](PreverifyJob& job) { served.push_back(job.peer); }, /*threaded=*/false};
    verifier.SetLimits(8, 64);
    uint64_t key{0};
    BOOST_CHECK(verifier.Offer(Job(5, ++key)) == Offer::QUEUED);
    BOOST_CHECK(verifier.Offer(Job(5, ++key)) == Offer::QUEUED);
    BOOST_CHECK(verifier.RunOne());
    // Before every turn from here on, a peer with a greater id than any so far gets a waiting job. Peer 5's second job
    // was in line before each of them: it is served next, and they in the order they came.
    for (int64_t peer = 6; peer <= 9; ++peer) {
        BOOST_CHECK(verifier.Offer(Job(peer, ++key)) == Offer::QUEUED);
        BOOST_CHECK(verifier.RunOne());
    }
    while (verifier.RunOne()) {}
    BOOST_CHECK((served == std::vector<int64_t>{5, 5, 6, 7, 8, 9}));
}

BOOST_AUTO_TEST_CASE(a_full_verifier_drops_the_newest_waiting_job_of_the_peer_holding_most)
{
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(4, 5);
    std::vector<std::shared_ptr<PreverifyJob>> a;
    for (uint64_t i = 1; i <= 4; ++i) {
        a.push_back(Job(1, i));
        BOOST_CHECK(verifier.Offer(a.back()) == Offer::QUEUED);
    }
    BOOST_CHECK(verifier.Offer(Job(2, 10)) == Offer::QUEUED);
    BOOST_CHECK_EQUAL(verifier.Held(), 5U);
    // Full: peer 1 holds the most, so its newest waiting job makes room, in the same step.
    bool evicted{false};
    BOOST_CHECK(verifier.Offer(Job(2, 11), &evicted) == Offer::QUEUED);
    BOOST_CHECK(evicted);
    BOOST_CHECK_EQUAL(a[3]->State(), NetMessagePrep::DROPPED);
    BOOST_CHECK_EQUAL(a[2]->State(), NetMessagePrep::PENDING);
    BOOST_CHECK_EQUAL(verifier.HeldBy(1), 3U);
    BOOST_CHECK_EQUAL(verifier.HeldBy(2), 2U);
    // Peer 1 still holds the most: its own new job is refused, and nothing is dropped for it.
    evicted = false;
    BOOST_CHECK(verifier.Offer(Job(1, 12), &evicted) == Offer::QUEUE_FULL);
    BOOST_CHECK(!evicted);
    BOOST_CHECK_EQUAL(verifier.Held(), 5U);
    // A tie is not "more": two peers holding as much never drop each other's jobs.
    ReportPreverifier even{[](PreverifyJob&) {}, /*threaded=*/false};
    even.SetLimits(4, 4);
    for (uint64_t i = 1; i <= 2; ++i) BOOST_CHECK(even.Offer(Job(1, i)) == Offer::QUEUED);
    for (uint64_t i = 3; i <= 4; ++i) BOOST_CHECK(even.Offer(Job(2, i)) == Offer::QUEUED);
    BOOST_CHECK(even.Offer(Job(2, 5)) == Offer::QUEUE_FULL);
    // The per-peer limit refuses whatever the total.
    ReportPreverifier small{[](PreverifyJob&) {}, /*threaded=*/false};
    small.SetLimits(1, 10);
    BOOST_CHECK(small.Offer(Job(1, 1)) == Offer::QUEUED);
    BOOST_CHECK(small.Offer(Job(1, 2)) == Offer::QUEUE_FULL);
}

BOOST_AUTO_TEST_CASE(an_evicted_jobs_bytes_can_get_a_job_again)
{
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(4, 3);
    auto first = Job(1, 1);
    auto second = Job(1, 2);
    BOOST_CHECK(verifier.Offer(first) == Offer::QUEUED);
    BOOST_CHECK(verifier.Offer(second) == Offer::QUEUED);
    BOOST_CHECK(verifier.Offer(Job(2, 3)) == Offer::QUEUED);
    bool evicted{false};
    BOOST_CHECK(verifier.Offer(Job(3, 4), &evicted) == Offer::QUEUED);
    BOOST_CHECK(evicted);
    BOOST_CHECK_EQUAL(second->State(), NetMessagePrep::DROPPED);
    // The dropped job's key left the index with it: another copy of those bytes gets a job (here it makes room
    // again, from peer 1, which still holds the most).
    BOOST_CHECK(verifier.Offer(Job(4, 2), &evicted) == Offer::QUEUED);
    BOOST_CHECK(evicted);
    BOOST_CHECK_EQUAL(first->State(), NetMessagePrep::DROPPED);
    // A report whose job was dropped is taken the way a report without one is.
    BOOST_CHECK(!second->TryTake());
}

BOOST_AUTO_TEST_CASE(the_same_bytes_get_no_job_until_the_first_report_is_taken)
{
    std::promise<void> started;
    std::promise<void> release;
    std::shared_future<void> release_future{release.get_future().share()};
    // Released however the test ends, so the verifier's destructor never joins a thread that still waits.
    bool released{false};
    const auto release_once = [&] {
        if (!released) release.set_value();
        released = true;
    };
    struct Guard {
        std::function<void()> fn;
        ~Guard() { fn(); }
    };
    std::atomic<int> runs{0};
    ReportPreverifier verifier{[&](PreverifyJob&) {
                                   if (runs++ == 0) {
                                       started.set_value();
                                       release_future.wait();
                                   }
                               },
                               /*threaded=*/true};
    const Guard guard{release_once};
    verifier.SetLimits(8, 64);
    auto original = Job(1, 7);
    BOOST_REQUIRE(verifier.Offer(original) == Offer::QUEUED);
    BOOST_REQUIRE(started.get_future().wait_for(30s) == std::future_status::ready);
    // Running.
    BOOST_CHECK_EQUAL(original->State(), NetMessagePrep::RUNNING);
    BOOST_CHECK(verifier.Offer(Job(2, 7)) == Offer::DUP_PENDING);
    release_once();
    for (int i = 0; i < 500 && original->State() != NetMessagePrep::DONE; ++i) std::this_thread::sleep_for(10ms);
    BOOST_REQUIRE_EQUAL(original->State(), NetMessagePrep::DONE);
    // Done, but its report not taken yet.
    BOOST_CHECK(verifier.Offer(Job(3, 7)) == Offer::DUP_PENDING);
    verifier.Taken(*original->prep);
    BOOST_CHECK_EQUAL(verifier.Held(), 0U);
    BOOST_CHECK(verifier.Offer(Job(3, 7)) == Offer::QUEUED);

    // Waiting.
    ReportPreverifier manual{[](PreverifyJob&) {}, /*threaded=*/false};
    manual.SetLimits(8, 64);
    auto waiting = Job(1, 9);
    BOOST_CHECK(manual.Offer(waiting) == Offer::QUEUED);
    BOOST_CHECK(manual.Offer(Job(2, 9)) == Offer::DUP_PENDING);
}

BOOST_AUTO_TEST_CASE(a_gone_peer_releases_every_job_it_held)
{
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(8, 64);
    auto done = Job(1, 1);
    auto waiting = Job(1, 2);
    BOOST_CHECK(verifier.Offer(done) == Offer::QUEUED);
    BOOST_CHECK(verifier.RunOne());
    BOOST_CHECK_EQUAL(done->State(), NetMessagePrep::DONE);
    BOOST_CHECK(verifier.Offer(waiting) == Offer::QUEUED);
    BOOST_CHECK_EQUAL(verifier.HeldBy(1), 2U);
    verifier.DropPeer(1);
    BOOST_CHECK_EQUAL(waiting->State(), NetMessagePrep::DROPPED);
    BOOST_CHECK_EQUAL(verifier.Held(), 0U);
    BOOST_CHECK_EQUAL(verifier.Waiting(), 0U);
    // Neither key is taken any more: the done job's no more than the waiting one's.
    BOOST_CHECK(verifier.Offer(Job(2, 1)) == Offer::QUEUED);
    BOOST_CHECK(verifier.Offer(Job(2, 2)) == Offer::QUEUED);
    // A report taken after its peer went (it cannot be, but if) releases nothing twice.
    verifier.Taken(*done->prep);
    BOOST_CHECK_EQUAL(verifier.Held(), 2U);
}

BOOST_AUTO_TEST_CASE(done_jobs_count_until_their_reports_are_taken)
{
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(2, 2);
    auto one = Job(1, 1);
    auto two = Job(1, 2);
    BOOST_CHECK(verifier.Offer(one) == Offer::QUEUED);
    BOOST_CHECK(verifier.Offer(two) == Offer::QUEUED);
    while (verifier.RunOne()) {}
    // Full of done jobs: none waits, so none can make room.
    bool evicted{false};
    BOOST_CHECK(verifier.Offer(Job(2, 3), &evicted) == Offer::QUEUE_FULL);
    BOOST_CHECK(!evicted);
    verifier.Taken(*one->prep);
    BOOST_CHECK(verifier.Offer(Job(2, 3)) == Offer::QUEUED);
}

BOOST_AUTO_TEST_CASE(a_report_taken_first_is_never_run)
{
    int runs{0};
    ReportPreverifier verifier{[&](PreverifyJob&) { ++runs; }, /*threaded=*/false};
    verifier.SetLimits(8, 64);
    auto taken = Job(1, 1);
    BOOST_CHECK(verifier.Offer(taken) == Offer::QUEUED);
    // Taken by the message thread (its CAS, no mutex), while the job still waits on its line: it is not run.
    BOOST_CHECK(taken->TryTake());
    BOOST_CHECK(!verifier.RunOne());
    BOOST_CHECK_EQUAL(runs, 0);
    BOOST_CHECK_EQUAL(taken->State(), NetMessagePrep::TAKEN);
    verifier.Taken(*taken->prep);
    BOOST_CHECK_EQUAL(verifier.Held(), 0U);
    // Run first: the message thread can no longer take it over.
    auto run = Job(1, 2);
    BOOST_CHECK(verifier.Offer(run) == Offer::QUEUED);
    BOOST_CHECK(verifier.RunOne());
    BOOST_CHECK_EQUAL(runs, 1);
    BOOST_CHECK(!run->TryTake());
    BOOST_CHECK_EQUAL(run->State(), NetMessagePrep::DONE);
}

BOOST_AUTO_TEST_CASE(a_failing_job_is_done_and_changes_nothing)
{
    ReportPreverifier verifier{[](PreverifyJob&) { throw std::runtime_error{"unreadable"}; }, /*threaded=*/false};
    verifier.SetLimits(8, 64);
    auto job = Job(1, 1);
    BOOST_CHECK(verifier.Offer(job) == Offer::QUEUED);
    BOOST_CHECK(verifier.RunOne());
    BOOST_CHECK_EQUAL(job->State(), NetMessagePrep::DONE);
}

BOOST_AUTO_TEST_CASE(stop_drops_the_waiting_jobs_and_refuses_new_ones)
{
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(8, 64);
    auto one = Job(1, 1);
    auto two = Job(2, 2);
    BOOST_CHECK(verifier.Offer(one) == Offer::QUEUED);
    BOOST_CHECK(verifier.Offer(two) == Offer::QUEUED);
    verifier.Stop();
    BOOST_CHECK_EQUAL(one->State(), NetMessagePrep::DROPPED);
    BOOST_CHECK_EQUAL(two->State(), NetMessagePrep::DROPPED);
    BOOST_CHECK_EQUAL(verifier.Held(), 0U);
    BOOST_CHECK(verifier.Offer(Job(1, 3)) == Offer::STOPPED);
}

BOOST_AUTO_TEST_CASE(a_job_no_longer_held_is_freed_and_only_its_state_stays)
{
    // What a report's queue entry keeps of a job is its state (prep): the test keeps that, and watches the job itself.
    struct Watched {
        std::shared_ptr<NetMessagePrep> prep;
        std::weak_ptr<PreverifyJob> job;
    };
    CBLSSecretKey key;
    key.MakeNewKey();
    const auto offer = [&](ReportPreverifier& verifier, int64_t peer, uint64_t dup, bool* evicted = nullptr) {
        auto job = Job(peer, dup);
        job->payload.assign(165, 0x42);
        job->operator_key = key.GetPublicKey();
        BOOST_REQUIRE(verifier.Offer(job, evicted) == Offer::QUEUED);
        return Watched{job->prep, job};
    };
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(4, 2);
    const Watched first = offer(verifier, 1, 1);
    const Watched second = offer(verifier, 1, 2);
    BOOST_CHECK(!first.job.expired() && !second.job.expired()); // held: the verifier keeps them
    // Dropped to make room: freed at once -- payload, key and all -- though its state stays with the queue entry.
    bool evicted{false};
    const Watched third = offer(verifier, 2, 3, &evicted);
    BOOST_REQUIRE(evicted);
    BOOST_CHECK_EQUAL(second.prep->state.load(), NetMessagePrep::DROPPED);
    BOOST_CHECK(second.job.expired());
    // Run, then taken: freed on the take. Taken before it ran: freed on the take as well.
    BOOST_CHECK(verifier.RunOne());
    BOOST_CHECK_EQUAL(first.prep->state.load(), NetMessagePrep::DONE);
    BOOST_CHECK(!first.job.expired());
    verifier.Taken(*first.prep);
    BOOST_CHECK(first.job.expired());
    BOOST_CHECK(third.prep->TryMove(NetMessagePrep::TAKEN));
    verifier.Taken(*third.prep);
    BOOST_CHECK(third.job.expired());
    // A peer gone: its done job and its waiting job alike.
    const Watched done = offer(verifier, 5, 6);
    BOOST_CHECK(verifier.RunOne());
    const Watched waiting = offer(verifier, 5, 7);
    verifier.DropPeer(5);
    BOOST_CHECK(done.job.expired() && waiting.job.expired());
    BOOST_CHECK_EQUAL(verifier.Held(), 0U);

    // Released while it runs (its report taken in arrival order): it lives until its run ends, and no longer.
    std::promise<void> started;
    std::promise<void> release;
    std::shared_future<void> release_future{release.get_future().share()};
    bool released{false};
    const auto release_once = [&] {
        if (!released) release.set_value();
        released = true;
    };
    struct Guard {
        std::function<void()> fn;
        ~Guard() { fn(); }
    };
    std::atomic<bool> had_data{false};
    ReportPreverifier threaded{[&](PreverifyJob& job) {
                                   started.set_value();
                                   release_future.wait();
                                   had_data = job.payload.size() == 165 && job.operator_key.IsValid(); // still its own
                               },
                               /*threaded=*/true};
    const Guard guard{release_once};
    threaded.SetLimits(4, 4);
    const Watched running = offer(threaded, 1, 9);
    BOOST_REQUIRE(started.get_future().wait_for(30s) == std::future_status::ready);
    BOOST_CHECK(!running.prep->TryMove(NetMessagePrep::TAKEN)); // it runs
    threaded.Taken(*running.prep);
    BOOST_CHECK_EQUAL(threaded.Held(), 0U);
    BOOST_CHECK(!running.job.expired());
    release_once();
    threaded.Stop(); // returns once the run has ended
    BOOST_CHECK(had_data.load());
    BOOST_CHECK_EQUAL(running.prep->state.load(), NetMessagePrep::DONE);
    BOOST_CHECK(running.job.expired());
}

BOOST_AUTO_TEST_CASE(lowering_the_total_drops_from_whoever_holds_the_most_each_time)
{
    // Three peers hold 6, 4 and 2 waiting jobs under a total of 12. Lowered to 6 with no peer above its own limit,
    // the six drops come from the peer holding the most at each step, its newest first, ties to the smaller id:
    // 1, 1, then 1 and 2 in turn (4, 4), and so on down to 2, 2, 2.
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(8, 12);
    std::map<int64_t, std::vector<std::shared_ptr<PreverifyJob>>> jobs;
    uint64_t key{0};
    for (const auto& [peer, count] : std::vector<std::pair<int64_t, int>>{{1, 6}, {2, 4}, {3, 2}}) {
        for (int i = 0; i < count; ++i) {
            jobs[peer].push_back(Job(peer, ++key));
            BOOST_REQUIRE(verifier.Offer(jobs[peer].back()) == Offer::QUEUED);
        }
    }
    verifier.SetLimits(8, 6);
    BOOST_CHECK_EQUAL(verifier.Held(), 6U);
    for (int64_t peer : {1, 2, 3}) BOOST_CHECK_EQUAL(verifier.HeldBy(peer), 2U);
    // Each peer kept its two oldest.
    for (const auto& [peer, list] : jobs) {
        for (size_t i = 0; i < list.size(); ++i) {
            BOOST_CHECK_EQUAL(list[i]->State(), i < 2 ? NetMessagePrep::PENDING : NetMessagePrep::DROPPED);
        }
    }
    // The same limits again change nothing; and what is left runs in turn.
    verifier.SetLimits(8, 6);
    BOOST_CHECK_EQUAL(verifier.Held(), 6U);
    size_t ran{0};
    while (verifier.RunOne()) ++ran;
    BOOST_CHECK_EQUAL(ran, 6U);
}

BOOST_AUTO_TEST_CASE(lowered_limits_drop_the_waiting_excess_and_take_nothing_above_them)
{
    ReportPreverifier verifier{[](PreverifyJob&) {}, /*threaded=*/false};
    verifier.SetLimits(8, 8);
    std::vector<std::shared_ptr<PreverifyJob>> a, b;
    for (uint64_t i = 1; i <= 4; ++i) a.push_back(Job(1, i));
    for (uint64_t i = 5; i <= 6; ++i) b.push_back(Job(2, i));
    for (const auto& job : a) BOOST_CHECK(verifier.Offer(job) == Offer::QUEUED);
    for (const auto& job : b) BOOST_CHECK(verifier.Offer(job) == Offer::QUEUED);
    // Peer 1 above its new limit loses its newest waiting jobs first; then the total comes down from the peer
    // holding the most.
    verifier.SetLimits(2, 3);
    BOOST_CHECK_EQUAL(a[3]->State(), NetMessagePrep::DROPPED);
    BOOST_CHECK_EQUAL(a[2]->State(), NetMessagePrep::DROPPED);
    BOOST_CHECK_EQUAL(verifier.Held(), 3U);
    BOOST_CHECK_EQUAL(verifier.HeldBy(1) + verifier.HeldBy(2), 3U);
    BOOST_CHECK_LE(verifier.HeldBy(1), 2U);
    BOOST_CHECK_LE(verifier.HeldBy(2), 2U);

    // Done jobs cannot be dropped: above the lowered total nothing is taken in, not even in exchange.
    ReportPreverifier done{[](PreverifyJob&) {}, /*threaded=*/false};
    done.SetLimits(4, 4);
    std::vector<std::shared_ptr<PreverifyJob>> held;
    for (uint64_t i = 1; i <= 4; ++i) {
        held.push_back(Job(static_cast<int64_t>(i), i));
        BOOST_CHECK(done.Offer(held.back()) == Offer::QUEUED);
    }
    while (done.RunOne()) {}
    done.SetLimits(4, 2);
    BOOST_CHECK_EQUAL(done.Held(), 4U);
    BOOST_CHECK(done.Offer(Job(9, 10)) == Offer::QUEUE_FULL);
    done.Taken(*held[0]->prep);
    done.Taken(*held[1]->prep);
    BOOST_CHECK(done.Offer(Job(9, 11)) == Offer::QUEUE_FULL); // at the total, and no waiting job to make room
    done.Taken(*held[2]->prep);
    BOOST_CHECK(done.Offer(Job(9, 12)) == Offer::QUEUED);
}

BOOST_AUTO_TEST_CASE(the_thread_runs_every_job_and_stops_with_the_verifier)
{
    std::atomic<int> runs{0};
    std::vector<std::shared_ptr<PreverifyJob>> jobs;
    {
        ReportPreverifier verifier{[&](PreverifyJob&) { ++runs; }, /*threaded=*/true};
        verifier.SetLimits(8, 64);
        for (uint64_t i = 1; i <= 6; ++i) {
            jobs.push_back(Job(static_cast<int64_t>(i % 3), i));
            BOOST_CHECK(verifier.Offer(jobs.back()) == Offer::QUEUED);
        }
        for (int i = 0; i < 500 && runs.load() < 6; ++i) std::this_thread::sleep_for(10ms);
        BOOST_CHECK_EQUAL(runs.load(), 6);
    }
    for (const auto& job : jobs) BOOST_CHECK_EQUAL(job->State(), NetMessagePrep::DONE);
}

// ---- this node's own observation, as the window reads it ----

namespace {
struct FakeChain {
    std::vector<std::unique_ptr<CBlockIndex>> blocks;
    CBlockIndex* Add(CBlockIndex* prev)
    {
        auto block = std::make_unique<CBlockIndex>();
        block->pprev = prev;
        block->nHeight = prev != nullptr ? prev->nHeight + 1 : 0;
        block->BuildSkip();
        blocks.push_back(std::move(block));
        return blocks.back().get();
    }
    std::vector<CBlockIndex*> Line(CBlockIndex* from, int length)
    {
        std::vector<CBlockIndex*> out{from};
        for (int i = 0; i < length; ++i) out.push_back(Add(out.back()));
        return out;
    }
};
} // namespace

BOOST_AUTO_TEST_CASE(the_observation_mark_follows_the_notified_tips)
{
    FakeChain chain;
    const auto main = chain.Line(chain.Add(nullptr), 20); // main[h] is at height h
    const uint256 base{ArithToUint256(arith_uint256{24})};
    dsl::ObservationMark mark{main[17]};
    // 18 is notified, and the tick of its event emits the reports and marks the observation. As the first
    // notification, it asks for the arming, which its caller queues ahead of the event.
    BOOST_CHECK(mark.OnSyncTip(main[18]));
    mark.Arm();
    const auto generation = mark.EventBegin(main[18]);
    BOOST_REQUIRE(generation);
    BOOST_CHECK(!mark.Done(1, base)); // nothing before the mark
    mark.Publish(*generation, 1, base);
    BOOST_CHECK(mark.Done(1, base));
    BOOST_CHECK(!mark.Done(2, base));
    BOOST_CHECK(!mark.Done(1, uint256::ONE));
    // The active tip at the notified one or past it (connected, not yet notified) is no rewind; behind it is one.
    BOOST_CHECK(mark.NotRewinding(main[18]));
    BOOST_CHECK(mark.NotRewinding(main[19]));
    BOOST_CHECK(!mark.NotRewinding(main[17]));
    // Advancing keeps the mark, and so does the same tip notified again (an off-chain block invalidated).
    mark.OnSyncTip(main[19]);
    const uint64_t kept{mark.Generation()};
    mark.OnSyncTip(main[19]);
    BOOST_CHECK_EQUAL(mark.Generation(), kept);
    BOOST_CHECK(mark.Done(1, base));
    // A rewind clears it, though the base stayed; a branch switch at a greater height does too.
    mark.OnSyncTip(main[17]);
    BOOST_CHECK(!mark.Done(1, base));
    BOOST_CHECK_EQUAL(mark.Generation(), kept + 1);
    const auto side = chain.Line(main[16], 3);
    mark.OnSyncTip(side.back());
    BOOST_CHECK_EQUAL(mark.Generation(), kept + 2);
    // A tick whose event was queued before either cannot mark: the generation it gets is its own event's.
    mark.Publish(kept, 1, base);
    BOOST_CHECK(!mark.Done(1, base));
}

BOOST_AUTO_TEST_CASE(a_tick_gets_the_generation_of_its_own_event)
{
    FakeChain chain;
    const auto main = chain.Line(chain.Add(nullptr), 20);
    const uint256 base{ArithToUint256(arith_uint256{24})};
    dsl::ObservationMark mark{main[17]};
    // 18 is notified; before its event runs, a rewind to 17, and 18 connected again.
    BOOST_CHECK(mark.OnSyncTip(main[18]));
    BOOST_CHECK(!mark.OnSyncTip(main[17])); // only the first notification asks for the arming
    mark.OnSyncTip(main[18]);
    mark.Arm(); // queued with the first notification, ahead of its event
    const auto first = mark.EventBegin(main[18]); // the first event for 18
    BOOST_REQUIRE(first);
    mark.Publish(*first, 1, base);
    BOOST_CHECK(!mark.Done(1, base));
    const auto rewound = mark.EventBegin(main[17]);
    const auto again = mark.EventBegin(main[18]);
    BOOST_REQUIRE(rewound && again);
    BOOST_CHECK_EQUAL(*again, *rewound);
    BOOST_CHECK_NE(*again, *first);
    mark.Publish(*again, 1, base);
    BOOST_CHECK(mark.Done(1, base));
    // Every event takes its own entry, tick or not (none follows during the initial block download): nothing waits.
    mark.OnSyncTip(main[19]);
    mark.OnSyncTip(main[20]);
    BOOST_CHECK(mark.EventBegin(main[19]));
    BOOST_CHECK(mark.EventBegin(main[20]));
    BOOST_CHECK(!mark.EventBegin(main[20])); // no notification left for another
}

BOOST_AUTO_TEST_CASE(an_event_whose_entry_was_pushed_out_gets_no_generation_and_takes_no_other)
{
    FakeChain chain;
    const auto main = chain.Line(chain.Add(nullptr), 20);
    const uint256 base{ArithToUint256(arith_uint256{24})};
    dsl::ObservationMark mark{main[17]};
    // 18 is notified, then a rewind to 17, then 18 connected again -- and so many notifications after it, before
    // any event runs, that the first two entries are pushed out. The entry now in front is the reconnected 18's.
    mark.OnSyncTip(main[18]);
    const uint64_t before{mark.Generation()};
    mark.OnSyncTip(main[17]);
    mark.OnSyncTip(main[18]);
    const uint64_t after{mark.Generation()};
    BOOST_REQUIRE_NE(after, before);
    for (size_t i = 0; i + 1 < dsl::ObservationMark::MAX_PENDING_EVENTS; ++i) mark.OnSyncTip(main[18]);
    mark.Arm(); // queued with the first notification, ahead of its event
    // The first event, of the 18 from before the rewind: its entry is gone, and it does not take the reconnected
    // 18's, although the block is the same.
    BOOST_CHECK(!mark.EventBegin(main[18]));
    // The rewind's event: gone as well.
    BOOST_CHECK(!mark.EventBegin(main[17]));
    // The reconnected 18's event gets its own generation, the current one, and its tick may mark.
    const auto own = mark.EventBegin(main[18]);
    BOOST_REQUIRE(own);
    BOOST_CHECK_EQUAL(*own, after);
    mark.Publish(*own, 1, base);
    BOOST_CHECK(mark.Done(1, base));
    // Every event after it finds its own entry, to the last one.
    for (size_t i = 0; i + 1 < dsl::ObservationMark::MAX_PENDING_EVENTS; ++i) {
        const auto later = mark.EventBegin(main[18]);
        BOOST_REQUIRE_MESSAGE(later, "event " << i);
        BOOST_CHECK_EQUAL(*later, after);
    }
    BOOST_CHECK(!mark.EventBegin(main[18]));
}

BOOST_AUTO_TEST_CASE(an_event_without_a_notification_takes_no_entry)
{
    FakeChain chain;
    const auto main = chain.Line(chain.Add(nullptr), 20);
    dsl::ObservationMark mark{main[17]};
    // An event before any notification: nothing is on the line for it.
    BOOST_CHECK(!mark.EventBegin(main[17]));
    // Armed, with a notification on the line: an event of another tip takes nothing, and the entry stays for its own.
    BOOST_CHECK(mark.OnSyncTip(main[18]));
    mark.Arm();
    BOOST_CHECK(!mark.EventBegin(main[17]));
    const auto own = mark.EventBegin(main[18]);
    BOOST_REQUIRE(own);
    BOOST_CHECK_EQUAL(*own, mark.Generation());
}

BOOST_AUTO_TEST_CASE(events_queued_before_the_first_notification_take_nothing_even_for_its_block)
{
    FakeChain chain;
    const auto main = chain.Line(chain.Add(nullptr), 20);
    // An event for 18 was queued before this object listened (a queued event reaches every subscriber there is when
    // it runs). Then 18 is notified to it: the first notification, whose arming is queued behind that event.
    dsl::ObservationMark mark{main[17]};
    BOOST_CHECK(mark.OnSyncTip(main[18]));
    mark.OnSyncTip(main[19]);
    // The old event runs first: the same block, but not its notification. It takes nothing and gets no generation.
    BOOST_CHECK(!mark.EventBegin(main[18]));
    mark.Arm();
    // The notified tips' events get their entries, each its own.
    const auto own = mark.EventBegin(main[18]);
    BOOST_REQUIRE(own);
    BOOST_CHECK_EQUAL(*own, mark.Generation());
    BOOST_CHECK(mark.EventBegin(main[19]));
    BOOST_CHECK(!mark.EventBegin(main[19]));

    // With entries pushed out before the arming, an old event does not spend the count of lost ones either.
    dsl::ObservationMark full{main[17]};
    BOOST_CHECK(full.OnSyncTip(main[18]));
    for (size_t i = 0; i < dsl::ObservationMark::MAX_PENDING_EVENTS; ++i) full.OnSyncTip(main[18]); // one pushed out
    BOOST_CHECK(!full.EventBegin(main[18])); // the old event
    full.Arm();
    BOOST_CHECK(!full.EventBegin(main[18])); // the first notification's event: its entry was the one pushed out
    for (size_t i = 0; i < dsl::ObservationMark::MAX_PENDING_EVENTS; ++i) {
        BOOST_REQUIRE_MESSAGE(full.EventBegin(main[18]), "event " << i);
    }
    BOOST_CHECK(!full.EventBegin(main[18]));
}

// ---- the overtake policy ----

BOOST_AUTO_TEST_CASE(the_overtake_policy)
{
    for (const char* type : {NetMsgType::TX, NetMsgType::ISDLOCK, NetMsgType::QSIGSESANN, NetMsgType::QSIGSHARESINV,
                             NetMsgType::QGETSIGSHARES, NetMsgType::QBSIGSHARES, NetMsgType::QSIGSHARE,
                             NetMsgType::QSIGREC, NetMsgType::PONG}) {
        BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(type)), NetMessagePollGate::ALWAYS);
    }
    for (const char* type : {NetMsgType::PING, NetMsgType::BLOCK, NetMsgType::HEADERS, NetMsgType::CMPCTBLOCK,
                             NetMsgType::POSERESPONSE, NetMsgType::POSECOMMIT, NetMsgType::ADDR}) {
        BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(type)), NetMessagePollGate::NEVER);
    }
    const auto is = Inventory({MSG_TX, MSG_ISDLOCK, MSG_QUORUM_RECOVERED_SIG});
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, is)), NetMessagePollGate::ALWAYS);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::NOTFOUND, is)), NetMessagePollGate::ALWAYS);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::GETDATA, is)), NetMessagePollGate::IF_SEND_ROOM);
    // A mixed, empty, unreadable or oversized inventory never overtakes.
    const auto mixed = Inventory({MSG_TX, MSG_BLOCK});
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, mixed)), NetMessagePollGate::NEVER);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::GETDATA, mixed)), NetMessagePollGate::NEVER);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, Inventory({}))), NetMessagePollGate::NEVER);
    auto truncated = is;
    truncated.pop_back();
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, truncated)), NetMessagePollGate::NEVER);
    CDataStream oversized{SER_NETWORK, PROTOCOL_VERSION};
    WriteCompactSize(oversized, MAX_INV_SZ + 1);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, {UCharCast(oversized.data()), UCharCast(oversized.data()) + oversized.size()})),
                      NetMessagePollGate::NEVER);
    // Each inventory type at its own handler's limit: NOTFOUND takes 116 entries without a score, the 117th scores.
    const auto txs = [](size_t n) { return Inventory(std::vector<uint32_t>(n, MSG_TX)); };
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::NOTFOUND, txs(116))), NetMessagePollGate::ALWAYS);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::NOTFOUND, txs(117))), NetMessagePollGate::NEVER);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, txs(117))), NetMessagePollGate::ALWAYS);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::GETDATA, txs(117))), NetMessagePollGate::IF_SEND_ROOM);
    // A count the handler's read refuses (not canonical: 253 and then a value below 253) never overtakes.
    std::vector<unsigned char> noncanonical{253, 1, 0};
    const auto one = Inventory({MSG_TX});
    noncanonical.insert(noncanonical.end(), one.begin() + 1, one.end());
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, noncanonical)), NetMessagePollGate::NEVER);
    // Bytes after the last entry are not read, as the handler does not read them.
    auto trailing = one;
    trailing.push_back(0x77);
    BOOST_CHECK_EQUAL(DSLOvertakePolicy(Message(NetMsgType::INV, trailing)), NetMessagePollGate::ALWAYS);
}

BOOST_AUTO_TEST_CASE(the_report_preverification_job_touches_nothing_but_its_own_data)
{
    // The job's test wait runs on its own: another thread's report admission is not held up by it.
    CBLSSecretKey key;
    key.MakeNewKey();
    dsl::CPoSeServiceReport report;
    report.nEpoch = 3;
    report.targetProTxHash = uint256::ONE;
    report.sentinelProTxHash = ArithToUint256(arith_uint256{2});
    report.status = static_cast<uint8_t>(dsl::ServiceStatus::ONLINE);
    const uint256 base{ArithToUint256(arith_uint256{99})};
    report.Sign(key, base);
    CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
    stream << report;
    PreverifyJob job;
    job.payload.assign(UCharCast(stream.data()), UCharCast(stream.data()) + stream.size());
    job.operator_key = key.GetPublicKey();
    job.base_hash = base;
    dsl::ClearReportSignatureCacheForTest();
    std::optional<TestDelay> slow{std::in_place, 1500ms};
    const auto start = std::chrono::steady_clock::now();
    std::thread worker{[&] { dsl::PreverifyReport(job); }};
    // Meanwhile this thread checks the same signature itself, through the same cache: it does not wait for the job.
    std::this_thread::sleep_for(100ms);
    const bool valid{report.VerifySig(key.GetPublicKey(), base)};
    const auto checked = std::chrono::steady_clock::now() - start;
    worker.join();
    slow.reset();
    BOOST_CHECK(valid);
    BOOST_CHECK_MESSAGE(checked < 1000ms, "a check on another thread waited for the job's test wait");
    BOOST_CHECK(std::chrono::steady_clock::now() - start >= 1500ms);
    // An unreadable payload throws (the verifier counts the job done, with the cache unchanged).
    PreverifyJob unreadable;
    unreadable.payload = std::vector<unsigned char>(10, 0xff);
    BOOST_CHECK_THROW(dsl::PreverifyReport(unreadable), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

// ---- the receive queue ----

namespace {
struct QueueSetup : TestingSetup {
    ConnmanTestMsg connman{0x1337, 0x1337, *m_node.addrman, *m_node.netgroupman};
    std::unique_ptr<CNode> node{MakeTestPeer(7)};
    std::chrono::steady_clock::time_point now{std::chrono::steady_clock::time_point{} + 1000s};

    QueueSetup() { SetQueueClockMockForTest(now); }
    ~QueueSetup() { SetQueueClockMockForTest(std::nullopt); }

    void Advance(std::chrono::steady_clock::duration by)
    {
        now += by;
        SetQueueClockMockForTest(now);
    }
    void Push(const std::string& type, std::vector<unsigned char> payload = {})
    {
        CSerializedNetMsg msg;
        msg.m_type = type;
        msg.data = std::move(payload);
        BOOST_REQUIRE(connman.ReceiveMsgFrom(*node, std::move(msg)));
    }
    //! A report, given a preparation in `state` (none for nullopt) by a scan: what the queue keeps of a job.
    std::shared_ptr<NetMessagePrep> PushReport(std::optional<uint8_t> state = NetMessagePrep::PENDING, size_t size = 3)
    {
        Push(NetMsgType::POSEREPORT, std::vector<unsigned char>(size, 0x42));
        const auto batch = node->PeekUnscannedReports(16, size);
        BOOST_REQUIRE(!batch.items.empty());
        std::shared_ptr<NetMessagePrep> job;
        std::vector<std::pair<uint64_t, std::shared_ptr<NetMessagePrep>>> preps;
        if (state) {
            job = std::make_shared<NetMessagePrep>();
            job->state = *state;
            preps.emplace_back(batch.items.back().seq, job);
        }
        node->MarkReportsScanned(batch.items.size(), preps);
        return job;
    }
    NetMessagePollGate Gate(bool open, bool getdata_ok = false) const
    {
        NetMessagePollGate gate;
        gate.watermark = node->QueueWatermark();
        gate.window_open = open;
        gate.getdata_ok = getdata_ok;
        gate.young = 5s;
        gate.classify = DSLOvertakePolicy;
        return gate;
    }
    //! The type of the message taken, or "" if none.
    std::string Next(const NetMessagePollGate* gate = nullptr, NetMessagePollOutcome* outcome = nullptr)
    {
        auto polled = node->PollMessage(gate, outcome);
        return polled ? polled->first.m_type : std::string{};
    }
    std::string NextOpen(NetMessagePollOutcome* outcome = nullptr)
    {
        const auto gate = Gate(/*open=*/true);
        return Next(&gate, outcome);
    }
};
} // namespace

BOOST_AUTO_TEST_SUITE(dsl_preverify_queue_tests)

BOOST_FIXTURE_TEST_CASE(without_a_gate_the_two_lists_keep_the_order_of_arrival, QueueSetup)
{
    Push(NetMsgType::PING, {1, 2, 3, 4, 5, 6, 7, 8});
    PushReport();
    Push(NetMsgType::TX);
    PushReport();
    Push(NetMsgType::PONG, {1, 2, 3, 4, 5, 6, 7, 8});
    for (const char* type : {NetMsgType::PING, NetMsgType::POSEREPORT, NetMsgType::TX, NetMsgType::POSEREPORT, NetMsgType::PONG}) {
        BOOST_CHECK_EQUAL(Next(), type);
    }
    BOOST_CHECK_EQUAL(Next(), "");
    // The same with a gate whose window is shut.
    Push(NetMsgType::PING, {1, 2, 3, 4, 5, 6, 7, 8});
    PushReport();
    Push(NetMsgType::TX);
    const auto shut = Gate(/*open=*/false);
    BOOST_CHECK_EQUAL(Next(&shut), NetMsgType::PING);
    BOOST_CHECK_EQUAL(Next(&shut), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(Next(&shut), NetMsgType::TX);
}

BOOST_FIXTURE_TEST_CASE(in_the_window_an_instantsend_message_overtakes_a_young_unverified_report, QueueSetup)
{
    PushReport();
    Push(NetMsgType::TX);
    NetMessagePollOutcome outcome;
    BOOST_CHECK_EQUAL(NextOpen(&outcome), NetMsgType::TX);
    BOOST_CHECK(outcome.overtook);
    BOOST_CHECK_EQUAL(outcome.head_state, NetMessagePrep::PENDING);
    // Only the report is left: the gate holds it, and nothing comes out of the peer.
    outcome = {};
    BOOST_CHECK_EQUAL(NextOpen(&outcome), "");
    BOOST_CHECK(outcome.gate_held);
    // At exactly five seconds it is no longer young: in order.
    Advance(5s);
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::POSEREPORT);
}

BOOST_FIXTURE_TEST_CASE(a_running_job_is_unverified_as_well, QueueSetup)
{
    PushReport(NetMessagePrep::RUNNING);
    Push(NetMsgType::ISDLOCK);
    NetMessagePollOutcome outcome;
    BOOST_CHECK_EQUAL(NextOpen(&outcome), NetMsgType::ISDLOCK);
    BOOST_CHECK_EQUAL(outcome.head_state, NetMessagePrep::RUNNING);
}

BOOST_FIXTURE_TEST_CASE(a_report_the_queue_hands_out_is_taken_before_its_job_can_start, QueueSetup)
{
    int runs{0};
    ReportPreverifier verifier{[&](PreverifyJob&) { ++runs; }, /*threaded=*/false};
    verifier.SetLimits(8, 64);
    // A report whose queue entry keeps the state of a job the verifier holds, as a scan leaves it.
    const auto queue_with = [&](const std::shared_ptr<PreverifyJob>& job) {
        BOOST_REQUIRE(verifier.Offer(job) == Offer::QUEUED);
        Push(NetMsgType::POSEREPORT, std::vector<unsigned char>(3, 0x42));
        const auto batch = node->PeekUnscannedReports(16, 3);
        BOOST_REQUIRE_EQUAL(batch.items.size(), 1U);
        node->MarkReportsScanned(1, {{batch.items.back().seq, job->prep}});
    };
    // The job has not started: the poll that hands the report out takes it (PENDING -> TAKEN) before it returns, so
    // the job never starts.
    const auto pending = Job(7, 1);
    queue_with(pending);
    auto polled = node->PollMessage();
    BOOST_REQUIRE(polled);
    BOOST_REQUIRE(polled->first.m_prep);
    BOOST_CHECK_EQUAL(polled->first.m_prep->state.load(), NetMessagePrep::TAKEN);
    BOOST_CHECK(!verifier.RunOne());
    BOOST_CHECK_EQUAL(runs, 0);
    verifier.Taken(*polled->first.m_prep);
    BOOST_CHECK_EQUAL(verifier.Held(), 0U);
    // The job ran first: the poll hands the report out all the same, and the state stays the job's.
    const auto ran = Job(7, 2);
    queue_with(ran);
    BOOST_REQUIRE(verifier.RunOne());
    polled = node->PollMessage();
    BOOST_REQUIRE(polled);
    BOOST_CHECK_EQUAL(polled->first.m_prep->state.load(), NetMessagePrep::DONE);
    BOOST_CHECK_EQUAL(runs, 1);
}

BOOST_FIXTURE_TEST_CASE(pong_ping_and_ping_pong, QueueSetup)
{
    // PONG, PING behind the report: PONG, the report, PING.
    PushReport();
    Push(NetMsgType::PONG, {1, 2, 3, 4, 5, 6, 7, 8});
    Push(NetMsgType::PING, {1, 2, 3, 4, 5, 6, 7, 8});
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::PONG);
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::PING);
    // PING, PONG: the report, PING, PONG -- a message that may not overtake keeps everything behind it in order.
    PushReport();
    Push(NetMsgType::PING, {1, 2, 3, 4, 5, 6, 7, 8});
    Push(NetMsgType::PONG, {1, 2, 3, 4, 5, 6, 7, 8});
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::PING);
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::PONG);
}

BOOST_FIXTURE_TEST_CASE(a_message_that_came_after_the_snapshot_does_not_overtake, QueueSetup)
{
    PushReport();
    const auto snapshot = Gate(/*open=*/true);
    Push(NetMsgType::TX);
    NetMessagePollOutcome outcome;
    BOOST_CHECK_EQUAL(Next(&snapshot, &outcome), NetMsgType::POSEREPORT);
    BOOST_CHECK(outcome.stale);
    BOOST_CHECK(!outcome.overtook);
    BOOST_CHECK(!outcome.gate_held);
    // A report that came after the snapshot is not held either.
    const auto early = Gate(/*open=*/true);
    PushReport();
    outcome = {};
    BOOST_CHECK_EQUAL(Next(&early, &outcome), NetMsgType::TX);
    outcome = {};
    BOOST_CHECK_EQUAL(Next(&early, &outcome), NetMsgType::POSEREPORT);
    BOOST_CHECK(outcome.stale);
}

BOOST_FIXTURE_TEST_CASE(a_finished_old_or_unprepared_report_goes_in_order_and_is_never_held, QueueSetup)
{
    for (const std::optional<uint8_t> state : {std::optional<uint8_t>{NetMessagePrep::DONE},
                                               std::optional<uint8_t>{NetMessagePrep::TAKEN},
                                               std::optional<uint8_t>{NetMessagePrep::DROPPED},
                                               std::optional<uint8_t>{}}) {
        PushReport(state);
        Push(NetMsgType::TX);
        BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::POSEREPORT);
        BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::TX);
        PushReport(state);
        NetMessagePollOutcome outcome;
        BOOST_CHECK_EQUAL(NextOpen(&outcome), NetMsgType::POSEREPORT);
        BOOST_CHECK(!outcome.gate_held);
    }
    // Pending, but the window shut: in order, never held.
    PushReport();
    const auto shut = Gate(/*open=*/false);
    BOOST_CHECK_EQUAL(Next(&shut), NetMsgType::POSEREPORT);
}

BOOST_FIXTURE_TEST_CASE(the_job_finishing_between_snapshot_and_poll_restores_the_order, QueueSetup)
{
    auto job = PushReport();
    Push(NetMsgType::TX);
    const auto snapshot = Gate(/*open=*/true);
    job->state = NetMessagePrep::DONE; // after the snapshot, before the poll
    BOOST_CHECK_EQUAL(Next(&snapshot), NetMsgType::POSEREPORT);
    // The same for a report that turns five seconds old in between.
    PushReport();
    Push(NetMsgType::TX);
    const auto later = Gate(/*open=*/true);
    Advance(5s);
    BOOST_CHECK_EQUAL(Next(&later), NetMsgType::TX); // the TX that arrived first
    BOOST_CHECK_EQUAL(Next(&later), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(Next(&later), NetMsgType::TX);
}

BOOST_FIXTURE_TEST_CASE(a_getdata_overtakes_only_with_room_to_send, QueueSetup)
{
    const auto inv = Inventory({MSG_TX});
    PushReport();
    Push(NetMsgType::GETDATA, inv);
    const auto full = Gate(/*open=*/true, /*getdata_ok=*/false);
    BOOST_CHECK_EQUAL(Next(&full), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(Next(&full), NetMsgType::GETDATA);
    PushReport();
    Push(NetMsgType::GETDATA, inv);
    const auto room = Gate(/*open=*/true, /*getdata_ok=*/true);
    BOOST_CHECK_EQUAL(Next(&room), NetMsgType::GETDATA);
}

BOOST_FIXTURE_TEST_CASE(the_snapshot_reads_content_only_for_a_head_report_that_may_be_overtaken, QueueSetup)
{
    int asked{0};
    const auto counting = [&](const CNetMessage& msg) {
        ++asked;
        return DSLOvertakePolicy(msg);
    };
    const auto inv = Inventory({MSG_TX});
    // No report, a report without a job, a finished one, an old one, or one behind the other head: it may not wait,
    // and that is known without reading any content.
    Push(NetMsgType::INV, inv);
    BOOST_CHECK(!node->HeadReportMayWait(5s));
    PushReport(std::nullopt);
    BOOST_CHECK(!node->HeadReportMayWait(5s)); // no job (and the INV arrived first)
    BOOST_CHECK_EQUAL(Next(), NetMsgType::INV);
    Push(NetMsgType::INV, inv);
    BOOST_CHECK(!node->HeadReportMayWait(5s)); // no job
    BOOST_CHECK_EQUAL(Next(), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(Next(), NetMsgType::INV);
    PushReport(NetMessagePrep::DONE);
    Push(NetMsgType::INV, inv);
    BOOST_CHECK(!node->HeadReportMayWait(5s));
    BOOST_CHECK_EQUAL(Next(), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(Next(), NetMsgType::INV);
    PushReport();
    Push(NetMsgType::INV, inv);
    Advance(5s);
    BOOST_CHECK(!node->HeadReportMayWait(5s));
    BOOST_CHECK_EQUAL(Next(), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(Next(), NetMsgType::INV);
    // A young head report with an unfinished job, in front: it may wait. The other head's content is read only when
    // the caller asks (it does once it knows the window is open), and only if that head was queued at the watermark.
    PushReport();
    const uint64_t before{node->QueueWatermark()};
    Push(NetMsgType::GETDATA, inv);
    BOOST_CHECK(node->HeadReportMayWait(5s));
    BOOST_CHECK_EQUAL(asked, 0);
    BOOST_CHECK_EQUAL(node->ClassifyMainHead(before, counting), -1); // it came after that watermark: not read
    BOOST_CHECK_EQUAL(asked, 0);
    BOOST_CHECK_EQUAL(node->ClassifyMainHead(node->QueueWatermark(), counting), NetMessagePollGate::IF_SEND_ROOM);
    BOOST_CHECK_EQUAL(asked, 1);
    // Asked once for a message, then kept: by a later snapshot, and by the poll.
    BOOST_CHECK_EQUAL(node->ClassifyMainHead(node->QueueWatermark(), counting), NetMessagePollGate::IF_SEND_ROOM);
    auto gate = Gate(/*open=*/true, /*getdata_ok=*/true);
    gate.classify = counting;
    BOOST_CHECK_EQUAL(Next(&gate), NetMsgType::GETDATA);
    BOOST_CHECK_EQUAL(asked, 1);
    BOOST_CHECK_EQUAL(Next(), NetMsgType::POSEREPORT);
    // A lone report that may wait: nothing to read.
    PushReport();
    BOOST_CHECK(node->HeadReportMayWait(5s));
    BOOST_CHECK_EQUAL(node->ClassifyMainHead(node->QueueWatermark(), counting), -1);
    BOOST_CHECK_EQUAL(asked, 1);
}

BOOST_FIXTURE_TEST_CASE(an_outcome_used_again_starts_clean, QueueSetup)
{
    PushReport();
    NetMessagePollOutcome outcome;
    BOOST_CHECK_EQUAL(NextOpen(&outcome), "");
    BOOST_CHECK(outcome.gate_held);
    // The same object for a later poll that takes in arrival order: the earlier hold does not stop it.
    Advance(5s);
    BOOST_CHECK_EQUAL(NextOpen(&outcome), NetMsgType::POSEREPORT);
    BOOST_CHECK(!outcome.gate_held);
    // And for a poll of an empty queue.
    outcome.gate_held = outcome.overtook = outcome.stale = true;
    outcome.head_seq = 7;
    BOOST_CHECK_EQUAL(Next(nullptr, &outcome), "");
    BOOST_CHECK(!outcome.gate_held && !outcome.overtook && !outcome.stale);
    BOOST_CHECK_EQUAL(outcome.head_seq, 0U);
}

BOOST_FIXTURE_TEST_CASE(the_head_that_arrived_first_goes_first, QueueSetup)
{
    Push(NetMsgType::BLOCK);
    PushReport();
    NetMessagePollOutcome outcome;
    BOOST_CHECK_EQUAL(NextOpen(&outcome), NetMsgType::BLOCK);
    BOOST_CHECK(!outcome.overtook);
    // A message that may not overtake, behind the report, waits for it.
    Push(NetMsgType::BLOCK);
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::POSEREPORT);
    BOOST_CHECK_EQUAL(NextOpen(), NetMsgType::BLOCK);
}

BOOST_FIXTURE_TEST_CASE(the_scan_cursor_and_the_counts_the_budget_reads, QueueSetup)
{
    Push(NetMsgType::POSECOMMIT);
    Push(NetMsgType::POSEREPORT, std::vector<unsigned char>(5, 1));
    Push(NetMsgType::POSEREPORT, std::vector<unsigned char>(4, 2));
    Push(NetMsgType::POSERESPONSE);
    Push(NetMsgType::TX);
    auto batch = node->PeekUnscannedReports(16, 5);
    BOOST_REQUIRE_EQUAL(batch.items.size(), 2U);
    BOOST_CHECK_EQUAL(batch.main_dsl, 2U); // the commitment and the announcement, not the transaction
    BOOST_CHECK_EQUAL(batch.scanned, 0U);
    BOOST_CHECK(batch.items[0].size_ok);
    BOOST_CHECK_EQUAL(batch.items[0].payload.size(), 5U);
    BOOST_CHECK(!batch.items[1].size_ok); // another size: not copied
    BOOST_CHECK(batch.items[1].payload.empty());
    auto job = std::make_shared<NetMessagePrep>();
    node->MarkReportsScanned(1, {{batch.items[0].seq, job}});
    batch = node->PeekUnscannedReports(16, 5);
    BOOST_REQUIRE_EQUAL(batch.items.size(), 1U);
    BOOST_CHECK_EQUAL(batch.scanned, 1U);
    BOOST_CHECK_EQUAL(Next(), NetMsgType::POSECOMMIT);
    batch = node->PeekUnscannedReports(16, 5);
    BOOST_CHECK_EQUAL(batch.main_dsl, 1U);
    // The scanned report leaves with its job; the unscanned one is still at the cursor.
    auto polled = node->PollMessage();
    BOOST_REQUIRE(polled);
    BOOST_CHECK(polled->first.m_prep == job);
    batch = node->PeekUnscannedReports(16, 5);
    BOOST_CHECK_EQUAL(batch.scanned, 0U);
    BOOST_REQUIRE_EQUAL(batch.items.size(), 1U);
    // Taken unscanned, the cursor moves past it; a new report after an empty cursor is found.
    BOOST_CHECK_EQUAL(Next(), NetMsgType::POSEREPORT);
    BOOST_CHECK(node->PeekUnscannedReports(16, 5).items.empty());
    Push(NetMsgType::POSEREPORT, std::vector<unsigned char>(5, 3));
    BOOST_CHECK_EQUAL(node->PeekUnscannedReports(16, 5).items.size(), 1U);
    node->MarkReportsScanned(1, {});
    BOOST_CHECK(node->PeekUnscannedReports(16, 5).items.empty());
    Push(NetMsgType::POSEREPORT, std::vector<unsigned char>(5, 4));
    BOOST_CHECK_EQUAL(node->PeekUnscannedReports(16, 5).items.size(), 1U);
    BOOST_CHECK_EQUAL(node->PeekUnscannedReports(16, 5).scanned, 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ---- a real PeerManager, reports through the receive queue ----

namespace {
constexpr int ACTIVATION_HEIGHT{240}; // the base block of epoch 10
constexpr uint32_t EPOCH{10};
constexpr size_t MN_COUNT{12};
constexpr int UNREADABLE_SCORE{10}; // DSL_MSG_MISBEHAVING_UNREADABLE in net_processing.cpp
constexpr size_t SCAN_MAX{16};      // DSL_PREVERIFY_SCAN_MAX in net_processing.cpp
// DSLHonestEpochCeiling in net_processing.cpp, the jobs one peer may hold: a floor of 64 masternodes
// (DSL_MSG_BUDGET_MIN_MNS), one announcement and nDSLSentinelCount reports each, two commitments
// (DSL_COMMITMENTS_PER_EPOCH).
size_t PerPeerJobLimit(size_t masternodes)
{
    return std::max<size_t>(masternodes, 64) * (1 + static_cast<size_t>(Params().GetConsensus().nDSLSentinelCount)) + 2;
}

//! Holds the validation interface's queue until released: what validation queues meanwhile waits behind it.
class QueueBlocker
{
public:
    QueueBlocker()
    {
        CallFunctionInValidationInterfaceQueue([wait = m_release.get_future().share()] { wait.wait(); });
    }
    ~QueueBlocker() { Release(); }
    void Release()
    {
        if (!m_released) m_release.set_value();
        m_released = true;
    }

private:
    std::promise<void> m_release;
    bool m_released{false};
};

struct PreverifySetup : RegTestingSetup {
    //! The process-wide test settings the constructor changes, put back whenever the fixture goes -- also when its
    //! constructor stops at a failed requirement, and its destructor does not run. The first member: it goes last.
    struct Settings {
        const bool perf{dsl::PerfEnabled()};
        ~Settings()
        {
            SetDSLPreverifyModeForTest(1);
            dsl::SetPerfEnabled(perf);
            dsl::TakePerfSnapshot();
            SetQueueClockMockForTest(std::nullopt);
        }
    } settings;
    ConnmanTestMsg connman{0x1337, 0x1337, *m_node.addrman, *m_node.netgroupman};
    std::unique_ptr<CDeterministicMNManager> dmnman;
    std::unique_ptr<dsl::CPoSeServiceManager> dslman{std::make_unique<dsl::CPoSeServiceManager>()};
    std::unique_ptr<CActiveMasternodeManager> activeman; // only for a fixture that is a masternode itself
    std::unique_ptr<PeerManager> peerman;
    //! The test nodes. Whenever the fixture goes -- also when its constructor stops at a failed requirement, and its
    //! destructor does not run -- the nodes PeerManager knows are finalized and the connection manager lets its own go
    //! (its destructor would finalize them through a message handler the test one does not have): after the
    //! registration below has waited for the callbacks, and while PeerManager is there (declared before this).
    struct Nodes {
        std::unique_ptr<PeerManager>& peerman;
        ConnmanTestMsg& connman;
        std::vector<std::unique_ptr<CNode>> owned; //!< the fixture's own; the connection manager owns the others
        std::vector<CNode*> known;                 //!< initialized in PeerManager, not finalized yet
        CNode& Own(std::unique_ptr<CNode> node)
        {
            owned.push_back(std::move(node));
            return *owned.back();
        }
        void Initialize(CNode& node)
        {
            {
                LOCK(NetEventsInterface::g_msgproc_mutex);
                peerman->InitializeNode(node, NODE_NETWORK);
            }
            known.push_back(&node);
        }
        void Finalize(CNode& node)
        {
            known.erase(std::remove(known.begin(), known.end(), &node), known.end());
            peerman->FinalizeNode(node);
        }
        ~Nodes()
        {
            for (CNode* node : known) peerman->FinalizeNode(*node);
            connman.ClearTestNodes();
        }
    } nodes{peerman, connman};
    //! PeerManager's registration for validation events: undone, and the events under way waited for, whenever the
    //! fixture goes -- also when its constructor stops at a failed requirement -- before the nodes, PeerManager and the
    //! members it uses go (it is declared after them).
    struct Registration {
        CValidationInterface* registered{nullptr};
        void Register(CValidationInterface* callbacks)
        {
            RegisterValidationInterface(callbacks);
            registered = callbacks;
        }
        void Undo()
        {
            if (registered == nullptr) return;
            UnregisterValidationInterface(registered);
            registered = nullptr;
            SyncWithValidationInterfaceQueue();
        }
        ~Registration() { Undo(); }
    } registration;
    std::vector<CBLSSecretKey> operator_keys;
    std::vector<uint256> protx;
    CNode* peer_a{nullptr}; // owned by the nodes
    CNode* peer_b{nullptr}; // owned by the nodes
    const CBlockIndex* base{nullptr};
    CNode* listener{nullptr}; // owned by connman: the relays land here
    std::atomic<bool> interrupt{false};
    Tally tally;
    std::chrono::steady_clock::time_point now{std::chrono::steady_clock::time_point{} + 1000s};
    const size_t mn_count;
    //! The POSEREPORT payloads relayed to each processed peer, kept before its send buffer is emptied (Process).
    std::map<NodeId, std::vector<std::vector<unsigned char>>> sent_reports;

    // mode: the pre-verifier (SetDSLPreverifyModeForTest; 2, the default here, runs jobs only when the test does);
    // blocksonly: PeerManager as under -blocksonly; masternodes: the size of the list; active: this node is the first
    // of them itself.
    explicit PreverifySetup(int mode = 2, bool blocksonly = false, size_t masternodes = MN_COUNT, bool active = false)
        : RegTestingSetup{{"-testactivationheight=dsl@240", "-dip3params=2:2"}}, mn_count{masternodes}
    {
        SetDSLPreverifyModeForTest(mode);
        SetQueueClockMockForTest(now);
        dsl::SetPerfEnabled(false);
        dsl::TakePerfSnapshot();
        for (int height = 1; height < ACTIVATION_HEIGHT; ++height) MineBlock(m_node, CScript{} << OP_TRUE);
        SyncWithValidationInterfaceQueue();
        static_cast<TestChainState&>(m_node.chainman->ActiveChainstate()).JumpOutOfIbd();
        BOOST_REQUIRE(m_node.netfulfilledman->LoadCache(false));
        BOOST_REQUIRE(m_node.mn_metaman->LoadCache(false));
        m_node.mn_sync->SwitchToNextAsset();
        BOOST_REQUIRE(m_node.mn_sync->IsBlockchainSynced());
        dmnman = std::make_unique<CDeterministicMNManager>(m_node.chainman->ActiveChainstate(), *m_node.evodb);
        if (active) {
            // The first masternode of the list, by its operator key (the tick resolves the identity from the key).
            CBLSSecretKey key;
            key.MakeNewKey();
            operator_keys.push_back(key);
            protx.push_back(ArithToUint256(arith_uint256{1}));
            activeman = std::make_unique<CActiveMasternodeManager>(key, connman, dmnman);
        }
        peerman = PeerManager::make(Params(), connman, *m_node.addrman, m_node.banman.get(), *m_node.chainman,
                                    *m_node.mempool, *m_node.mn_metaman, *m_node.mn_sync, *m_node.govman,
                                    *m_node.sporkman, activeman.get(), dmnman, m_node.cj_ctx, m_node.llmq_ctx, dslman,
                                    blocksonly);
        SetDSLPreverifyModeForTest(1);
        peerman->SetBestHeight(ACTIVATION_HEIGHT - 1);
        registration.Register(peerman.get());
        peer_a = &nodes.Own(MakeTestPeer(0));
        peer_b = &nodes.Own(MakeTestPeer(1));
        nodes.Initialize(*peer_a);
        nodes.Initialize(*peer_b);
        listener = MakeTestPeer(9).release();
        connman.AddTestNode(*listener);
        nodes.Initialize(*listener);
        {
            // The base block's event, with its tick, waits until the block's list is installed.
            QueueBlocker blocker;
            MineBlock(m_node, CScript{} << OP_TRUE);
            base = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
            BOOST_REQUIRE_EQUAL(base->nHeight, ACTIVATION_HEIGHT);
            InstallList(base);
        }
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH);
    }

    ~PreverifySetup()
    {
        SyncWithValidationInterfaceQueue();
        registration.Undo();
        // The nodes and the settings go as the members go (Nodes, Settings).
    }

    // One more connected peer, of the given type and permissions; `connected` false: before the handshake ends.
    CNode& AddPeer(NodeId id, ConnectionType type, NetPermissionFlags flags = NetPermissionFlags::None, bool connected = true)
    {
        in_addr address{};
        address.s_addr = htonl(0x01020304 + id);
        CNodeOptions options;
        options.permission_flags = flags;
        auto node = std::make_unique<CNode>(id, nullptr, CAddress{CService{address, 8333}, NODE_NETWORK}, 0, 0, CAddress{},
                                            std::string{}, type, false, std::move(options));
        node->nVersion = PROTOCOL_VERSION;
        node->SetCommonVersion(PROTOCOL_VERSION);
        node->fSuccessfullyConnected = connected;
        CNode& added{nodes.Own(std::move(node))};
        nodes.Initialize(added);
        return added;
    }
    //! One more connected peer that the relays reach as well: the connection manager owns it, as it does the listener.
    CNode& AddRelayPeer(NodeId id)
    {
        CNode* node{MakeTestPeer(id).release()};
        connman.AddTestNode(*node);
        nodes.Initialize(*node);
        return *node;
    }
    // The POSEREPORT payloads relayed to the listener so far.
    std::vector<std::vector<unsigned char>> Relayed() const
    {
        LOCK(listener->cs_vSend);
        std::vector<std::vector<unsigned char>> out;
        for (const CSerializedNetMsg& msg : listener->vSendMsg) {
            if (msg.m_type == NetMsgType::POSEREPORT) out.push_back(msg.data);
        }
        return out;
    }

    //! The list of `count` masternodes (all of the fixture's by default) as the list of block `at`.
    void InstallList(const CBlockIndex* at, size_t count = 0)
    {
        if (count == 0) count = mn_count;
        CDeterministicMNList list{at->GetBlockHash(), at->nHeight, static_cast<uint32_t>(count)};
        for (size_t i = 0; i < count; ++i) {
            if (operator_keys.size() <= i) {
                CBLSSecretKey key;
                key.MakeNewKey();
                operator_keys.push_back(key);
                protx.push_back(ArithToUint256(arith_uint256{i + 1}));
            }
            auto mn = std::make_shared<CDeterministicMN>(i);
            mn->proTxHash = protx[i];
            mn->collateralOutpoint = COutPoint{mn->proTxHash, 0};
            auto state = std::make_shared<CDeterministicMNState>();
            state->nRegisteredHeight = 1;
            state->UpdateConfirmedHash(mn->proTxHash, at->GetBlockHash());
            CKey owner_key;
            owner_key.MakeNewKey(true);
            state->keyIDOwner = owner_key.GetPubKey().GetID();
            state->pubKeyOperator.Set(operator_keys[i].GetPublicKey(), /*specificLegacyScheme=*/false);
            mn->pdmnState = state;
            list.AddMN(mn);
        }
        m_node.evodb->Write(std::make_pair(CDeterministicMNManager::ListSnapshotDbKey(), at->GetBlockHash()), list);
        dmnman->UpdatedBlockTip(at);
        BOOST_REQUIRE_EQUAL(dmnman->GetListForBlock(at).GetAllMNsCount(), count);
    }

    std::vector<uint256> Sentinels(const uint256& target, const CBlockIndex* at = nullptr) const
    {
        const CBlockIndex* block{at != nullptr ? at : base};
        return dsl::CalcSentinelsForMN(dmnman->GetListForBlock(block), target, block->GetBlockHash(),
                                       static_cast<size_t>(Params().GetConsensus().nDSLSentinelCount));
    }
    uint256 Outsider(const uint256& target) const
    {
        const auto sentinels = Sentinels(target);
        for (const auto& hash : protx) {
            if (hash != target && std::find(sentinels.begin(), sentinels.end(), hash) == sentinels.end()) return hash;
        }
        BOOST_FAIL("every masternode is a sentinel of the target");
        return {};
    }
    const CBLSSecretKey& KeyOf(const uint256& id) const
    {
        const auto it = std::find(protx.begin(), protx.end(), id);
        BOOST_REQUIRE(it != protx.end());
        return operator_keys.at(static_cast<size_t>(it - protx.begin()));
    }
    dsl::CPoSeServiceReport Report(const uint256& target, const uint256& sentinel,
                                   dsl::ServiceStatus status = dsl::ServiceStatus::ONLINE, const CBlockIndex* at = nullptr) const
    {
        const CBlockIndex* block{at != nullptr ? at : base};
        dsl::CPoSeServiceReport report;
        report.nEpoch = static_cast<uint32_t>(block->nHeight / Params().GetConsensus().nDSLEpochInterval);
        report.targetProTxHash = target;
        report.sentinelProTxHash = sentinel;
        report.status = static_cast<uint8_t>(status);
        report.Sign(KeyOf(sentinel), block->GetBlockHash());
        return report;
    }
    // Every report the epoch whose base is `at` assigned: (target, sentinel) pairs over all targets.
    std::vector<std::pair<uint256, uint256>> AssignedAt(const CBlockIndex* at) const
    {
        std::vector<std::pair<uint256, uint256>> out;
        for (const auto& target : protx) {
            for (const auto& sentinel : Sentinels(target, at)) out.emplace_back(target, sentinel);
        }
        return out;
    }
    // On to the base block of the next epoch, with its list (of `count` masternodes; all of them by default).
    const CBlockIndex* AdvanceToNextEpoch(size_t count = 0)
    {
        const int next_base{ACTIVATION_HEIGHT + Params().GetConsensus().nDSLEpochInterval};
        MineTo(next_base - 1);
        const CBlockIndex* next{nullptr};
        {
            QueueBlocker blocker; // as for the first base block
            MineBlock(m_node, CScript{} << OP_TRUE);
            next = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
            BOOST_REQUIRE_EQUAL(next->nHeight, next_base);
            InstallList(next, count);
        }
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH + 1);
        return next;
    }
    static std::vector<unsigned char> Wire(const dsl::CPoSeServiceReport& report)
    {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << report;
        return {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()};
    }
    //! Every report the epoch assigned: (target, sentinel) pairs over all targets.
    std::vector<std::pair<uint256, uint256>> Assigned() const
    {
        std::vector<std::pair<uint256, uint256>> out;
        for (const auto& target : protx) {
            for (const auto& sentinel : Sentinels(target)) out.emplace_back(target, sentinel);
        }
        return out;
    }

    void Queue(CNode& from, const std::string& type, std::vector<unsigned char> payload)
    {
        CSerializedNetMsg msg;
        msg.m_type = type;
        msg.data = std::move(payload);
        BOOST_REQUIRE(connman.ReceiveMsgFrom(from, std::move(msg)));
    }
    void Queue(CNode& from, const dsl::CPoSeServiceReport& report) { Queue(from, NetMsgType::POSEREPORT, Wire(report)); }
    //! The liveness announcement of a masternode of the list, for the epoch whose base is `at`.
    dsl::CPoSeServiceResponse Announcement(const uint256& id, const CBlockIndex* at = nullptr) const
    {
        const CBlockIndex* block{at != nullptr ? at : base};
        dsl::CPoSeServiceResponse resp;
        resp.nEpoch = static_cast<uint32_t>(block->nHeight / Params().GetConsensus().nDSLEpochInterval);
        resp.proTxHash = id;
        resp.sig = dsl::SignChallengeResponse(KeyOf(id), block->GetBlockHash(), id);
        return resp;
    }
    void Queue(CNode& from, const dsl::CPoSeServiceResponse& resp)
    {
        CDataStream stream{SER_NETWORK, PROTOCOL_VERSION};
        stream << resp;
        Queue(from, NetMsgType::POSERESPONSE, {UCharCast(stream.data()), UCharCast(stream.data()) + stream.size()});
    }
    //! A message that only keeps a report behind it waiting for one round.
    void QueuePing(CNode& from) { Queue(from, NetMsgType::PING, {1, 2, 3, 4, 5, 6, 7, 8}); }
    //! Keeps the reports relayed to `peer` so far (FlushSendBuffer throws its send queue away).
    void KeepSent(CNode& peer)
    {
        LOCK(peer.cs_vSend);
        for (const CSerializedNetMsg& msg : peer.vSendMsg) {
            if (msg.m_type == NetMsgType::POSEREPORT) sent_reports[peer.GetId()].push_back(msg.data);
        }
    }
    bool Process(CNode& from)
    {
        // The test connection manager has no send buffer to speak of (nSendBufferMaxSize 0): anything pushed to the
        // peer -- its VERSION, a PONG -- pauses its sending, and ProcessMessages then takes nothing from it.
        KeepSent(from);
        connman.FlushSendBuffer(from);
        from.fPauseSend = false;
        LOCK(NetEventsInterface::g_msgproc_mutex);
        return peerman->ProcessMessages(&from, interrupt);
    }
    //! The same without touching the peer's send buffer first: for a test that sets its occupancy.
    bool ProcessAsIs(CNode& from)
    {
        LOCK(NetEventsInterface::g_msgproc_mutex);
        return peerman->ProcessMessages(&from, interrupt);
    }
    //! Runs the pre-verifier's waiting jobs (it has no thread in mode 2, SetDSLPreverifyModeForTest).
    size_t RunJobs() { return peerman->RunReportPreverifyJobsForTest(1000); }
    //! The peer's budget earned, and the view set, by one report taken the ordinary way.
    void Prime(CNode& from, const std::pair<uint256, uint256>& key)
    {
        Queue(from, Report(key.first, key.second));
        Process(from);
        BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, key.first, key.second));
    }
    void Observe()
    {
        dsl::TakePerfSnapshot();
        tally = Tally{};
        dsl::SetPerfEnabled(true);
    }
    const Tally& Poll()
    {
        tally.Add(dsl::TakePerfSnapshot());
        return tally;
    }
    int Score(const CNode& peer) const { return MisbehaviorScore(*peerman, peer); }
    void MineTo(int height)
    {
        while (WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()) < height) {
            MineBlock(m_node, CScript{} << OP_TRUE);
        }
        SyncWithValidationInterfaceQueue();
    }
    // `block` taken away by a reorg, as InvalidateBlock does, and given back.
    void Disconnect(const CBlockIndex* block)
    {
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, const_cast<CBlockIndex*>(block)));
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_LT(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), block->nHeight);
    }
    void Reconnect(const CBlockIndex* block)
    {
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(const_cast<CBlockIndex*>(block)));
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_GE(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), block->nHeight);
    }
    //! The epoch's base block replaced by another block at its height, with the same list: `base` is the new one.
    const CBlockIndex* SwitchBase()
    {
        const CBlockIndex* old{base};
        Disconnect(old);
        {
            QueueBlocker blocker; // as for the first base block
            MineBlock(m_node, CScript{} << OP_TRUE << OP_TRUE);
            base = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
            BOOST_REQUIRE_EQUAL(base->nHeight, ACTIVATION_HEIGHT);
            BOOST_REQUIRE(base != old);
            InstallList(base);
        }
        SyncWithValidationInterfaceQueue();
        BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH);
        return old;
    }
};

struct PreverifyOffSetup : PreverifySetup {
    PreverifyOffSetup() : PreverifySetup{0} {}
};
struct PreverifyThreadSetup : PreverifySetup {
    PreverifyThreadSetup() : PreverifySetup{1} {}
};
struct PreverifyBlocksOnlySetup : PreverifySetup {
    PreverifyBlocksOnlySetup() : PreverifySetup{2, /*blocksonly=*/true} {}
};
//! Eighty masternodes: above the floor the limits are sized from, so a shorter list lowers them.
struct PreverifyManySetup : PreverifySetup {
    PreverifyManySetup() : PreverifySetup{2, /*blocksonly=*/false, /*masternodes=*/80} {}
};
//! This node is a masternode of the list itself: its tick announces, emits its own reports and marks its observation.
struct PreverifyActiveSetup : PreverifySetup {
    PreverifyActiveSetup() : PreverifySetup{2, /*blocksonly=*/false, MN_COUNT, /*active=*/true} {}
};
} // namespace

BOOST_AUTO_TEST_SUITE(dsl_preverify_peerman_tests)

BOOST_FIXTURE_TEST_CASE(a_job_warms_the_cache_and_the_admission_finds_it, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Observe();
    // A ping in front keeps the report queued for a round, in which the scan gives it a job.
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a); // the scan, then the ping
    BOOST_CHECK(!dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_BLS], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::BLS_VERIFY], 0U); // the thread's check counts only as preverify_bls
    Process(*peer_a); // the report
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::BLS_VERIFY], 0U); // admitted from the cache
    BOOST_CHECK_EQUAL(tally[PerfMetric::CACHE_HIT], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::NET_ADMIT_CACHE_MISS], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_EXCLUDED], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::ACCEPTED], 1U);

    // Forced out of the cache after its job, the report is checked where it is processed (L5).
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(2).first, keys.at(2).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    dsl::ClearReportSignatureCacheForTest();
    Observe();
    Process(*peer_a);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(2).first, keys.at(2).second));
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::BLS_VERIFY], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::NET_ADMIT_CACHE_MISS], 1U);
}

BOOST_FIXTURE_TEST_CASE(a_report_taken_before_its_job_starts_is_checked_where_it_is_processed, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a); // the job is offered, the ping goes
    Process(*peer_a); // the report is taken first: its job never starts
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::BLS_VERIFY], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BLS], 0U);
}

BOOST_FIXTURE_TEST_CASE(reports_the_ordinary_path_would_not_check_get_no_job, PreverifySetup)
{
    const auto keys = Assigned();
    // Before the view is set (no report or announcement of this epoch was resolved yet), nothing gets a job.
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(0).first, keys.at(0).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    Process(*peer_a);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, keys.at(0).first, keys.at(0).second));

    const uint256 target{keys.at(1).first};
    const auto held = Report(keys.at(1).first, keys.at(1).second);
    Queue(*peer_a, held);
    Process(*peer_a);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, held.targetProTxHash, held.sentinelProTxHash));

    auto with_bytes = Wire(Report(keys.at(2).first, keys.at(2).second));
    with_bytes.push_back(0);
    auto future = Report(keys.at(3).first, keys.at(3).second);
    future.nEpoch = EPOCH + 1;
    future.Sign(KeyOf(future.sentinelProTxHash), base->GetBlockHash());
    const auto outsider = Report(target, Outsider(target));
    auto no_target = Report(target, keys.at(1).second);
    no_target.targetProTxHash = ArithToUint256(arith_uint256{999});
    no_target.Sign(KeyOf(no_target.sentinelProTxHash), base->GetBlockHash());
    const auto occupied = Report(held.targetProTxHash, held.sentinelProTxHash, dsl::ServiceStatus::MISSED);

    const std::vector<std::vector<unsigned char>> cases{with_bytes, Wire(future), Wire(outsider), Wire(no_target),
                                                        Wire(held), Wire(occupied)};
    for (const auto& bytes : cases) {
        Observe();
        QueuePing(*peer_a);
        Queue(*peer_a, NetMsgType::POSEREPORT, bytes);
        Process(*peer_a);
        BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_EXCLUDED], 1U);
        BOOST_CHECK_EQUAL(RunJobs(), 0U);
        Process(*peer_a);
    }
}

BOOST_FIXTURE_TEST_CASE(a_copy_gets_no_job_while_the_first_is_not_taken, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Prime(*peer_b, keys.at(1));
    const auto report = Report(keys.at(2).first, keys.at(2).second);
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, report);
    QueuePing(*peer_b);
    Queue(*peer_b, report);
    Process(*peer_a);
    Process(*peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_DUP_PENDING], 1U);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    Process(*peer_a);
    Process(*peer_b); // the copy, dropped unread as the ordinary path drops it
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, report.targetProTxHash, report.sentinelProTxHash));
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::DUPLICATE_UNDECODED], 1U);
}

BOOST_FIXTURE_TEST_CASE(a_report_stored_first_from_another_peer_drops_the_jobs_of_its_key_not_started, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Prime(*peer_b, keys.at(1));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    peerman->SetDSLTokensForTest(peer_b->GetId(), 100.0);
    // Peer a's report gets a job, which does not start; peer b's byte copy gets none, as the same bytes have one.
    const auto report = Report(keys.at(2).first, keys.at(2).second);
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, report);
    Process(*peer_a); // the scan gives a's report its job, and the ping
    QueuePing(*peer_b);
    Queue(*peer_b, report);
    Process(*peer_b); // no job for the copy, and the ping
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_DUP_PENDING], 1U);
    // b's copy is taken first, the ordinary way, and stored: a's job is dropped before it starts...
    Process(*peer_b);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, report.targetProTxHash, report.sentinelProTxHash));
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    // ...and a's report goes the way of a copy that never had one: dropped unread.
    Process(*peer_a);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE_UNDECODED], 1U);

    // A report of the same target and sentinel in other bytes (another status) has a job of its own, which a stored
    // report of the key drops as well: the ordinary path refuses it before its signature.
    const auto online = Report(keys.at(3).first, keys.at(3).second);
    const auto missed = Report(keys.at(3).first, keys.at(3).second, dsl::ServiceStatus::MISSED);
    QueuePing(*peer_a);
    Queue(*peer_a, missed);
    Process(*peer_a); // a job for the MISSED one
    QueuePing(*peer_b);
    Queue(*peer_b, online);
    Process(*peer_b); // a job for the ONLINE one: other bytes
    Process(*peer_b); // the ONLINE one taken before its job started, and stored
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, online.targetProTxHash, online.sentinelProTxHash));
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    Process(*peer_a); // the MISSED one: a duplicate of the key, refused before its signature
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BLS], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE], 2U);
}

namespace {
//! What the next test does right after the store holds a report, still under its lock (SetStoreInsertTestHookForTest).
std::function<void()> g_after_insert;
void AfterInsert()
{
    if (g_after_insert) g_after_insert();
}
} // namespace

BOOST_FIXTURE_TEST_CASE(the_store_drops_the_jobs_of_a_key_as_it_takes_a_report_of_it, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Prime(*peer_b, keys.at(2));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    peerman->SetDSLTokensForTest(peer_b->GetId(), 100.0);
    // Two jobs of one key, in other bytes -- a's report says ONLINE, b's MISSED -- and neither starts.
    const auto online = Report(keys.at(1).first, keys.at(1).second);
    const auto missed = Report(keys.at(1).first, keys.at(1).second, dsl::ServiceStatus::MISSED);
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, online);
    Process(*peer_a); // a's job, and the ping
    QueuePing(*peer_b);
    Queue(*peer_b, missed);
    Process(*peer_b); // b's job, and the ping
    // The store takes in a report of the key by itself, with no PeerManager code before or after it. Right after it
    // holds the report -- still in its own step, under its lock -- the jobs are tried: none of them is there to start,
    // both went before the report was held.
    std::optional<size_t> ran_once_held;
    g_after_insert = [&] { ran_once_held = RunJobs(); };
    dsl::SetStoreInsertTestHookForTest(AfterInsert);
    // Whatever happens below, the hook is taken away.
    struct Guard {
        ~Guard()
        {
            dsl::SetStoreInsertTestHookForTest(nullptr);
            g_after_insert = nullptr;
        }
    };
    {
        const Guard guard;
        BOOST_REQUIRE(dslman->ProcessReport(online, dmnman->GetListForBlock(base), base->GetBlockHash(), Params().GetConsensus()));
    }
    BOOST_REQUIRE(ran_once_held);
    BOOST_CHECK_EQUAL(*ran_once_held, 0U);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    // a's copy: dropped unread; b's: a duplicate of the key, refused before its signature.
    Process(*peer_a);
    Process(*peer_b);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BLS], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE_UNDECODED], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE], 2U);
}

BOOST_FIXTURE_TEST_CASE(another_peerman_made_and_gone_leaves_the_store_telling_this_one, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    // Another PeerManager on the same store, with a pre-verifier of its own, made and gone -- as when a test makes a
    // masternode of its node and replaces its PeerManager: the store tells each of them, and the one gone leaves the
    // others' observers in place.
    SetDSLPreverifyModeForTest(2);
    {
        const auto other = PeerManager::make(Params(), connman, *m_node.addrman, m_node.banman.get(), *m_node.chainman,
                                             *m_node.mempool, *m_node.mn_metaman, *m_node.mn_sync, *m_node.govman,
                                             *m_node.sporkman, /*mn_activeman=*/nullptr, dmnman, m_node.cj_ctx,
                                             m_node.llmq_ctx, dslman, /*ignore_incoming_txs=*/false);
    }
    // This one's job for a's report, which does not start, and the store taking in a report of its key: the job goes.
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_REQUIRE(dslman->ProcessReport(Report(keys.at(1).first, keys.at(1).second, dsl::ServiceStatus::MISSED),
                                        dmnman->GetListForBlock(base), base->GetBlockHash(), Params().GetConsensus()));
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
}

namespace {
//! What the next test does while the scan is past its first look at the store and before its last
//! (SetDSLScanTestHookForTest).
std::function<void()> g_during_scan;
void DuringScan()
{
    if (g_during_scan) g_during_scan();
}
} // namespace

BOOST_FIXTURE_TEST_CASE(a_report_stored_while_the_scan_looks_gets_no_job, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    // The same bytes reach the store from elsewhere after the scan's first look at the store found the key free, and
    // before its offer: the scan's last look, one step with the offer, finds it held, and offers nothing.
    const auto report = Report(keys.at(1).first, keys.at(1).second);
    bool stored{false};
    g_during_scan = [&] {
        if (!stored) {
            stored = dslman->ProcessReport(report, dmnman->GetListForBlock(base), base->GetBlockHash(), Params().GetConsensus());
        }
    };
    SetDSLScanTestHookForTest(DuringScan);
    // Whatever happens below, the hook is taken away.
    struct Guard {
        ~Guard()
        {
            SetDSLScanTestHookForTest(nullptr);
            g_during_scan = nullptr;
        }
    };
    {
        const Guard guard;
        Observe();
        QueuePing(*peer_a);
        Queue(*peer_a, report);
        Process(*peer_a); // the scan, the report stored meanwhile; and the ping
    }
    BOOST_REQUIRE(stored);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    // a's copy: dropped unread.
    Process(*peer_a);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BLS], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE_UNDECODED], 1U);
}

namespace {
//! What the next test does while the scan holds the store's lock, between its last look at the store (the key free)
//! and its offer (SetDSLOfferTestHookForTest).
std::function<void()> g_before_offer;
void BeforeOffer()
{
    if (g_before_offer) g_before_offer();
}
} // namespace

BOOST_FIXTURE_TEST_CASE(nothing_is_stored_between_the_scans_last_look_and_its_offer, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    // Between the scan's last look at the store, which found the key free, and its offer, another thread tries to
    // store the same bytes. It has to wait for the offer -- the look and the offer are one hold of the store's lock --
    // and its insert then drops the job offered.
    const auto report = Report(keys.at(1).first, keys.at(1).second);
    std::future<bool> stored;
    bool stored_meanwhile{false};
    g_before_offer = [&] {
        stored = std::async(std::launch::async, [&] {
            return dslman->ProcessReport(report, dmnman->GetListForBlock(base), base->GetBlockHash(), Params().GetConsensus());
        });
        stored_meanwhile = stored.wait_for(2s) == std::future_status::ready;
    };
    SetDSLOfferTestHookForTest(BeforeOffer);
    // Whatever happens below, the hook is taken away.
    struct Guard {
        ~Guard()
        {
            SetDSLOfferTestHookForTest(nullptr);
            g_before_offer = nullptr;
        }
    };
    {
        const Guard guard;
        Observe();
        QueuePing(*peer_a);
        Queue(*peer_a, report);
        Process(*peer_a); // the scan: the offer, the other thread's insert after it; and the ping
    }
    BOOST_REQUIRE(stored.valid());
    BOOST_REQUIRE(stored.get());
    BOOST_CHECK_MESSAGE(!stored_meanwhile, "a report was stored between the scan's last look at the store and its offer");
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    // a's copy: dropped unread.
    Process(*peer_a);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BLS], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE_UNDECODED], 1U);
}

BOOST_FIXTURE_TEST_CASE(a_report_of_an_epoch_the_store_has_let_go_gets_no_job, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0)); // the scan's view: this epoch, on its base
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    // The store's window has moved past this epoch (as the tick moves it at each epoch's base): the epoch has aged out
    // there, while the chain, the scan's view and the peer's budget would all let a report of it have a job.
    dslman->Store().SetCurrentEpoch(EPOCH + 8);
    BOOST_REQUIRE(dslman->Store().ReportAgedOut(EPOCH));
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_EXCLUDED], 1U);
}

namespace {
//! Keeps an observation mark the way PeerManager does -- each synchronous notification through
//! dsl::NotifyObservationMark -- and records the start of each event: its tip and the generation it got.
class MarkWatcher final : public CValidationInterface
{
public:
    explicit MarkWatcher(const CBlockIndex* tip) : mark{std::make_shared<dsl::ObservationMark>(tip)} {}
    const std::shared_ptr<dsl::ObservationMark> mark;
    std::vector<std::pair<const CBlockIndex*, std::optional<uint64_t>>> Events() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_events;
    }

protected:
    void SynchronousUpdatedBlockTip(const CBlockIndex* tip, const CBlockIndex*, bool) override
    {
        dsl::NotifyObservationMark(mark, tip);
    }
    void UpdatedBlockTip(const CBlockIndex* tip, const CBlockIndex*, bool) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_events.emplace_back(tip, mark->EventBegin(tip));
    }

private:
    mutable Mutex m_mutex;
    std::vector<std::pair<const CBlockIndex*, std::optional<uint64_t>>> m_events GUARDED_BY(m_mutex);
};
} // namespace

BOOST_FIXTURE_TEST_CASE(the_first_notification_arms_the_mark_behind_the_events_queued_before_it, RegTestingSetup)
{
    // Validation's own queue, held, so that the events below wait: block B connected, then disconnected -- two events
    // queued before the watcher listens; when they run they reach it all the same. Then the watcher listens, and B is
    // connected again: its first notification, of the very block of the oldest waiting event, and the event of it.
    MineBlock(m_node, CScript{} << OP_TRUE);
    SyncWithValidationInterfaceQueue();
    const CBlockIndex* before = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
    const CBlockIndex* block{nullptr};
    std::shared_ptr<MarkWatcher> watcher;
    {
        QueueBlocker blocker;
        MineBlock(m_node, CScript{} << OP_TRUE);
        block = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE(block->pprev == before);
        BlockValidationState state;
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().InvalidateBlock(state, const_cast<CBlockIndex*>(block)));
        BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()) == before);
        watcher = std::make_shared<MarkWatcher>(before);
        RegisterSharedValidationInterface(watcher);
        WITH_LOCK(cs_main, m_node.chainman->ActiveChainstate().ResetBlockFailureFlags(const_cast<CBlockIndex*>(block)));
        BOOST_REQUIRE(m_node.chainman->ActiveChainstate().ActivateBestChain(state));
        BOOST_REQUIRE(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip()) == block);
    }
    SyncWithValidationInterfaceQueue();
    UnregisterSharedValidationInterface(watcher);
    // The first event, B's from before the watcher listened, ran before the arming: it took nothing, although its
    // block is the one notified; nor did the disconnection's. The arming ran next, then the event of the watcher's own
    // notification, which got its generation.
    const auto events = watcher->Events();
    BOOST_REQUIRE_EQUAL(events.size(), 3U);
    BOOST_CHECK(events[0].first == block && !events[0].second);
    BOOST_CHECK(events[1].first == before && !events[1].second);
    BOOST_CHECK(events[2].first == block);
    BOOST_REQUIRE(events[2].second);
    BOOST_CHECK_EQUAL(*events[2].second, watcher->mark->Generation());
}

BOOST_FIXTURE_TEST_CASE(the_same_unreadable_report_from_two_peers_scores_both, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Prime(*peer_b, keys.at(1));
    // Exactly one report long, with signature bytes that are no point.
    auto bytes = Wire(Report(keys.at(2).first, keys.at(2).second));
    std::fill(bytes.end() - 96, bytes.end(), 0xff);
    for (bool run : {false, true}) {
        const int before_a{Score(*peer_a)};
        const int before_b{Score(*peer_b)};
        QueuePing(*peer_a);
        Queue(*peer_a, NetMsgType::POSEREPORT, bytes);
        QueuePing(*peer_b);
        Queue(*peer_b, NetMsgType::POSEREPORT, bytes);
        Process(*peer_a);
        Process(*peer_b);
        if (run) RunJobs();
        Process(*peer_a);
        Process(*peer_b);
        BOOST_CHECK_EQUAL(Score(*peer_a), before_a + UNREADABLE_SCORE);
        BOOST_CHECK_EQUAL(Score(*peer_b), before_b + UNREADABLE_SCORE);
        RunJobs();
    }
}

BOOST_FIXTURE_TEST_CASE(the_wave_window_is_the_two_blocks_from_the_emission, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    const int interval{Params().GetConsensus().nDSLEpochInterval};
    size_t next{1};
    // Position: whether a lone young report with a pending job is held.
    for (const auto& [pos, held] : std::vector<std::pair<int, bool>>{{17, false}, {18, true}, {19, true}, {20, false}, {21, false}}) {
        MineTo(ACTIVATION_HEIGHT + pos);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()) % interval, pos);
        const auto key = keys.at(next++);
        Queue(*peer_a, Report(key.first, key.second));
        Observe();
        Process(*peer_a); // scanned and offered; nothing runs it
        BOOST_CHECK_MESSAGE(dslman->Store().HaveReport(EPOCH, key.first, key.second) != held, "position " << pos);
        BOOST_CHECK_EQUAL(Poll()[PerfMetric::GATE_HOLD], held ? 1U : 0U);
        if (held) {
            // Five seconds after its arrival it goes, job or not.
            now += 5s;
            SetQueueClockMockForTest(now);
            Process(*peer_a);
            BOOST_CHECK(dslman->Store().HaveReport(EPOCH, key.first, key.second));
        }
        RunJobs();
    }
}

BOOST_FIXTURE_TEST_CASE(the_test_delay_is_waited_once_by_whoever_started_the_report, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    constexpr auto delay{400ms};
    const auto timed = [](auto&& fn) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        return std::chrono::steady_clock::now() - start;
    };
    std::optional<TestDelay> slow{std::in_place, delay};
    // The thread started the job: it waits, before reading; the message thread does not wait again.
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_CHECK(timed([&] { BOOST_CHECK_EQUAL(RunJobs(), 1U); }) >= delay);
    const auto after_thread = timed([&] { Process(*peer_a); });
    // The message thread took the report first: the job never starts, and the message thread waits.
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(2).first, keys.at(2).second));
    Process(*peer_a);
    const auto taken_first = timed([&] { Process(*peer_a); });
    const auto never_run = timed([&] { BOOST_CHECK_EQUAL(RunJobs(), 0U); });
    slow.reset();
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(2).first, keys.at(2).second));
    BOOST_CHECK_MESSAGE(after_thread < delay, "the message thread waited again after the thread had waited");
    BOOST_CHECK(taken_first >= delay);
    BOOST_CHECK(never_run < delay);
}

BOOST_FIXTURE_TEST_CASE(the_budget_covers_every_message_in_front_and_a_whole_token, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    size_t next{1};
    // One report queued behind a ping: it needs one whole stored token.
    for (const auto& [tokens, jobs] : std::vector<std::pair<double, size_t>>{{0.99, 0}, {1.0, 1}}) {
        const auto key = keys.at(next++);
        QueuePing(*peer_a);
        Queue(*peer_a, Report(key.first, key.second));
        peerman->SetDSLTokensForTest(peer_a->GetId(), tokens);
        Observe();
        Process(*peer_a);
        BOOST_CHECK_MESSAGE(RunJobs() == jobs, tokens << " tokens");
        BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_BUDGET_SHORT], jobs == 0 ? 1U : 0U);
        Process(*peer_a);
    }
    // Two reports: the second needs a token beyond the first's.
    for (const auto& [tokens, jobs] : std::vector<std::pair<double, size_t>>{{1.99, 1}, {2.0, 2}}) {
        const auto first = keys.at(next++);
        const auto second = keys.at(next++);
        QueuePing(*peer_a);
        Queue(*peer_a, Report(first.first, first.second));
        Queue(*peer_a, Report(second.first, second.second));
        peerman->SetDSLTokensForTest(peer_a->GetId(), tokens);
        Process(*peer_a);
        BOOST_CHECK_MESSAGE(RunJobs() == jobs, tokens << " tokens");
        peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
        Process(*peer_a);
        Process(*peer_a);
    }
    // A challenge, an announcement or a commitment in front costs a token as well.
    const auto key = keys.at(next++);
    Queue(*peer_a, NetMsgType::POSECOMMIT, {0});
    Queue(*peer_a, Report(key.first, key.second));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 1.0);
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
}

BOOST_FIXTURE_TEST_CASE(the_scan_starts_at_the_activation_height_exactly, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    // One block below: the scan does not run at all, as the report's own handling does not.
    Disconnect(base);
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_EXCLUDED], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BUDGET_SHORT], 0U);
    Process(*peer_a);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::ACTIVATION_REFUSED], 1U);
    // At the activation height itself, the same report gets its job.
    Reconnect(base);
    InstallList(base);
    SyncWithValidationInterfaceQueue();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    Process(*peer_a);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
}

BOOST_FIXTURE_TEST_CASE(a_gone_peer_leaves_no_job_behind, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Prime(*peer_b, keys.at(1));
    const auto report = Report(keys.at(2).first, keys.at(2).second);
    QueuePing(*peer_a);
    Queue(*peer_a, report);
    Process(*peer_a);
    nodes.Finalize(*peer_a);
    peer_a = &nodes.Own(MakeTestPeer(5));
    nodes.Initialize(*peer_a);
    // The gone peer's job no longer holds the bytes: the same report from another peer gets one.
    Observe();
    QueuePing(*peer_b);
    Queue(*peer_b, report);
    Process(*peer_b);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_DUP_PENDING], 0U);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(dsl_preverify_off_tests)

BOOST_FIXTURE_TEST_CASE(without_preverification_reports_are_checked_where_they_are_processed, PreverifyOffSetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    Process(*peer_a);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::BLS_VERIFY], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::GATE_SNAPSHOT], 0U);
}

BOOST_AUTO_TEST_SUITE_END()

// ---- more of a real PeerManager: who gets a job, the view, the signing zone, the thread, and equivalence ----

namespace {
//! What a run of the equivalence script left behind, report by report: the same in every mode of the pre-verifier.
//! Reports are named by the script; "?" is a payload that is none of the script's reports, byte for byte.
struct ScriptResult {
    std::vector<std::string> stored_before; //!< the store before the base is switched: "name:status", sorted
    std::vector<std::string> stored_after;  //!< and at the end
    std::map<std::string, std::vector<std::string>> relayed; //!< to each recipient, in the order sent
    int score_a{0};
    int score_b{0};
    uint64_t accepted{0};
    uint64_t refused{0};
    uint64_t decode_refused{0};
    uint64_t duplicate{0};
    uint64_t budget_refused{0};
    bool operator==(const ScriptResult& o) const
    {
        return std::tie(stored_before, stored_after, relayed, score_a, score_b, accepted, refused, decode_refused,
                        duplicate, budget_refused) ==
               std::tie(o.stored_before, o.stored_after, o.relayed, o.score_a, o.score_b, o.accepted, o.refused,
                        o.decode_refused, o.duplicate, o.budget_refused);
    }
};
std::ostream& operator<<(std::ostream& os, const ScriptResult& r)
{
    const auto list = [&os](const std::vector<std::string>& names) {
        os << "[";
        for (const auto& name : names) os << name << " ";
        os << "]";
    };
    os << "stored_before=";
    list(r.stored_before);
    os << " stored_after=";
    list(r.stored_after);
    for (const auto& [recipient, names] : r.relayed) {
        os << " to_" << recipient << "=";
        list(names);
    }
    return os << " score_a=" << r.score_a << " score_b=" << r.score_b << " accepted=" << r.accepted
              << " refused=" << r.refused << " decode_refused=" << r.decode_refused << " duplicate=" << r.duplicate
              << " budget_refused=" << r.budget_refused;
}

/** A fixed sequence of reports from two peers, a and b, that the relays reach as well, each report queued behind a
 *  ping (so the scan sees it before its processing): new, a byte copy, a bad signature, a conflicting status, a
 *  trailing byte, unreadable bytes, an unassigned sentinel; then one from b beyond its budget, and the same bytes from
 *  a within its own; then, after the epoch's base block was replaced by another, one signed against the old base and
 *  two against the new one. `run_jobs`: the pre-verifier's jobs run between the scan and the processing. */
ScriptResult RunEquivalenceScript(PreverifySetup& f, bool run_jobs)
{
    CNode& a = f.AddRelayPeer(30);
    CNode& b = f.AddRelayPeer(31);
    std::map<std::vector<unsigned char>, std::string> names;
    const auto named = [&](const std::string& name, const dsl::CPoSeServiceReport& report) {
        names[PreverifySetup::Wire(report)] = name;
        return report;
    };
    const auto name_of = [&](const std::vector<unsigned char>& bytes) {
        const auto it = names.find(bytes);
        return it == names.end() ? std::string{"?"} : it->second;
    };
    const auto stored = [&] {
        std::vector<std::string> out;
        for (const auto& report : f.dslman->Store().GetReportsForEpoch(EPOCH)) {
            out.push_back(name_of(PreverifySetup::Wire(report)) + ":" + ToString(static_cast<int>(report.status)));
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto step = [&](CNode& peer, const std::vector<unsigned char>& bytes) {
        f.QueuePing(peer);
        f.Queue(peer, NetMsgType::POSEREPORT, bytes);
        f.Process(peer); // the scan, and the ping
        if (run_jobs) f.RunJobs();
        f.Process(peer); // the report
    };

    const auto keys = f.Assigned();
    const auto prime_a = named("prime_a", f.Report(keys.at(0).first, keys.at(0).second));
    const auto prime_b = named("prime_b", f.Report(keys.at(1).first, keys.at(1).second));
    f.Queue(a, prime_a);
    f.Process(a);
    f.Queue(b, prime_b);
    f.Process(b);
    const auto first = named("first", f.Report(keys.at(2).first, keys.at(2).second));
    auto bad = f.Report(keys.at(3).first, keys.at(3).second);
    const uint256 other_signer{keys.at(3).second == keys.at(0).second ? keys.at(1).second : keys.at(0).second};
    bad.Sign(f.KeyOf(other_signer), f.base->GetBlockHash());
    named("bad", bad);
    const auto conflicting = named("conflicting", f.Report(keys.at(2).first, keys.at(2).second, dsl::ServiceStatus::MISSED));
    auto trailing = PreverifySetup::Wire(named("trailing", f.Report(keys.at(4).first, keys.at(4).second)));
    trailing.push_back(0);
    auto unreadable = PreverifySetup::Wire(f.Report(keys.at(5).first, keys.at(5).second));
    std::fill(unreadable.end() - 96, unreadable.end(), 0xff);
    const uint256 target{keys.at(6).first};
    const auto outsider = named("outsider", f.Report(target, f.Outsider(target)));
    const auto late = named("late", f.Report(keys.at(7).first, keys.at(7).second));

    f.Observe();
    step(a, PreverifySetup::Wire(first));
    step(b, PreverifySetup::Wire(first));
    step(a, PreverifySetup::Wire(bad));
    step(b, PreverifySetup::Wire(conflicting));
    step(a, trailing);
    step(b, unreadable);
    step(a, PreverifySetup::Wire(outsider));
    // Beyond its sender's budget (half a token left): refused unread, whatever the pre-verifier did, and b is not
    // taken to have it. The same bytes from a, within its budget: taken, and relayed to b as well.
    f.peerman->SetDSLTokensForTest(b.GetId(), 0.5);
    step(b, PreverifySetup::Wire(late));
    f.peerman->SetDSLTokensForTest(b.GetId(), 100.0);
    f.peerman->SetDSLTokensForTest(a.GetId(), 100.0);
    step(a, PreverifySetup::Wire(late));

    ScriptResult r;
    r.stored_before = stored();
    // The counters so far: the epoch change below writes the perf summary, which takes them (LogPerfSummary).
    f.Poll();

    // The epoch's base block replaced by another: the store starts the epoch again. A report assigned on the new
    // base but signed against the old one is refused; reports signed against the new one are taken.
    const CBlockIndex* old_base = f.SwitchBase();
    const auto rebased = f.Assigned();
    auto stale = f.Report(rebased.at(0).first, rebased.at(0).second);
    stale.Sign(f.KeyOf(stale.sentinelProTxHash), old_base->GetBlockHash());
    named("stale", stale);
    const auto new_a = named("new_a", f.Report(rebased.at(1).first, rebased.at(1).second));
    const auto new_b = named("new_b", f.Report(rebased.at(2).first, rebased.at(2).second));
    step(a, PreverifySetup::Wire(stale));
    step(a, PreverifySetup::Wire(new_a));
    step(b, PreverifySetup::Wire(new_b));

    r.stored_after = stored();
    f.KeepSent(a);
    f.KeepSent(b);
    for (const auto& [recipient, id] : std::vector<std::pair<std::string, NodeId>>{{"a", a.GetId()}, {"b", b.GetId()}}) {
        for (const auto& bytes : f.sent_reports[id]) r.relayed[recipient].push_back(name_of(bytes));
    }
    for (const auto& bytes : f.Relayed()) r.relayed["listener"].push_back(name_of(bytes));
    r.score_a = f.Score(a);
    r.score_b = f.Score(b);
    const Tally& t = f.Poll();
    r.accepted = t[PerfMetric::ACCEPTED];
    r.refused = t[PerfMetric::REFUSED];
    r.decode_refused = t[PerfMetric::DECODE_REFUSED];
    r.duplicate = t[PerfMetric::DUPLICATE];
    r.budget_refused = t[PerfMetric::BUDGET_REFUSED];
    return r;
}

// What every mode must leave, report by report.
//  - Stored before the base is switched: the two primed reports, the first, the one sent with a trailing byte, and
//    the late one from the sender whose budget covered it, each with the status it was sent with (1: ONLINE). The
//    conflicting status did not replace the first's.
//  - Stored at the end: the two signed against the new base.
//  - Relayed: every report taken, once, as its canonical bytes (the trailing byte is not relayed), to the listener
//    and to the other sender, never back to the peer it came from. The late one reaches b: its copy refused for the
//    budget did not count as b having it.
//  - Refused: the bad signature, the unassigned sentinel and the one signed against the old base; the unreadable one
//    refused undecoded and scored; the byte copy and the conflicting status taken as duplicates; one refused for
//    the budget.
const ScriptResult EXPECTED_SCRIPT{
    /*stored_before=*/{"first:1", "late:1", "prime_a:1", "prime_b:1", "trailing:1"},
    /*stored_after=*/{"new_a:1", "new_b:1"},
    /*relayed=*/{{"a", {"prime_b", "new_b"}},
                 {"b", {"prime_a", "first", "trailing", "late", "new_a"}},
                 {"listener", {"prime_a", "prime_b", "first", "trailing", "late", "new_a", "new_b"}}},
    /*score_a=*/0, /*score_b=*/UNREADABLE_SCORE,
    /*accepted=*/5, /*refused=*/3, /*decode_refused=*/1, /*duplicate=*/2, /*budget_refused=*/1};
} // namespace

BOOST_AUTO_TEST_SUITE(dsl_preverify_peerman_tests)

BOOST_FIXTURE_TEST_CASE(the_same_sequence_ends_the_same_without_a_thread_jobs_run, PreverifySetup)
{
    BOOST_CHECK_EQUAL(RunEquivalenceScript(*this, /*run_jobs=*/true), EXPECTED_SCRIPT);
}

BOOST_FIXTURE_TEST_CASE(the_same_sequence_ends_the_same_without_a_thread_jobs_held, PreverifySetup)
{
    BOOST_CHECK_EQUAL(RunEquivalenceScript(*this, /*run_jobs=*/false), EXPECTED_SCRIPT);
}

BOOST_FIXTURE_TEST_CASE(reports_from_a_connection_the_ordinary_path_would_not_read_get_no_job, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    CNode& early = AddPeer(20, ConnectionType::OUTBOUND_FULL_RELAY, NetPermissionFlags::None, /*connected=*/false);
    CNode& blockonly = AddPeer(21, ConnectionType::BLOCK_RELAY);
    size_t next{1};
    for (CNode* node : {&early, &blockonly}) {
        peerman->SetDSLTokensForTest(node->GetId(), 100.0);
        const auto key = keys.at(next++);
        Observe();
        QueuePing(*node);
        Queue(*node, Report(key.first, key.second));
        Process(*node);
        BOOST_CHECK_EQUAL(RunJobs(), 0U);
        Poll();
        BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_EXCLUDED] + tally[PerfMetric::PREVERIFY_BUDGET_SHORT], 0U);
    }
    // The ordinary path still disconnects the block-relay-only connection for its report, as before.
    Process(blockonly);
    BOOST_CHECK(blockonly.fDisconnect);
    // A peer with NoBan gets a job on the count limit alone, with nothing in its budget.
    CNode& noban = AddPeer(22, ConnectionType::INBOUND, NetPermissionFlags::NoBan);
    const auto key = keys.at(next++);
    QueuePing(noban);
    Queue(noban, Report(key.first, key.second));
    Process(noban);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
}

BOOST_FIXTURE_TEST_CASE(an_older_epoch_view_comes_back_only_after_a_real_rewind, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    const CBlockIndex* next = AdvanceToNextEpoch();
    const auto newer = AssignedAt(next);
    Queue(*peer_a, Report(newer.at(0).first, newer.at(0).second, dsl::ServiceStatus::ONLINE, next));
    Process(*peer_a);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH + 1, newer.at(0).first, newer.at(0).second));
    // A report of the retained older epoch, taken the ordinary way, leaves the newer view: the next one of that
    // epoch gets no job.
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(2).first, keys.at(2).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    Process(*peer_a);
    // After the chain goes back into the older epoch, its view returns, and a report of it gets a job again.
    Disconnect(next);
    Queue(*peer_a, Report(keys.at(3).first, keys.at(3).second));
    Process(*peer_a);
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(4).first, keys.at(4).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
}

BOOST_FIXTURE_TEST_CASE(a_report_held_at_position_19_goes_in_order_at_position_20, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    MineTo(ACTIVATION_HEIGHT + 19);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Process(*peer_a);
    BOOST_REQUIRE(!dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second)); // held, young, pending
    // The signing zone: the window shuts, and the same young report with the same pending job goes in order.
    MineTo(ACTIVATION_HEIGHT + 20);
    Observe();
    Process(*peer_a);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::GATE_HOLD], 0U);
}

BOOST_FIXTURE_TEST_CASE(the_message_behind_a_report_is_read_only_while_the_window_is_open, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    // Position 18, the window open: the message behind a young report with a pending job is asked whether it may
    // overtake (a ping may not, so the report goes first).
    MineTo(ACTIVATION_HEIGHT + 18);
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    QueuePing(*peer_a);
    Process(*peer_a);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    auto behind = peer_a->PollMessage();
    BOOST_REQUIRE(behind);
    BOOST_CHECK_EQUAL(behind->first.m_type, NetMsgType::PING);
    BOOST_CHECK_EQUAL(static_cast<int>(behind->first.m_overtake), static_cast<int>(NetMessagePollGate::NEVER));
    // Position 20, the window shut: the same queue with an inventory behind the report. The report goes in order,
    // and the inventory's content was never read.
    MineTo(ACTIVATION_HEIGHT + 20);
    Queue(*peer_a, Report(keys.at(2).first, keys.at(2).second));
    Queue(*peer_a, NetMsgType::INV, Inventory({MSG_TX, MSG_TX, MSG_TX}));
    Process(*peer_a);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, keys.at(2).first, keys.at(2).second));
    behind = peer_a->PollMessage();
    BOOST_REQUIRE(behind);
    BOOST_CHECK_EQUAL(behind->first.m_type, NetMsgType::INV);
    BOOST_CHECK_EQUAL(static_cast<int>(behind->first.m_overtake), -1);
}

BOOST_FIXTURE_TEST_CASE(a_getdata_overtakes_only_below_half_of_the_send_buffer, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    MineTo(ACTIVATION_HEIGHT + 18); // the window is open: this node is no masternode
    const auto getdata = Inventory({MSG_TX});
    size_t next{1};
    // The connection's send occupancy as its sending is paused by: its send queue's share and what its transport has
    // taken to send. At exactly half of the send buffer it is not below half: the GETDATA waits for the report. With
    // two bytes more of buffer it is below, and the GETDATA goes ahead.
    for (const bool room : {false, true}) {
        const auto key = keys.at(next++);
        Queue(*peer_a, Report(key.first, key.second));
        Queue(*peer_a, NetMsgType::GETDATA, getdata);
        KeepSent(*peer_a);
        connman.FlushSendBuffer(*peer_a);
        size_t occupancy{0};
        {
            LOCK(peer_a->cs_vSend);
            CSerializedNetMsg in_transport;
            in_transport.m_type = NetMsgType::PING;
            in_transport.data.assign(64, 0);
            BOOST_REQUIRE(peer_a->m_transport->SetMessageToSend(in_transport));
            const size_t transport{peer_a->m_transport->GetSendMemoryUsage()};
            BOOST_REQUIRE_GT(transport, 0U);
            peer_a->m_send_memusage = 1000;
            occupancy = 1000 + transport;
        }
        connman.SetSendBufferMaxSize(static_cast<unsigned int>(room ? 2 * occupancy + 2 : 2 * occupancy));
        peer_a->fPauseSend = false;
        Observe();
        ProcessAsIs(*peer_a);
        BOOST_CHECK_MESSAGE(Poll()[PerfMetric::MSG_OVERTOOK] == (room ? 1U : 0U), "room=" << room);
        BOOST_CHECK_MESSAGE(dslman->Store().HaveReport(EPOCH, key.first, key.second) != room, "room=" << room);
        // The rest of the queue, for the next case.
        connman.SetSendBufferMaxSize(0);
        RunJobs();
        Process(*peer_a);
        Process(*peer_a);
        BOOST_CHECK(dslman->Store().HaveReport(EPOCH, key.first, key.second));
    }
}

namespace {
//! The overtake policy of the next test (SetDSLOvertakePolicyForTest): DSLOvertakePolicy's verdict, counting the
//! calls made while cs_main was held. Seen from another thread, as cs_main is recursive (the asking thread would take
//! it again although it held it): free if that thread takes it within five seconds, while the asker waits here.
std::atomic<int> g_policy_calls{0};
std::atomic<int> g_policy_calls_under_main{0};
int8_t PolicyWatchingMain(const CNetMessage& msg)
{
    ++g_policy_calls;
    const bool main_free{std::async(std::launch::async, [] {
                             const auto deadline{std::chrono::steady_clock::now() + 5s};
                             do {
                                 TRY_LOCK(::cs_main, lock);
                                 if (lock) return true;
                                 std::this_thread::sleep_for(10ms);
                             } while (std::chrono::steady_clock::now() < deadline);
                             return false;
                         }).get()};
    if (!main_free) ++g_policy_calls_under_main;
    return DSLOvertakePolicy(msg);
}
} // namespace

BOOST_FIXTURE_TEST_CASE(the_message_behind_a_report_is_read_with_cs_main_free, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    MineTo(ACTIVATION_HEIGHT + 18); // the window is open: this node is no masternode
    // A young report with a pending job, and an inventory of a transaction behind it: the inventory's content is read,
    // and validation does not wait for the reading -- cs_main is free while the policy reads it.
    Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
    Queue(*peer_a, NetMsgType::INV, Inventory({MSG_TX}));
    g_policy_calls = 0;
    g_policy_calls_under_main = 0;
    SetDSLOvertakePolicyForTest(PolicyWatchingMain);
    // Whatever happens below, the policy is given back.
    struct Guard {
        ~Guard() { SetDSLOvertakePolicyForTest(nullptr); }
    };
    {
        const Guard guard;
        Observe();
        Process(*peer_a);
    }
    // The inventory went ahead of the report, read once, with cs_main free.
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::MSG_OVERTOOK], 1U);
    BOOST_CHECK(!dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    BOOST_CHECK_EQUAL(g_policy_calls.load(), 1);
    BOOST_CHECK_EQUAL(g_policy_calls_under_main.load(), 0);
    // The rest of the queue: the report, once its job has run.
    RunJobs();
    Process(*peer_a);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
}

namespace {
//! The overtake policy of the next test: DSLOvertakePolicy's verdict, after what the test does while the policy reads
//! the message -- after the snapshot's first look at the window, without cs_main (g_during_classification).
std::function<void()> g_during_classification;
int8_t PolicyWithAStep(const CNetMessage& msg)
{
    if (g_during_classification) g_during_classification();
    return DSLOvertakePolicy(msg);
}
} // namespace

BOOST_FIXTURE_TEST_CASE(the_window_that_shuts_while_the_message_is_read_decides, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    MineTo(ACTIVATION_HEIGHT + 19); // the window's last block: open, this node is no masternode
    const auto key = keys.at(1);
    // A young report with a pending job and a GETDATA of a transaction behind it, with no room to send at the
    // snapshot's first look.
    Queue(*peer_a, Report(key.first, key.second));
    Queue(*peer_a, NetMsgType::GETDATA, Inventory({MSG_TX}));
    KeepSent(*peer_a);
    connman.FlushSendBuffer(*peer_a);
    peer_a->fPauseSend = false;
    // While the policy reads the GETDATA, another thread connects position 20 -- the window shuts -- and only then is
    // there room to send. The window open and room to send never hold at once.
    std::future<void> mined;
    bool connected_meanwhile{false};
    g_during_classification = [&] {
        mined = std::async(std::launch::async, [&] { MineBlock(m_node, CScript{} << OP_TRUE); });
        connected_meanwhile = mined.wait_for(30s) == std::future_status::ready;
        connman.SetSendBufferMaxSize(1U << 20);
    };
    SetDSLOvertakePolicyForTest(PolicyWithAStep);
    // Whatever happens below, the policy is given back.
    struct Guard {
        ~Guard()
        {
            SetDSLOvertakePolicyForTest(nullptr);
            g_during_classification = nullptr;
        }
    };
    {
        const Guard guard;
        Observe();
        ProcessAsIs(*peer_a);
    }
    if (mined.valid()) mined.wait();
    connman.SetSendBufferMaxSize(0);
    BOOST_REQUIRE(connected_meanwhile);
    BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), ACTIVATION_HEIGHT + 20);
    // The decision looked at the window again, with the room to send, and saw it shut: the report went first, in
    // arrival order, and the GETDATA did not overtake.
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::MSG_OVERTOOK], 0U);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, key.first, key.second));
    SyncWithValidationInterfaceQueue();
}

BOOST_FIXTURE_TEST_CASE(one_scan_visits_at_most_sixteen_reports, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 1000.0);
    // The view knows the assignment of a target once a scan has derived it, one target a scan: the first report of
    // each of four targets warms it, so that nothing but the count limits the scan below.
    std::vector<std::pair<uint256, uint256>> rest;
    std::set<uint256> warmed;
    for (size_t i = 1; i < keys.size() && warmed.size() < 4; ++i) {
        if (!warmed.insert(keys[i].first).second) {
            rest.push_back(keys[i]);
            continue;
        }
        QueuePing(*peer_a);
        Queue(*peer_a, Report(keys[i].first, keys[i].second));
        Process(*peer_a);
        BOOST_REQUIRE_EQUAL(RunJobs(), 1U);
        Process(*peer_a);
    }
    BOOST_REQUIRE_GE(rest.size(), SCAN_MAX + 1);
    rest.resize(SCAN_MAX + 1);
    QueuePing(*peer_a);
    QueuePing(*peer_a);
    for (const auto& key : rest) Queue(*peer_a, Report(key.first, key.second));
    Process(*peer_a); // one scan, and the first ping
    BOOST_CHECK_EQUAL(RunJobs(), SCAN_MAX);
    Process(*peer_a); // the next scan, and the second ping
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
}

BOOST_FIXTURE_TEST_CASE(one_scan_derives_one_assignment_the_view_does_not_have, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 1000.0);
    // One report of each of two targets the scan has not met: both would get a job, but for the assignment.
    std::vector<std::pair<uint256, uint256>> fresh;
    for (size_t i = 1; i < keys.size() && fresh.size() < 2; ++i) {
        if (keys[i].first == keys[0].first) continue;
        if (!fresh.empty() && keys[i].first == fresh.back().first) continue;
        fresh.push_back(keys[i]);
    }
    BOOST_REQUIRE_EQUAL(fresh.size(), 2U);
    Observe();
    QueuePing(*peer_a);
    QueuePing(*peer_a);
    for (const auto& key : fresh) Queue(*peer_a, Report(key.first, key.second));
    Process(*peer_a); // the first target's assignment is derived, the second waits for the next scan
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_COLD_ASSIGN], 1U);
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_COLD_ASSIGN], 2U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_EXCLUDED], 0U);
}

BOOST_FIXTURE_TEST_CASE(a_masternode_probe_gets_jobs_only_after_its_mnauth, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    size_t next{1};
    // A probe connection: before its first message, and after a first message that was not MNAUTH (the ordinary path
    // drops the connection for it), its reports get no job -- the scan does not even run. After MNAUTH they do.
    struct Case {
        std::chrono::seconds first_message;
        bool mnauth;
        size_t jobs;
    };
    NodeId id{40};
    for (const Case& c : {Case{0s, false, 0}, Case{1s, false, 0}, Case{1s, true, 1}}) {
        CNode& probe = AddPeer(id++, ConnectionType::OUTBOUND_FULL_RELAY);
        probe.m_masternode_probe_connection = true;
        probe.nTimeFirstMessageReceived = c.first_message;
        probe.fFirstMessageIsMNAUTH = c.mnauth;
        peerman->SetDSLTokensForTest(probe.GetId(), 100.0);
        const auto key = keys.at(next++);
        Observe();
        QueuePing(probe);
        Queue(probe, Report(key.first, key.second));
        Process(probe);
        BOOST_CHECK_MESSAGE(RunJobs() == c.jobs, "first message at " << c.first_message.count() << ", mnauth " << c.mnauth);
        Poll();
        BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_EXCLUDED] + tally[PerfMetric::PREVERIFY_BUDGET_SHORT], 0U);
    }
}

BOOST_FIXTURE_TEST_CASE(a_report_of_an_epoch_the_store_has_not_begun_gets_no_job, PreverifySetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    const int next_base{ACTIVATION_HEIGHT + Params().GetConsensus().nDSLEpochInterval};
    MineTo(next_base - 1);
    const CBlockIndex* next{nullptr};
    std::vector<std::pair<uint256, uint256>> newer;
    {
        // The next epoch's base block is connected, and its event waits: the chain is in the next epoch, the store,
        // which the tick moves on, is not.
        QueueBlocker blocker;
        MineBlock(m_node, CScript{} << OP_TRUE);
        next = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE_EQUAL(next->nHeight, next_base);
        InstallList(next);
        newer = AssignedAt(next);
        BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH);
        // Taken the ordinary way, a report of that epoch moves the scan's view to it, and the store refuses it.
        Queue(*peer_a, Report(newer.at(0).first, newer.at(0).second, dsl::ServiceStatus::ONLINE, next));
        Process(*peer_a);
        BOOST_REQUIRE(!dslman->Store().HaveReport(EPOCH + 1, newer.at(0).first, newer.at(0).second));
        // The next one has its base on the active chain and in the view, both ends on the list, an assigned sentinel
        // and a budget: only the store's epoch window keeps a job from it.
        Observe();
        QueuePing(*peer_a);
        Queue(*peer_a, Report(newer.at(1).first, newer.at(1).second, dsl::ServiceStatus::ONLINE, next));
        Process(*peer_a);
        BOOST_CHECK_EQUAL(RunJobs(), 0U);
        BOOST_CHECK_EQUAL(Poll()[PerfMetric::PREVERIFY_EXCLUDED], 1U);
        Process(*peer_a);
        BOOST_CHECK(!dslman->Store().HaveReport(EPOCH + 1, newer.at(1).first, newer.at(1).second));
    }
    SyncWithValidationInterfaceQueue();
    BOOST_REQUIRE_EQUAL(dslman->CurrentEpoch(), EPOCH + 1);
    // Once the tick has begun the epoch, such a report gets its job.
    QueuePing(*peer_a);
    Queue(*peer_a, Report(newer.at(2).first, newer.at(2).second, dsl::ServiceStatus::ONLINE, next));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    Process(*peer_a);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH + 1, newer.at(2).first, newer.at(2).second));
}

BOOST_FIXTURE_TEST_CASE(lowered_limits_reach_the_waiting_jobs_where_the_count_is_read, PreverifyManySetup)
{
    // Eighty masternodes: one peer may hold more jobs than the epoch assigns reports. All of the epoch's reports
    // from one peer, queued behind pings so that every scan gives one more target's reports their jobs (a scan
    // derives one assignment) while none is processed.
    const auto keys = Assigned();
    const size_t offered{keys.size() - 1};
    const size_t lowered{PerPeerJobLimit(64)};
    BOOST_REQUIRE_LE(offered, PerPeerJobLimit(mn_count));
    BOOST_REQUIRE_GT(offered, lowered);
    Prime(*peer_a, keys.at(0));
    peerman->SetDSLTokensForTest(peer_a->GetId(), 1000000.0);
    const size_t rounds{mn_count + 1};
    for (size_t i = 0; i < rounds; ++i) QueuePing(*peer_a);
    for (size_t i = 1; i < keys.size(); ++i) Queue(*peer_a, Report(keys[i].first, keys[i].second));
    Observe();
    for (size_t i = 0; i < rounds; ++i) Process(*peer_a);
    // Every one of them was scanned and got its job.
    BOOST_REQUIRE(peer_a->PeekUnscannedReports(1, dsl::CPoSeServiceReport::WIRE_SIZE).items.empty());
    Poll();
    BOOST_REQUIRE_EQUAL(tally[PerfMetric::PREVERIFY_QUEUE_FULL] + tally[PerfMetric::PREVERIFY_EXCLUDED] +
                        tally[PerfMetric::PREVERIFY_BUDGET_SHORT], 0U);
    // The next epoch's list is shorter, at the floor. The count is read at the first Sentinel message of the epoch --
    // here another peer's, and no report: the limits come down there and then, and the waiting jobs above them are
    // dropped, although nothing is scanned.
    AdvanceToNextEpoch(64);
    Queue(*peer_b, NetMsgType::POSECOMMIT, {0});
    Process(*peer_b);
    BOOST_CHECK_EQUAL(RunJobs(), lowered);
}

BOOST_FIXTURE_TEST_CASE(before_its_own_observation_a_masternode_takes_reports_in_order, PreverifyActiveSetup)
{
    const uint256 me{protx.at(0)};
    // A target this node is a sentinel of, and three reports by other sentinels.
    uint256 target;
    for (size_t i = 1; i < protx.size() && target.IsNull(); ++i) {
        const auto sentinels = Sentinels(protx[i]);
        if (std::find(sentinels.begin(), sentinels.end(), me) != sentinels.end()) target = protx[i];
    }
    BOOST_REQUIRE(!target.IsNull());
    std::vector<std::pair<uint256, uint256>> others;
    for (const auto& key : Assigned()) {
        if (key.second != me && others.size() < 3) others.push_back(key);
    }
    BOOST_REQUIRE_EQUAL(others.size(), 3U);
    Prime(*peer_a, others.at(0));
    MineTo(ACTIVATION_HEIGHT + 17);
    BOOST_REQUIRE(!dslman->HasResponded(target));
    Observe();
    // Position 18's tick stops once it has reserved the epoch's emission: its event has begun, this node's reports are
    // not emitted yet and its observation is not marked -- the last point before the mark (SetDSLEmitTestHookForTest).
    StopPoint& tick = StopPoint::Of<2>();
    tick.Reset();
    SetDSLEmitTestHookForTest([] { StopPoint::Of<2>().Stop(); });
    // Whatever happens below, the hook is taken away and the tick let go -- and out of the hook -- before the fixture
    // waits for the validation queue.
    struct Guard {
        ~Guard()
        {
            SetDSLEmitTestHookForTest(nullptr);
            StopPoint::Of<2>().Done();
        }
    };
    {
        const Guard guard;
        MineBlock(m_node, CScript{} << OP_TRUE);
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Height()), ACTIVATION_HEIGHT + 18);
        BOOST_REQUIRE(tick.WaitReached(60s));
        // A lone young report with a pending job: before the mark it is not held, but taken in arrival order...
        Queue(*peer_a, Report(others.at(1).first, others.at(1).second));
        Process(*peer_a);
        BOOST_CHECK(dslman->Store().HaveReport(EPOCH, others.at(1).first, others.at(1).second));
        BOOST_CHECK_EQUAL(Poll()[PerfMetric::GATE_HOLD], 0U);
        // ...so the target's announcement, arriving behind it, is read in the very next round, before the emission.
        Queue(*peer_a, Announcement(target));
        Process(*peer_a);
        BOOST_CHECK(dslman->HasResponded(target));
        tick.Release();
    }
    SyncWithValidationInterfaceQueue();
    // The tick ran: this node's own signed report on the target says ONLINE.
    std::optional<uint8_t> own_status;
    for (const auto& bytes : Relayed()) {
        CDataStream stream{bytes, SER_NETWORK, PROTOCOL_VERSION};
        dsl::CPoSeServiceReport report;
        stream >> report;
        if (report.nEpoch == EPOCH && report.sentinelProTxHash == me && report.targetProTxHash == target) {
            BOOST_CHECK(report.VerifySig(KeyOf(me).GetPublicKey(), base->GetBlockHash()));
            own_status = report.status;
        }
    }
    BOOST_REQUIRE(own_status);
    BOOST_CHECK_EQUAL(static_cast<int>(*own_status), static_cast<int>(dsl::ServiceStatus::ONLINE));
    // And its observation is marked: the window is open now, and the same kind of report is held.
    Queue(*peer_a, Report(others.at(2).first, others.at(2).second));
    Process(*peer_a);
    BOOST_CHECK(!dslman->Store().HaveReport(EPOCH, others.at(2).first, others.at(2).second));
    BOOST_CHECK_EQUAL(Poll()[PerfMetric::GATE_HOLD], 1U);
    RunJobs();
}

BOOST_FIXTURE_TEST_CASE(a_masternode_storing_its_own_report_drops_the_jobs_of_its_key_not_started, PreverifyActiveSetup)
{
    const uint256 me{protx.at(0)};
    // A target this node is a sentinel of; nobody announces it, so this node's own report on it says MISSED.
    uint256 target;
    for (size_t i = 1; i < protx.size() && target.IsNull(); ++i) {
        const auto sentinels = Sentinels(protx[i]);
        if (std::find(sentinels.begin(), sentinels.end(), me) != sentinels.end()) target = protx[i];
    }
    BOOST_REQUIRE(!target.IsNull());
    std::optional<std::pair<uint256, uint256>> other;
    for (const auto& key : Assigned()) {
        if (key.second != me && !other) other = key;
    }
    BOOST_REQUIRE(other);
    Prime(*peer_a, *other);
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    MineTo(ACTIVATION_HEIGHT + 17);
    // A report of this node's own key -- signed with its operator key elsewhere, saying ONLINE -- reaches it before its
    // tick emits: it gets a job, which does not start.
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(target, me, dsl::ServiceStatus::ONLINE));
    Process(*peer_a); // the scan gives the report its job, and the ping
    // Position 18: the tick emits and stores this node's own report on the target, and the job of that key is dropped.
    MineTo(ACTIVATION_HEIGHT + 18);
    BOOST_REQUIRE(dslman->Store().HaveReport(EPOCH, target, me));
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    // The peer's report goes the way of a report that never had a job: a duplicate of the key, refused before its
    // signature.
    Process(*peer_a);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BLS], 0U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::DUPLICATE], 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(dsl_preverify_off_tests)

BOOST_FIXTURE_TEST_CASE(the_same_sequence_ends_the_same_with_no_preverifier, PreverifyOffSetup)
{
    BOOST_CHECK_EQUAL(RunEquivalenceScript(*this, /*run_jobs=*/false), EXPECTED_SCRIPT);
}

BOOST_FIXTURE_TEST_CASE(the_thread_warms_the_cache_while_the_store_goes_on, PreverifyThreadSetup)
{
    const auto keys = Assigned();
    Prime(*peer_a, keys.at(0));
    // The dslverify thread's signature check, stopped and watched (dsl::SetSigcacheTestHook): the thread stops inside
    // the BLS check of the entry it missed, which the hook is part of, until it is released; then it tells that its
    // entry is in the cache. Whatever would surround the BLS check surrounds the stop as well.
    StopPoint& check = StopPoint::Of<1>();
    check.Reset();
    dsl::SetSigcacheTestHook([](dsl::VerifyContext context, bool inserted) {
        if (context != dsl::VerifyContext::PREVERIFY) return;
        if (inserted) {
            StopPoint::Of<1>().Signal();
        } else {
            StopPoint::Of<1>().Stop();
        }
    });
    // Whatever happens below, the hook is taken away and the thread let go -- and out of the hook -- before the fixture
    // stops the thread.
    struct Guard {
        ~Guard()
        {
            dsl::SetSigcacheTestHook(nullptr);
            StopPoint::Of<1>().Done();
        }
    };
    {
        const Guard guard;
        Observe();
        QueuePing(*peer_a);
        Queue(*peer_a, Report(keys.at(1).first, keys.at(1).second));
        Process(*peer_a); // the job is offered, and the dslverify thread starts it
        // The thread is inside its signature check now, stopped there.
        BOOST_REQUIRE(check.WaitReached(60s));
        // Meanwhile another report is stored -- through the same cache, with a signature check of its own -- and read
        // back: the thread holds no lock that either needs.
        const auto other = Report(keys.at(2).first, keys.at(2).second);
        auto stored = std::async(std::launch::async, [&] {
            return dslman->ProcessReport(other, dmnman->GetListForBlock(base), base->GetBlockHash(), Params().GetConsensus()) &&
                   dslman->Store().HaveReport(EPOCH, other.targetProTxHash, other.sentinelProTxHash);
        });
        const bool went_on{stored.wait_for(30s) == std::future_status::ready};
        BOOST_CHECK_MESSAGE(went_on, "the store waited for the thread's signature check");
        // The thread goes on, and tells when its entry is in the cache.
        check.Release();
        BOOST_CHECK(stored.get());
        BOOST_REQUIRE(check.WaitSignalled(60s));
    }
    // The report the thread warmed is admitted from the cache.
    Process(*peer_a);
    BOOST_CHECK(dslman->Store().HaveReport(EPOCH, keys.at(1).first, keys.at(1).second));
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_BLS], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::BLS_VERIFY], 1U); // the other report's, checked where it was taken in
    BOOST_CHECK_EQUAL(tally[PerfMetric::CACHE_HIT], 1U);
    BOOST_CHECK_EQUAL(tally[PerfMetric::NET_ADMIT_CACHE_MISS], 0U);
}

BOOST_FIXTURE_TEST_CASE(the_same_sequence_ends_the_same_with_a_thread, PreverifyThreadSetup)
{
    BOOST_CHECK_EQUAL(RunEquivalenceScript(*this, /*run_jobs=*/false), EXPECTED_SCRIPT);
}

BOOST_FIXTURE_TEST_CASE(under_blocksonly_only_a_connection_allowed_to_relay_gets_jobs, PreverifyBlocksOnlySetup)
{
    CNode& relay = AddPeer(23, ConnectionType::OUTBOUND_FULL_RELAY, NetPermissionFlags::Relay);
    const auto keys = Assigned();
    Prime(relay, keys.at(0));
    QueuePing(relay);
    Queue(relay, Report(keys.at(1).first, keys.at(1).second));
    Process(relay);
    BOOST_CHECK_EQUAL(RunJobs(), 1U);
    // Without the relay permission: no job, and the ordinary path disconnects it for the report, as before.
    peerman->SetDSLTokensForTest(peer_a->GetId(), 100.0);
    Observe();
    QueuePing(*peer_a);
    Queue(*peer_a, Report(keys.at(2).first, keys.at(2).second));
    Process(*peer_a);
    BOOST_CHECK_EQUAL(RunJobs(), 0U);
    Poll();
    BOOST_CHECK_EQUAL(tally[PerfMetric::PREVERIFY_EXCLUDED] + tally[PerfMetric::PREVERIFY_BUDGET_SHORT], 0U);
    Process(*peer_a);
    BOOST_CHECK(peer_a->fDisconnect);
}

BOOST_AUTO_TEST_SUITE_END()
