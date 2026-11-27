// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/pose_service_preverify.h>

#include <chain.h>
#include <evo/pose_service_metrics.h>
#include <evo/pose_service_sentinels.h>
#include <evo/pose_service_sigcache.h>
#include <streams.h>
#include <version.h>
#include <logging.h>
#include <util/thread.h>
#include <validationinterface.h>

#include <algorithm>
#include <queue>
#include <system_error>

namespace dsl {

ReportPreverifier::ReportPreverifier(WorkFn work, bool threaded) : m_work(std::move(work)), m_threaded(threaded) {}

ReportPreverifier::~ReportPreverifier() { Stop(); }

void ReportPreverifier::SetLimits(size_t per_peer, size_t total)
{
    LOCK(m_mutex);
    per_peer = std::max<size_t>(1, per_peer);
    total = std::max<size_t>(1, total);
    // The same limits again: nothing is above them, as Offer takes nothing in above them.
    if (per_peer == m_per_peer && total == m_total) return;
    m_per_peer = per_peer;
    m_total = total;
    // Waiting jobs above the new limits go now: first those of a peer above its own limit, newest first...
    std::vector<JobRef> over;
    for (auto& [id, line] : m_lines) {
        auto job = line.waiting.rbegin();
        for (size_t excess = line.held.size() > m_per_peer ? line.held.size() - m_per_peer : 0;
             excess > 0 && job != line.waiting.rend(); --excess, ++job) {
            over.push_back(*job);
        }
    }
    for (const JobRef& job : over) DropWaiting(job);
    if (m_held <= m_total) return;
    // ...then, while the total is above its limit, the newest waiting job of the peer that holds the most (of two that
    // hold as much, the one with the smaller id). One look at each line, then a logarithm of the lines for each drop.
    using Holding = std::pair<size_t, int64_t>; // jobs held, peer
    const auto fewer = [](const Holding& a, const Holding& b) {
        return a.first != b.first ? a.first < b.first : a.second > b.second;
    };
    std::priority_queue<Holding, std::vector<Holding>, decltype(fewer)> most{fewer};
    for (const auto& [id, line] : m_lines) {
        if (!line.waiting.empty()) most.emplace(line.held.size(), id);
    }
    while (m_held > m_total && !most.empty()) {
        const int64_t peer{most.top().second};
        most.pop();
        DropWaiting(m_lines.find(peer)->second.waiting.back());
        // The line went with its last held job; one that still has a waiting job may be the next again.
        const auto line = m_lines.find(peer);
        if (line != m_lines.end() && !line->second.waiting.empty()) most.emplace(line->second.held.size(), peer);
    }
    // What is still above the total runs or is done: it runs out as its reports are taken.
}

ReportPreverifier::OfferResult ReportPreverifier::Offer(const JobRef& job, bool* evicted)
{
    if (evicted != nullptr) *evicted = false;
    {
        LOCK(m_mutex);
        if (m_stop || m_failed.load()) return OfferResult::STOPPED;
        if (m_index.count(job->dup_key)) return OfferResult::DUP_PENDING;
        if (const auto line = m_lines.find(job->peer); line != m_lines.end() && line->second.held.size() >= m_per_peer) {
            return OfferResult::QUEUE_FULL;
        }
        if (m_held > m_total) return OfferResult::QUEUE_FULL; // above lowered limits: no exchange keeps it there
        if (m_held == m_total) {
            // Room is made in the same step as the job is taken in, or not at all.
            Line* victim = MostHolding(job->peer);
            if (victim == nullptr) return OfferResult::QUEUE_FULL;
            const JobRef dropped = victim->waiting.back(); // its newest waiting job
            LogPrint(BCLog::NET, "DSL preverify -- a waiting job of peer=%d dropped to make room for peer=%d\n",
                     dropped->peer, job->peer);
            DropWaiting(dropped);
            if (evicted != nullptr) *evicted = true;
        }
        if (m_threaded && !m_started) {
            try {
                m_thread = std::thread(&util::TraceThread, "dslverify", [this] { ThreadMain(); });
                m_started = true;
            } catch (const std::system_error& e) {
                LogPrintf("DSL preverify -- the dslverify thread did not start (%s): reports are checked where they are processed\n", e.what());
                m_failed = true;
                return OfferResult::STOPPED;
            }
        }
        job->queued = std::chrono::steady_clock::now();
        Line& line = m_lines[job->peer];
        job->m_held = true;
        job->m_held_at = line.held.insert(line.held.end(), job);
        job->m_waiting = true;
        job->m_wait_at = line.waiting.insert(line.waiting.end(), job);
        // A line that had no waiting job joins the turns at the back.
        if (line.waiting.size() == 1) line.turn_at = m_turns.insert(m_turns.end(), job->peer);
        m_index.emplace(job->dup_key, job);
        m_by_prep.emplace(job->prep.get(), job);
        job->m_store_at = m_by_store.emplace(job->store_key, job);
        ++m_held;
        ++m_waiting;
    }
    m_cv.notify_one();
    return OfferResult::QUEUED;
}

ReportPreverifier::Line* ReportPreverifier::MostHolding(int64_t peer)
{
    AssertLockHeld(m_mutex);
    // The offering peer wins every tie, so two peers holding as much never take turns at dropping each other's jobs.
    size_t most{0};
    if (const auto it = m_lines.find(peer); it != m_lines.end()) most = it->second.held.size();
    Line* victim{nullptr};
    for (auto& [id, line] : m_lines) {
        if (id == peer || line.waiting.empty()) continue;
        if (line.held.size() > most) {
            most = line.held.size();
            victim = &line;
        }
    }
    return victim;
}

void ReportPreverifier::Unwait(PreverifyJob& job)
{
    AssertLockHeld(m_mutex);
    if (!job.m_waiting) return;
    job.m_waiting = false;
    --m_waiting;
    // A waiting job is held, and a held job's line is there.
    Line& line = m_lines.find(job.peer)->second;
    line.waiting.erase(job.m_wait_at);
    if (line.waiting.empty()) m_turns.erase(line.turn_at); // its last waiting job: out of the turns
}

void ReportPreverifier::DropWaiting(JobRef job)
{
    AssertLockHeld(m_mutex);
    Unwait(*job);
    // The message thread may have taken its report a moment ago (its TryMove needs no mutex); then this fails, and
    // the job is released all the same.
    job->TryDrop();
    Release(job);
}

void ReportPreverifier::Release(JobRef job)
{
    AssertLockHeld(m_mutex);
    if (!job->m_held) return;
    job->m_held = false;
    --m_held;
    // The verifier's own references go. After them the job lives only as long as this call and, if it is running,
    // its run: then it is freed, payload and key with it, although its report's queue entry may keep the state (prep)
    // a while longer.
    if (const auto it = m_index.find(job->dup_key); it != m_index.end() && it->second == job) m_index.erase(it);
    m_by_prep.erase(job->prep.get());
    m_by_store.erase(job->m_store_at);
    const auto line = m_lines.find(job->peer);
    line->second.held.erase(job->m_held_at);
    if (line->second.held.empty()) m_lines.erase(line); // an empty held list leaves no waiting job behind
}

void ReportPreverifier::Taken(const NetMessagePrep& prep)
{
    LOCK(m_mutex);
    const auto it = m_by_prep.find(&prep);
    if (it == m_by_prep.end()) return; // released before: dropped, or its peer gone
    const JobRef job = it->second;
    Unwait(*job); // a job the message thread took before it started is no longer waiting
    Release(job);
}

void ReportPreverifier::DropPeer(int64_t peer)
{
    LOCK(m_mutex);
    const auto it = m_lines.find(peer);
    if (it == m_lines.end()) return;
    // Every job the peer holds -- waiting, running or done -- is released, in proportion to its own: its queue goes
    // with it, and nothing would take those reports any more.
    const std::vector<JobRef> held{it->second.held.begin(), it->second.held.end()};
    for (const JobRef& job : held) {
        if (job->m_waiting) {
            DropWaiting(job);
        } else {
            Release(job);
        }
    }
}

size_t ReportPreverifier::Stored(const uint256& store_key)
{
    LOCK(m_mutex);
    // The waiting ones first, as Release takes each out of the index this walks.
    std::vector<JobRef> waiting;
    const auto [first, last] = m_by_store.equal_range(store_key);
    for (auto it = first; it != last; ++it) {
        if (it->second->m_waiting) waiting.push_back(it->second);
    }
    for (const JobRef& job : waiting) DropWaiting(job);
    if (!waiting.empty()) {
        LogPrint(BCLog::NET, "DSL preverify -- %d waiting job(s) dropped: the store holds a report of their key\n",
                 waiting.size());
    }
    return waiting.size();
}

ReportPreverifier::JobRef ReportPreverifier::TakeNext()
{
    AssertLockHeld(m_mutex);
    // The peer in front of the turns; it goes to the back if it still has a waiting job.
    while (!m_turns.empty()) {
        Line& line = m_lines.find(m_turns.front())->second;
        JobRef job = line.waiting.front();
        Unwait(*job); // takes the peer out of the turns if this was its last waiting job
        if (!line.waiting.empty()) m_turns.splice(m_turns.end(), m_turns, line.turn_at);
        // A report the message thread took a moment ago (its TryMove needs no mutex) is not run; Taken releases it.
        if (job->TryRun()) return job;
    }
    return nullptr;
}

void ReportPreverifier::Run(PreverifyJob& job)
{
    const auto start = std::chrono::steady_clock::now();
    RecordPerf(job.epoch, job.base_hash, PerfMetric::PREVERIFY_WAIT,
               static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(start - job.queued).count()));
    {
        PerfTimer run_timer(job.epoch, job.base_hash, PerfMetric::PREVERIFY_RUN);
        try {
            m_work(job);
        } catch (const std::exception& e) {
            // The cache stays as it was; the report is checked where it is processed.
            LogPrint(BCLog::NET, "DSL preverify -- job of peer=%d failed: %s\n", job.peer, e.what());
        }
    }
    job.prep->state.store(NetMessagePrep::DONE);
}

