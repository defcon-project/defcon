// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/dmnstate.h>
#include <evo/pose_service_sentinels.h>
#include <hash.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <memory>
#include <set>

BOOST_FIXTURE_TEST_SUITE(pose_service_sentinels_tests, BasicTestingSetup)

namespace {
uint256 TaggedHash(uint64_t a, uint64_t b, std::string_view tag)
{
    CHashWriter w(SER_GETHASH, 0);
    w << a << b << std::string{tag};
    return w.GetHash();
}

// A masternode list whose members are confirmed (eligible for scoring); one
// member is left unconfirmed to prove selection ignores it.
CDeterministicMNList MakeList(size_t n, size_t unconfirmed_idx)
{
    CDeterministicMNList list(uint256(), /*height=*/1, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        auto dmn = std::make_shared<CDeterministicMN>(i);
        dmn->proTxHash = TaggedHash(i, 0, "protx");
        dmn->collateralOutpoint = COutPoint(dmn->proTxHash, 0);
        auto st = std::make_shared<CDeterministicMNState>();
        st->nRegisteredHeight = 1;
        if (i != unconfirmed_idx) {
            st->UpdateConfirmedHash(dmn->proTxHash, TaggedHash(i, 0, "confirmed"));
        }
        CKeyID owner;
        std::memcpy(owner.begin(), dmn->proTxHash.begin(), owner.size());
        st->keyIDOwner = owner;
        dmn->pdmnState = st;
        list.AddMN(dmn);
    }
    return list;
}
} // namespace

BOOST_AUTO_TEST_CASE(selection_is_deterministic_and_excludes_target)
{
    const auto list = MakeList(30, /*unconfirmed_idx=*/999);
    const uint256 target = TaggedHash(3, 0, "protx");
    const uint256 epoch = TaggedHash(100, 0, "epoch");

    const auto a = dsl::CalcSentinelsForMN(list, target, epoch, 7);
    const auto b = dsl::CalcSentinelsForMN(list, target, epoch, 7);
    BOOST_CHECK_EQUAL(a.size(), 7u);
    BOOST_CHECK(a == b); // deterministic

    // the target never sentinels itself, and there are no duplicates
    std::set<uint256> uniq(a.begin(), a.end());
    BOOST_CHECK_EQUAL(uniq.size(), a.size());
    BOOST_CHECK(uniq.find(target) == uniq.end());

    // a different epoch rotates the set
    const uint256 epoch2 = TaggedHash(101, 0, "epoch");
    const auto c = dsl::CalcSentinelsForMN(list, target, epoch2, 7);
    BOOST_CHECK(a != c);
}

BOOST_AUTO_TEST_CASE(unconfirmed_masternodes_are_never_sentinels)
{
    // node 0 is unconfirmed; it must never be selected for any target/epoch.
    const auto list = MakeList(20, /*unconfirmed_idx=*/0);
    const uint256 unconfirmed = TaggedHash(0, 0, "protx");
    for (uint64_t e = 0; e < 40; ++e) {
        for (uint64_t t = 1; t < 20; ++t) {
            const auto sentinels = dsl::CalcSentinelsForMN(list, TaggedHash(t, 0, "protx"),
                                                           TaggedHash(e, 0, "epoch"), 7);
            BOOST_CHECK(std::find(sentinels.begin(), sentinels.end(), unconfirmed) == sentinels.end());
        }
    }
}

BOOST_AUTO_TEST_CASE(report_signature_roundtrips)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    const CBLSPublicKey pk = sk.GetPublicKey();

    dsl::CPoSeServiceReport rep;
    rep.nEpoch = 42;
    rep.targetProTxHash = uint256::ONE;
    rep.sentinelProTxHash = uint256::TWO;
    rep.status = static_cast<uint8_t>(dsl::ServiceStatus::MISSED);
    const uint256 base = TaggedHash(7, 0, "epoch");
    rep.Sign(sk, base);
    BOOST_CHECK(rep.VerifySig(pk, base));

    // any tampering breaks the signature
    dsl::CPoSeServiceReport tampered = rep;
    tampered.status = static_cast<uint8_t>(dsl::ServiceStatus::ONLINE);
    BOOST_CHECK(!tampered.VerifySig(pk, base));

    // a different key does not verify
    CBLSSecretKey other;
    other.MakeNewKey();
    BOOST_CHECK(!rep.VerifySig(other.GetPublicKey(), base));

    // and neither does a different epoch base -- the signature is bound to it
    BOOST_CHECK(!rep.VerifySig(pk, TaggedHash(8, 0, "epoch")));
}

// The challenge nonce is bound to the epoch base and the target, so a response
// cannot be replayed across epochs or targets.
BOOST_AUTO_TEST_CASE(challenge_nonce_binds_epoch_and_target)
{
    const uint256 e1 = uint256::ONE, e2 = uint256::TWO;
    const uint256 t1 = TaggedHash(1, 0, "t"), t2 = TaggedHash(2, 0, "t");
    BOOST_CHECK(dsl::ServiceChallengeNonce(e1, t1) == dsl::ServiceChallengeNonce(e1, t1));
    BOOST_CHECK(dsl::ServiceChallengeNonce(e1, t1) != dsl::ServiceChallengeNonce(e2, t1));
    BOOST_CHECK(dsl::ServiceChallengeNonce(e1, t1) != dsl::ServiceChallengeNonce(e1, t2));
}

BOOST_AUTO_TEST_SUITE_END()
