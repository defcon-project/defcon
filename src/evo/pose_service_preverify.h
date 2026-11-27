// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_POSE_SERVICE_PREVERIFY_H
#define BITCOIN_EVO_POSE_SERVICE_PREVERIFY_H

#include <bls/bls.h>
#include <net.h>
#include <sync.h>
#include <uint256.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

class CBlockIndex;

namespace dsl {

/**
 * One pre-verification job: the bytes of one received POSEREPORT, the operator key and the base block hash it is to
 * be checked against. It is the verifier's own object. The report's queue entry keeps only the job's state (`prep`,
 * a NetMessagePrep), so the job is freed as soon as the verifier no longer holds it and its run, if one is under way,
 * has ended -- however long its report stays queued.
 *
 * The state moves PENDING -> RUNNING -> DONE on the verifier's thread; PENDING -> TAKEN when the receive queue hands the
 * report out to the message thread first (CNode::PollMessage, as it takes the report off the queue); PENDING -> DROPPED
 * when the verifier drops a job that has not started (to make room, the peer gone, the store holding a report of its
 * key, or the verifier stopped). Whoever moves it out of PENDING owns the report's test wait (-dsltestprocessdelay):
 * the verifier's thread if it started the job, the message thread otherwise.
 */
struct PreverifyJob {
    //! The state, shared with the report's queue entry.
    const std::shared_ptr<NetMessagePrep> prep{std::make_shared<NetMessagePrep>()};
    // Set before the offer.
    std::vector<unsigned char> payload;
    CBLSPublicKey operator_key;
    uint32_t epoch{0};
    uint256 base_hash;
    uint256 dup_key; //!< the key of (the bytes, the base) in the index of jobs held
    uint256 store_key; //!< the key of (the epoch, the target, the sentinel): what the store holds one report of
    int64_t peer{-1};
    std::chrono::steady_clock::time_point queued;

    uint8_t State() const { return prep->state.load(); }
    //! The message thread takes the report: PENDING -> TAKEN. False if the job started, or was dropped, first.
    bool TryTake() { return prep->TryMove(NetMessagePrep::TAKEN); }
    //! The verifier's thread starts the job: PENDING -> RUNNING. False if the report was taken first.
    bool TryRun() { return prep->TryMove(NetMessagePrep::RUNNING); }
    //! The verifier drops a job that has not started: PENDING -> DROPPED.
    bool TryDrop() { return prep->TryMove(NetMessagePrep::DROPPED); }

private:
    friend class ReportPreverifier;
    // All guarded by the verifier's mutex.
    bool m_held{false};    //!< counts against the limits; in the indexes and in its peer's list
    bool m_waiting{false}; //!< on its peer's line of waiting jobs
    std::list<std::shared_ptr<PreverifyJob>>::iterator m_held_at; //!< its place in the peer's list of held jobs
    std::list<std::shared_ptr<PreverifyJob>>::iterator m_wait_at; //!< its place on the peer's line
    std::multimap<uint256, std::shared_ptr<PreverifyJob>>::iterator m_store_at; //!< its place in the index by store key
};

/**
 * Checks the signatures of received Sentinel reports ahead of their processing, on a thread of its own (dslverify),
 * into the report signature cache. That is its only effect: the message thread still processes every report in
 * order with the unchanged code (ProcessDSLMessage -> AddReport), which finds the result in the cache.
 *
 * Jobs are served the way the message thread serves its peers: one line per peer, one job from each line in turn,
 * each line in the order of its offers. The peers with a waiting job take turns in a queue: a peer whose line gets a
 * waiting job goes to the back of it, and a peer served goes to the back again if it still has one; so a waiting peer
 * is served before any peer that came after it, however many come.
 *
 * A report the store holds makes the jobs of its key -- the epoch, the target and the sentinel, of which the store
 * keeps one report -- useless: the ordinary path refuses another report of that key before its signature, and a copy
 * of the same bytes unread. The store calls Stored() under its own lock just before it holds such a report (its insert
 * observer), and the scan offers a job only under that lock while the key is absent (CServiceReportStore::IfAbsent).
 * So a job of the key either started before the store held the report, and finishes, or never starts.
 *
 * Limits: a job is held from its offer until its report is taken (Taken), it is dropped, or its peer is gone
 * (DropPeer) -- waiting, running and done alike. A peer may hold at most `per_peer` jobs, all peers together at most
 * `total`. When exactly `total` are held, the newest waiting job of the peer that holds the most makes room for the
 * new one, in the same step; if no other peer holds more than the offering one, or none of theirs waits, the new one is
 * refused. Above `total` (after the limits were lowered) nothing is taken in until the held jobs drop below it. A job
 * is in the index of (bytes, base) for as long as it is held, so another copy of the same bytes on the same base gets
 * no job meanwhile, and a dropped job's key leaves the index with it. A job that is no longer held is freed, with
 * its payload and key, once its run (if any) has ended; its report's queue entry keeps only the state.
 *
 * Offering, taking, running and releasing a job cost constant or logarithmic bookkeeping, in the jobs held and the
 * peers that hold them, and a peer's departure costs in proportion to its own jobs; Stored() costs a logarithm and
 * one drop for each job of the key. Two steps look at each peer's line once: making room at the total limit (Offer),
 * and a change of the limits (SetLimits), each of whose drops then costs a logarithm of the peers. The work function
 * sees nothing but the job. Thread-safe.
 */
class ReportPreverifier
{
public:
    using WorkFn = std::function<void(PreverifyJob&)>;
    using JobRef = std::shared_ptr<PreverifyJob>;