bool ReportPreverifier::RunOne()
{
    JobRef job;
    {
        LOCK(m_mutex);
        job = TakeNext();
    }
    if (!job) return false;
    Run(*job);
    return true;
}

void ReportPreverifier::ThreadMain()
{
    while (true) {
        JobRef job;
        {
            WAIT_LOCK(m_mutex, lock);
            m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_stop || m_waiting > 0; });
            if (m_stop) return;
            job = TakeNext();
        }
        if (job) Run(*job);
    }
}

void ReportPreverifier::DropAllWaiting()
{
    AssertLockHeld(m_mutex);
    std::vector<JobRef> waiting;
    for (auto& [id, line] : m_lines) waiting.insert(waiting.end(), line.waiting.begin(), line.waiting.end());
    for (const JobRef& job : waiting) DropWaiting(job);
}

void ReportPreverifier::Stop()
{
    // One at a time: whoever calls while another is stopping waits here until the thread has ended.
    LOCK(m_stop_mutex);
    std::thread thread;
    {
        LOCK(m_mutex);
        m_stop = true;
        DropAllWaiting();
        // Joined outside the mutex: the running job finishes first.
        thread = std::move(m_thread);
    }
    m_cv.notify_all();
    if (thread.joinable()) thread.join();
}

size_t ReportPreverifier::Held() const
{
    LOCK(m_mutex);
    return m_held;
}

