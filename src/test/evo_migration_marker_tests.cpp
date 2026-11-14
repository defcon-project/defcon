// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <evo/deterministicmns.h>
#include <evo/evodb.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <util/system.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <string>

BOOST_FIXTURE_TEST_SUITE(evo_migration_marker_tests, BasicTestingSetup)

// The evo database rewrites its best-block key under the newest constant during
// normal operation, so a healthy database that upgraded across versions carries
// only one late marker. A migration gate that checks a single fixed key reads
// such a database as a broken migration attempt -- which once stopped a whole
// fleet of good databases from starting. Every gate must treat its own output
// marker OR any newer one as proof the work is behind it.
BOOST_AUTO_TEST_CASE(a_newer_database_counts_as_migrated)
{
    CDBWrapper db(gArgs.GetDataDirNet() / "evodb_marker_test", 1 << 20, /*fMemory=*/true, /*fWipe=*/true);

    // an empty database: nothing is done
    for (int m : {1, 2, 3, 4}) {
        BOOST_CHECK(!CDeterministicMNManager::MigrationAlreadyDone(db, m));
    }

    // the fleet database that refused to start: only b_b5, written by normal
    // operation on the previous binary -- migrations 1..3 are behind it, and
    // migration 4 is exactly the one that must run
    db.Write(std::string{"b_b5"}, 1);
    BOOST_CHECK(CDeterministicMNManager::MigrationAlreadyDone(db, 1));
    BOOST_CHECK(CDeterministicMNManager::MigrationAlreadyDone(db, 2));
    BOOST_CHECK(CDeterministicMNManager::MigrationAlreadyDone(db, 3));
    BOOST_CHECK(!CDeterministicMNManager::MigrationAlreadyDone(db, 4));

    // after migration 4 has run, the live best-block marker closes the chain
    db.Write(EVODB_BEST_BLOCK, 1);
    BOOST_CHECK(CDeterministicMNManager::MigrationAlreadyDone(db, 4));

    // an older database still lets every later migration run, in order
    CDBWrapper db2(gArgs.GetDataDirNet() / "evodb_marker_test2", 1 << 20, /*fMemory=*/true, /*fWipe=*/true);
    db2.Write(std::string{"b_b3"}, 1);
    BOOST_CHECK(CDeterministicMNManager::MigrationAlreadyDone(db2, 1));
    BOOST_CHECK(!CDeterministicMNManager::MigrationAlreadyDone(db2, 2));
    BOOST_CHECK(!CDeterministicMNManager::MigrationAlreadyDone(db2, 3));
    BOOST_CHECK(!CDeterministicMNManager::MigrationAlreadyDone(db2, 4));
}

// The repair mark goes into the database by itself. It used to commit the
// evodb transactions on its way: the block a thread was still connecting, and
// the connected blocks the root transaction holds until the next flush of the
// coins database, reached the disk with it, ahead of the coins.
BOOST_FIXTURE_TEST_CASE(repair_mark_leaves_the_transactions_alone, TestingSetup)
{
    // The node's own evodb (LoadChainstate opens it on disk) and its manager;
    // nothing has marked the history yet.
    CEvoDB& evodb = *Assert(m_node.evodb);
    CDeterministicMNManager& dmnman = *Assert(m_node.dmnman);
    const std::string committed_key{"r09_committed"};
    const std::string open_key{"r09_open"};
    BOOST_REQUIRE(!dmnman.IsRepaired());
    BOOST_REQUIRE(!evodb.GetRawDB().Exists(committed_key));
    // a connected block: committed into the root transaction, not yet flushed
    {
        auto tx = evodb.BeginTransaction();
        evodb.Write(committed_key, 1);
        tx->Commit();
    }
    // a block still being connected: its transaction is open
    auto open = evodb.BeginTransaction();
    evodb.Write(open_key, 1);

    dmnman.CompleteRepair();
    BOOST_CHECK(dmnman.IsRepaired());
    // The database itself, which the next start reads (after a crash too),
    // holds the mark and neither block.
    BOOST_CHECK(evodb.GetRawDB().Exists(std::string{"dmn_R1"}));
    BOOST_CHECK(!evodb.GetRawDB().Exists(committed_key));
    BOOST_CHECK(!evodb.GetRawDB().Exists(open_key));
    // The open transaction is still open, with its write in it, and rolls back.
    BOOST_CHECK(!WITH_LOCK(evodb.cs, return evodb.GetCurTransaction().IsClean()));
    BOOST_CHECK(evodb.Exists(open_key));
    open->Rollback();
    BOOST_CHECK(!evodb.Exists(open_key));
    // The connected block is where it was, in the root transaction.
    BOOST_CHECK(evodb.Exists(committed_key));
    // A manager made afresh on the same database reads the mark.
    CDeterministicMNManager fresh(m_node.chainman->ActiveChainstate(), evodb);
    BOOST_CHECK(fresh.IsRepaired());
}

BOOST_AUTO_TEST_SUITE_END()