    /** `work` decodes and checks one job. With `threaded` the first Offer starts the thread; without it nothing runs
     *  until RunOne() is called (tests). */
    explicit ReportPreverifier(WorkFn work, bool threaded = true);
    ~ReportPreverifier();
    ReportPreverifier(const ReportPreverifier&) = delete;
    ReportPreverifier& operator=(const ReportPreverifier&) = delete;

    /** New limits. Waiting jobs above them are dropped at once (a peer's newest first, from the peer above its limit,
     *  then from the peer that holds the most); running and done jobs above them run out as their reports are taken.
     *  The limits it already has change nothing and cost nothing. */
    void SetLimits(size_t per_peer, size_t total) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    enum class OfferResult {
        QUEUED,      //!< the job waits for the thread
        DUP_PENDING, //!< a job for the same bytes on the same base is still held; no job
        QUEUE_FULL,  //!< a limit refused it; no job
        STOPPED,     //!< the thread is stopped or could not start; no job
    };
    /** Hands in a job in state PENDING. On QUEUED `evicted` tells whether another peer's waiting job was dropped to
     *  make room (its report goes the way of a report that never had a job). */
    OfferResult Offer(const JobRef& job, bool* evicted = nullptr) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! The message thread took the report whose queue entry carries `prep` (whatever the state): its job, if it
    //! is still held, is held no longer.
    void Taken(const NetMessagePrep& prep) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The peer is gone: none of its jobs is held any more, and the waiting ones are dropped (a running one
    //! finishes on its own).
    void DropPeer(int64_t peer) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The store is about to hold a report of `store_key` (it calls this under its own lock): the jobs of that key that
    //! have not started are dropped (their reports go the way of reports that never had a job). Returns how many.
    size_t Stored(const uint256& store_key) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Runs the next job in turn on the calling thread; false if none waits. For a verifier without a thread.
    bool RunOne() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Stops the thread: the running job finishes, the waiting ones are dropped, and nothing is offered after it.
    //! Every caller returns only once the thread has ended, a second one that calls meanwhile too.
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_stop_mutex, !m_mutex);

    //! Jobs held (waiting, running or done, their report not taken), all of them and of one peer.
    size_t Held() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t HeldBy(int64_t peer) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Jobs waiting for the thread.
    size_t Waiting() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Whether the thread failed to start (the caller stops offering).
    bool Failed() const { return m_failed.load(); }

private:
    struct Line {
        std::list<JobRef> waiting; //!< the peer's waiting jobs, oldest first
        std::list<JobRef> held;    //!< every job the peer holds
        std::list<int64_t>::iterator turn_at; //!< its place in the turns, while `waiting` is not empty
    };

    //! The next waiting job in turn, now RUNNING; nullptr if none.
    JobRef TakeNext() EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void Run(PreverifyJob& job) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Takes a job off its line, if it is on one. The caller holds a reference of its own to the job.
    void Unwait(PreverifyJob& job) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! The job is held no longer: out of the indexes and its peer's list. By value: the caller's reference may be
    //! the very one in a list this erases.
    void Release(JobRef job) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! Drops a waiting job: off its line, DROPPED (unless its report was just taken), released. By value, as Release.
    void DropWaiting(JobRef job) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    //! The peer, other than `peer`, that holds the most and has a waiting job, if it holds more than `peer`.
    Line* MostHolding(int64_t peer) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void DropAllWaiting() EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void ThreadMain() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    const WorkFn m_work;
    const bool m_threaded;
    std::atomic<bool> m_failed{false};