size_t ReportPreverifier::HeldBy(int64_t peer) const
{
    LOCK(m_mutex);
    const auto it = m_lines.find(peer);
    return it == m_lines.end() ? 0 : it->second.held.size();
}

size_t ReportPreverifier::Waiting() const
{
    LOCK(m_mutex);
    return m_waiting;
}

void PreverifyReport(PreverifyJob& job)
{
    TestProcessDelay();
    CDataStream stream(job.payload, SER_NETWORK, PROTOCOL_VERSION);
    CPoSeServiceReport report;
    stream >> report;
    const ScopedVerifyContext context(VerifyContext::PREVERIFY);
    (void)report.VerifySig(job.operator_key, job.base_hash);
}

ObservationMark::ObservationMark(const CBlockIndex* tip) : m_sync_tip(tip) {}

bool ObservationMark::OnSyncTip(const CBlockIndex* tip)
{
    LOCK(m_mutex);
    const bool first{!m_arm_asked};
    m_arm_asked = true;
    // Against the tip notified before, not the fork point: a tip that stays (an off-chain block invalidated) or
    // descends keeps the mark; any other moves the generation on.
    if (tip != m_sync_tip) {
        const bool advance{m_sync_tip != nullptr && tip != nullptr && tip->GetAncestor(m_sync_tip->nHeight) == m_sync_tip};
        if (!advance) {
            m_mark = Mark{};
            ++m_generation;
        }
        m_sync_tip = tip;
    }
    // Every notification queues an event: this is the generation that event belongs to.
    m_events.emplace_back(tip, m_generation);
    if (m_events.size() > MAX_PENDING_EVENTS) {
        m_events.pop_front();
        ++m_lost;
    }
    return first;
}

void ObservationMark::Arm()
{
    LOCK(m_mutex);
    m_armed = true;
}

std::optional<uint64_t> ObservationMark::EventBegin(const CBlockIndex* tip)
{
    LOCK(m_mutex);
    if (!m_armed) return std::nullopt; // queued before this object's first notification: no entry is its own
    // Events run in the order of their notifications, so those of the entries pushed out come first.
    if (m_lost > 0) {
        --m_lost;
        return std::nullopt;
    }
    // An event with no notification on the line takes no entry.
    if (m_events.empty() || m_events.front().first != tip) return std::nullopt;
    const uint64_t generation{m_events.front().second};
    m_events.pop_front();
    return generation;
}

void ObservationMark::Publish(uint64_t generation, uint32_t epoch, const uint256& base)
{
    LOCK(m_mutex);
    if (generation != m_generation) return; // a rewind or branch switch came between: this observation is gone
    m_mark = Mark{/*valid=*/true, epoch, base};
}

bool ObservationMark::NotRewinding(const CBlockIndex* active_tip) const
{
    LOCK(m_mutex);
    return m_sync_tip != nullptr && active_tip != nullptr && active_tip->GetAncestor(m_sync_tip->nHeight) == m_sync_tip;
}

bool ObservationMark::Done(uint32_t epoch, const uint256& base) const
{
    LOCK(m_mutex);
    // A valid mark is of the current generation: Publish refuses an older one, and whatever moves the generation on
    // (a rewind, a branch switch) clears the mark in the same step.
    return m_mark.valid && m_mark.epoch == epoch && m_mark.base == base;
}

uint64_t ObservationMark::Generation() const
{
    LOCK(m_mutex);
    return m_generation;
}

void NotifyObservationMark(const std::shared_ptr<ObservationMark>& mark, const CBlockIndex* tip)
{
    if (mark->OnSyncTip(tip)) CallFunctionInValidationInterfaceQueue([mark] { mark->Arm(); });
}

} // namespace dsl
