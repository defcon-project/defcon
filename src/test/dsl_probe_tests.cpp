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

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(dsl_probe_tests, BasicTestingSetup)

namespace {
uint256 TaggedHash(uint64_t a, uint64_t b, std::string_view tag)
{
    CHashWriter w(SER_GETHASH, 0);
    w << a << b << std::string{tag};
    return w.GetHash();
}

// A list of confirmed masternodes (eligible for scoring).
CDeterministicMNList MakeList(size_t n)
{
    CDeterministicMNList list(uint256(), /*height=*/1, static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
        auto dmn = std::make_shared<CDeterministicMN>(i);
        dmn->proTxHash = TaggedHash(i, 0, "protx");
        dmn->collateralOutpoint = COutPoint(dmn->proTxHash, 0);
        auto st = std::make_shared<CDeterministicMNState>();
        st->nRegisteredHeight = 1;
        st->UpdateConfirmedHash(dmn->proTxHash, TaggedHash(i, 0, "confirmed"));
        CKeyID owner;
        std::memcpy(owner.begin(), dmn->proTxHash.begin(), owner.size());
        st->keyIDOwner = owner;
        dmn->pdmnState = st;
        list.AddMN(dmn);
    }
    return list;
}
} // namespace

// GetProbeTargetsForSentinel is exactly the inverse of CalcSentinelsForMN:
// a node probes precisely the masternodes whose sentinel set selected it.
BOOST_AUTO_TEST_CASE(probe_targets_are_the_inverse_of_assignment)
{
    const auto list = MakeList(40);
    const uint256 epoch = TaggedHash(100, 0, "epoch");
    const size_t count = 7;

    // sweep several sentinels
    for (uint64_t k : {2, 5, 11, 23}) {
        const uint256 sentinel = TaggedHash(k, 0, "protx");

        std::vector<uint256> expected;
        list.ForEachMN(false, [&](const auto& dmn) {
            if (dmn.proTxHash == sentinel) return;
            const auto s = dsl::CalcSentinelsForMN(list, dmn.proTxHash, epoch, count);
            if (std::find(s.begin(), s.end(), sentinel) != s.end()) expected.push_back(dmn.proTxHash);
        });

        auto got = dsl::GetProbeTargetsForSentinel(list, sentinel, epoch, count);
        std::sort(expected.begin(), expected.end());
        std::sort(got.begin(), got.end());
        BOOST_CHECK(got == expected);
        // a node never probes itself
        BOOST_CHECK(std::find(got.begin(), got.end(), sentinel) == got.end());
    }
}

// The liveness response verifies only for the exact epoch, target, and key.
BOOST_AUTO_TEST_CASE(challenge_response_roundtrips_and_binds)
{
    CBLSSecretKey targetKey;
    targetKey.MakeNewKey();
    const CBLSPublicKey pk = targetKey.GetPublicKey();

    const uint256 e1 = TaggedHash(1, 0, "epoch"), e2 = TaggedHash(2, 0, "epoch");
    const uint256 t1 = TaggedHash(1, 0, "protx"), t2 = TaggedHash(2, 0, "protx");

    const auto sig = dsl::SignChallengeResponse(targetKey, e1, t1);
    BOOST_CHECK(dsl::VerifyChallengeResponse(sig, pk, e1, t1));

    // a different epoch, target, or key does not verify -- no replay
    BOOST_CHECK(!dsl::VerifyChallengeResponse(sig, pk, e2, t1));
    BOOST_CHECK(!dsl::VerifyChallengeResponse(sig, pk, e1, t2));
    CBLSSecretKey other;
    other.MakeNewKey();
    BOOST_CHECK(!dsl::VerifyChallengeResponse(sig, other.GetPublicKey(), e1, t1));
}

BOOST_AUTO_TEST_SUITE_END()
