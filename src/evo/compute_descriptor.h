// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_COMPUTE_DESCRIPTOR_H
#define BITCOIN_EVO_COMPUTE_DESCRIPTOR_H

#include <serialize.h>
#include <uint256.h>
#include <univalue.h>
#include <util/strencodings.h>

#include <string>
#include <vector>

/** The service record a Compute masternode carries: who its oracle is and
 *  what it is certified to do. Versioned so a later field arrives as a
 *  version bump inside this struct instead of a masternode-state format
 *  change. Initial values arrive in the ProRegTx; a ProUpServTx replaces the
 *  record, which is how a certificate is renewed without re-registering. */
class CComputeServiceDescriptor
{
public:
    static constexpr uint16_t CURRENT_VERSION{1};
    static constexpr size_t ORACLE_KEY_SIZE{33};
    static constexpr size_t BLS_RESERVED_SIZE{48};
    static constexpr size_t MAX_ENDPOINT_SIZE{256};

    uint16_t nVersion{CURRENT_VERSION};
    /** Compressed secp256k1 oracle identity key: an EVM address derives from
     *  it and message signatures verify against it. */
    std::vector<unsigned char> vchOracleKey;
    /** Reserved for a per-node BLS key. Must stay empty at version 1 so a
     *  later version can occupy the slot without a format break. */
    std::vector<unsigned char> vchBlsReserved;
    std::string endpoint;
    uint256 certHash;
    int32_t certExpiryHeight{0};
    uint32_t capabilityBits{0};

    SERIALIZE_METHODS(CComputeServiceDescriptor, obj)
    {
        READWRITE(obj.nVersion);
        READWRITE(LIMITED_VECTOR(obj.vchOracleKey, ORACLE_KEY_SIZE));
        READWRITE(LIMITED_VECTOR(obj.vchBlsReserved, BLS_RESERVED_SIZE));
        READWRITE(LIMITED_STRING(obj.endpoint, MAX_ENDPOINT_SIZE));
        READWRITE(obj.certHash, obj.certExpiryHeight, obj.capabilityBits);
    }

    bool operator==(const CComputeServiceDescriptor& rhs) const
    {
        return nVersion == rhs.nVersion && vchOracleKey == rhs.vchOracleKey &&
               vchBlsReserved == rhs.vchBlsReserved && endpoint == rhs.endpoint &&
               certHash == rhs.certHash && certExpiryHeight == rhs.certExpiryHeight &&
               capabilityBits == rhs.capabilityBits;
    }
    bool operator!=(const CComputeServiceDescriptor& rhs) const { return !(*this == rhs); }

    /** A certificate is live while the chain has not reached its expiry
     *  height. Height, never wall time: every node must agree. */
    [[nodiscard]] bool IsCertValidAt(int nHeight) const
    {
        return !certHash.IsNull() && certExpiryHeight > nHeight;
    }

    // Inline: callers span libraries that do not all link the server
    // objects, and a JSON projection has no business owning a TU.
    [[nodiscard]] UniValue ToJson() const
    {
        UniValue obj;
        obj.setObject();
        obj.pushKV("version", nVersion);
        obj.pushKV("oracleKey", HexStr(vchOracleKey));
        obj.pushKV("endpoint", endpoint);
        obj.pushKV("certHash", certHash.ToString());
        obj.pushKV("certExpiryHeight", certExpiryHeight);
        obj.pushKV("capabilityBits", (int64_t)capabilityBits);
        return obj;
    }
};

#endif // BITCOIN_EVO_COMPUTE_DESCRIPTOR_H
