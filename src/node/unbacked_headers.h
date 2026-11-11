// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_UNBACKED_HEADERS_H
#define BITCOIN_NODE_UNBACKED_HEADERS_H

#include <cstddef>

namespace node {
/**
 * Whether headers the depth limit on headers without their blocks dropped from an outbound peer warrant the node's
 * deep-fork warning (net_processing.cpp, MaybeWarnDeepFork): they offer more work than the active tip, or the message
 * held as many headers as its own format allows (`limit`: 2000 plain headers, 8000 compressed ones, see
 * GetHeadersLimit), so the peer has more to send. Anything else may be a short, lower-work branch of no interest.
 */
constexpr bool DeepForkWarrants(bool more_work, size_t count, size_t limit)
{
    return more_work || count >= limit;
}
} // namespace node

#endif // BITCOIN_NODE_UNBACKED_HEADERS_H
