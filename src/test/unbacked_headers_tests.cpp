// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/unbacked_headers.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(unbacked_headers_tests)

// The deep-fork warning's condition: headers the depth limit dropped from an outbound peer warrant it when they offer
// more work than the tip, or when their message was full under its own format's limit (GetHeadersLimit: 2000 plain
// headers, 8000 compressed ones), whatever their work.
BOOST_AUTO_TEST_CASE(deep_fork_warrants)
{
    using node::DeepForkWarrants;
    for (const size_t limit : {size_t{MAX_HEADERS_UNCOMPRESSED_RESULT}, size_t{MAX_HEADERS_COMPRESSED_RESULT}}) {
        BOOST_CHECK(DeepForkWarrants(/*more_work=*/true, /*count=*/1, limit));
        BOOST_CHECK(DeepForkWarrants(/*more_work=*/true, limit - 1, limit));
        BOOST_CHECK(DeepForkWarrants(/*more_work=*/false, limit, limit));
        BOOST_CHECK(!DeepForkWarrants(/*more_work=*/false, limit - 1, limit));
        BOOST_CHECK(!DeepForkWarrants(/*more_work=*/false, /*count=*/1, limit));
    }
    // A full plain message is not full under the compressed format's limit.
    BOOST_CHECK(!DeepForkWarrants(/*more_work=*/false, MAX_HEADERS_UNCOMPRESSED_RESULT, MAX_HEADERS_COMPRESSED_RESULT));
}

BOOST_AUTO_TEST_SUITE_END()