    Mutex m_stop_mutex; //!< one Stop at a time; taken before m_mutex
    mutable Mutex m_mutex;
    std::condition_variable m_cv;
    std::map<uint256, JobRef> m_index GUARDED_BY(m_mutex);                 //!< the jobs held, by dup_key
    std::map<const NetMessagePrep*, JobRef> m_by_prep GUARDED_BY(m_mutex); //!< the jobs held, by their state object
    std::multimap<uint256, JobRef> m_by_store GUARDED_BY(m_mutex);         //!< the jobs held, by store_key
    std::map<int64_t, Line> m_lines GUARDED_BY(m_mutex);                   //!< the peers that hold a job
    std::list<int64_t> m_turns GUARDED_BY(m_mutex); //!< the peers with a waiting job, the next one to serve in front
    size_t m_held GUARDED_BY(m_mutex){0};
    size_t m_waiting GUARDED_BY(m_mutex){0};
    size_t m_per_peer GUARDED_BY(m_mutex){1};
    size_t m_total GUARDED_BY(m_mutex){1};
    bool m_stop GUARDED_BY(m_mutex){false};
    bool m_started GUARDED_BY(m_mutex){false};
    std::thread m_thread GUARDED_BY(m_mutex);
};

/** One pre-verification job, the work of the dslverify thread (ReportPreverifier's WorkFn): the report's test wait
 *  (-dsltestprocessdelay; this thread started the job, so the wait is this thread's, before the reading as in
 *  ReadDSLMessage), its reading, and the check of its signature, whose only effect is the signature cache's entry for a
 *  valid one. It touches nothing but the job: no store, list, chain or connection. Throws on an unreadable report. */
void PreverifyReport(PreverifyJob& job);

/**
 * This node's own observation of a Sentinel epoch, as the wave window reads it.
 *
 * Validation notifies every new tip twice, together and under cs_main: at once (SynchronousUpdatedBlockTip), and
 * through its queue of events (UpdatedBlockTip), which runs them in the order they were queued. OnSyncTip takes the
 * first: a tip that does not descend from the tip notified before -- a rewind or a branch switch -- clears the mark
 * and moves the generation on; one that stays or descends keeps both. It also puts the tip, with the generation it
 * leaves behind, at the end of the line of events that have not run yet. EventBegin takes the second, at the start of
 * every such event, whether or not a Sentinel tick follows it (none does during the initial block download): the
 * entry in front of the line is this event's own, it leaves the line, and its generation is the one the event's tick
 * may mark with. So neither a tick that began before a rewind nor one queued before it -- even for a block
 * disconnected and connected again since -- can mark the newer generation. Only the tick that emitted and relayed the
 * epoch's reports marks it (Publish), and only if its generation is still current.
 *
 * The line holds at most MAX_PENDING_EVENTS entries: a newer one pushes the oldest out, whose event -- it still runs
 * before the others -- then gets no generation and takes nobody else's entry. An event whose tip is not the one in
 * front of the line has no entry here: it gets no generation either, and leaves the line as it is.
 *
 * The events queued before this object's first notification -- before it listened, a queued event still reaches it
 * when it runs -- have no entry either, even one for the very block of that notification. So the first notification
 * asks its caller to queue Arm() in the validation queue (OnSyncTip returns true): there it lands behind every event
 * queued before and ahead of the notification's own, which validation queues right after it. Until Arm() has run, an
 * event takes no entry and gets no generation. NotifyObservationMark() does both steps of a notification. Thread-safe;
 * a leaf lock.
 */
class ObservationMark
{
public:
    //! At most this many notified tips wait for their events.
    static constexpr size_t MAX_PENDING_EVENTS{1024};

    explicit ObservationMark(const CBlockIndex* tip = nullptr);
    //! True for the first notification: the caller queues Arm() in the validation queue before the event of it.
    bool OnSyncTip(const CBlockIndex* tip) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Every event queued before the first notification has run: the events from here on have their entries.
    void Arm() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The start of the event queued with a notification of `tip`: the generation that notification left behind, if
    //! its entry is still on the line.
    std::optional<uint64_t> EventBegin(const CBlockIndex* tip) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Publish(uint64_t generation, uint32_t epoch, const uint256& base) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The active tip is the tip last notified or descends from it: no rewind is under way.
    bool NotRewinding(const CBlockIndex* active_tip) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The observation of (epoch, base) was marked done in the current generation.
    bool Done(uint32_t epoch, const uint256& base) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    uint64_t Generation() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    struct Mark {
        bool valid{false};
        uint32_t epoch{0};
        uint256 base;
    };
    mutable Mutex m_mutex;
    const CBlockIndex* m_sync_tip GUARDED_BY(m_mutex);
    uint64_t m_generation GUARDED_BY(m_mutex){0};
    Mark m_mark GUARDED_BY(m_mutex);
    std::deque<std::pair<const CBlockIndex*, uint64_t>> m_events GUARDED_BY(m_mutex);
    size_t m_lost GUARDED_BY(m_mutex){0}; //!< entries pushed out whose events have not run yet
    bool m_arm_asked GUARDED_BY(m_mutex){false}; //!< the first notification came, and asked for Arm()
    bool m_armed GUARDED_BY(m_mutex){false};     //!< Arm() ran: the events queued before the first notification are done
};

/** A synchronous tip notification to `mark` (SynchronousUpdatedBlockTip): OnSyncTip, and for the first one Arm()
 *  queued in the validation queue, which keeps `mark` alive until it runs. */
void NotifyObservationMark(const std::shared_ptr<ObservationMark>& mark, const CBlockIndex* tip);

} // namespace dsl

#endif // BITCOIN_EVO_POSE_SERVICE_PREVERIFY_H
