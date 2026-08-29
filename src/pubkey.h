// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2017 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PUBKEY_H
#define BITCOIN_PUBKEY_H

#include <bls/bls.h>
#include <hash.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstring>
#include <vector>

const unsigned int BIP32_EXTKEY_SIZE = 74;
const unsigned int BIP32_EXTKEY_WITH_VERSION_SIZE = 78;

/** A reference to a CKey: the Hash160 of its serialized public key */
class CKeyID : public uint160
{
public:
    CKeyID() : uint160() {}
    explicit CKeyID(const uint160& in) : uint160(in) {}
};

typedef uint256 ChainCode;

/** An encapsulated public key. */
class CPubKey
{
public:
    /**
     * secp256k1:
     */
    static constexpr unsigned int SIZE                   = 65;
    static constexpr unsigned int COMPRESSED_SIZE        = 33;
    static constexpr unsigned int SIGNATURE_SIZE         = 72;
    static constexpr unsigned int COMPACT_SIGNATURE_SIZE = 65;

    /**
     * bls:
     */
    static constexpr unsigned int BLS_PUBLIC_KEY_SIZE    = 48;
    static constexpr unsigned int BLS_SIGNATURE_SIZE     = 96;

    /**
     * see www.keylength.com
     * script supports up to 75 for single byte push
     */
    static_assert(
        SIZE >= COMPRESSED_SIZE,
        "COMPRESSED_SIZE is larger than SIZE");

private:
    /**
     * The serialized key data. Kept private: nothing outside the class sets
     * these, and a length past the buffer would make end() and operator[]
     * read out of bounds.
     */
    unsigned int vchlen;
    unsigned char vch[SIZE];

    //! Set this key data to be invalid.
    void Invalidate()
    {
        // Zero length is the only value IsValid() (size() > 0) reads as
        // invalid. The old COMPRESSED_SIZE left every default-constructed key,
        // and every key a wrong length was rejected into, reporting itself
        // valid over an uninitialised or stale buffer.
        vchlen = 0;
    }

public:

    bool static ValidSize(const std::vector<unsigned char> &vch) {
      // A BLS key is a fixed length with no header-byte convention.
      if (vch.size() == BLS_PUBLIC_KEY_SIZE) {
          return true;
      }
      // secp256k1: the header byte fixes the length, so a blob of key length
      // whose header byte belongs to a different length -- or to no key at all
      // -- is not a well-formed key. Length alone was accepted before, which
      // let e.g. a 33-byte blob led by 0x07 (an uncompressed prefix) classify
      // as a pubkey in Solver. This does not gate consensus: ValidSize feeds
      // only Solver (script classification / standardness), never the script
      // interpreter.
      if (vch.empty()) {
          return false;
      }
      switch (vch[0]) {
      case 0x02:
      case 0x03:
          return vch.size() == COMPRESSED_SIZE;
      case 0x04:
      case 0x06:
      case 0x07:
          return vch.size() == SIZE;
      default:
          return false;
      }
    }

    //! Construct an invalid public key.
    CPubKey()
    {
        Invalidate();
    }

    //! Initialize a public key using begin/end iterators to byte data.
    template <typename T>
    void Set(const T pbegin, const T pend)
    {
        // Store only the three real key sizes; any other length is not a key
        // and invalidates. The earlier version accepted any length up to SIZE,
        // which is how a blob of some other length became a "valid" key of
        // that length -- IsValid() only reads size() > 0.
        const size_t len = static_cast<size_t>(pend - pbegin);
        if (len == COMPRESSED_SIZE || len == SIZE || len == BLS_PUBLIC_KEY_SIZE) {
            vchlen = static_cast<unsigned int>(len);
            memcpy(vch, (unsigned char*)&pbegin[0], len);
        } else {
            Invalidate();
        }
    }

    //! Construct a public key using begin/end iterators to byte data.
    template <typename T>
    CPubKey(const T pbegin, const T pend)
    {
        Set(pbegin, pend);
    }

    //! Construct a public key from a byte vector.
    explicit CPubKey(Span<const uint8_t> _vch)
    {
        Set(_vch.begin(), _vch.end());
    }

    //! Simple read-only vector-like interface to the pubkey data.
    unsigned int size() const { return vchlen; }
    const unsigned char* data() const { return vch; }
    const unsigned char* begin() const { return vch; }
    const unsigned char* end() const { return vch + size(); }
    const unsigned char& operator[](unsigned int pos) const { return vch[pos]; }
    std::vector<unsigned char> getvch() const
    {
        return std::vector<unsigned char>(begin(), end());
    }

    //! Comparator implementation.
    friend bool operator==(const CPubKey& a, const CPubKey& b)
    {
        return a.vch[0] == b.vch[0] &&
               memcmp(a.vch, b.vch, a.size()) == 0;
    }
    friend bool operator!=(const CPubKey& a, const CPubKey& b)
    {
        return !(a == b);
    }
    friend bool operator<(const CPubKey& a, const CPubKey& b)
    {
        return a.vch[0] < b.vch[0] ||
               (a.vch[0] == b.vch[0] && memcmp(a.vch, b.vch, a.size()) < 0);
    }
    friend bool operator>(const CPubKey& a, const CPubKey& b)
    {
        return a.vch[0] > b.vch[0] ||
               (a.vch[0] == b.vch[0] && memcmp(a.vch, b.vch, a.size()) > 0);
    }

    //! Implement serialization, as if this was a byte vector.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        unsigned int len = size();
        ::WriteCompactSize(s, len);
        s.write(AsBytes(Span{vch, len}));
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const unsigned int len(::ReadCompactSize(s));
        if (len <= SIZE) {
            s.read(AsWritableBytes(Span{vch, len}));
            if (len != SIZE && len != COMPRESSED_SIZE && len != BLS_PUBLIC_KEY_SIZE) {
                Invalidate();
            } else {
                vchlen = len;
            }
        } else {
            // invalid pubkey, skip available data
            s.ignore(len);
            Invalidate();
        }
    }

    //! Lookup for key type
    bool IsBLS() const { return size() == BLS_PUBLIC_KEY_SIZE; }

    //! Get the KeyID of this public key (hash of its serialization)
    CKeyID GetID() const
    {
        return CKeyID(Hash160(Span{vch}.first(size())));
    }

    //! Get the 256-bit hash of this public key.
    uint256 GetHash() const
    {
        return Hash(Span{vch}.first(size()));
    }

    /*
     * Check syntactic correctness.
     *
     * Note that this is consensus critical as CheckSig() calls it!
     */
    bool IsValid() const
    {
        return size() > 0;
    }

    //! fully validate whether this is a valid public key (more expensive than IsValid())
    bool IsFullyValid() const;

    //! Check whether this is a compressed public key.
    bool IsCompressed() const
    {
        return size() != BLS_PUBLIC_KEY_SIZE && size() == COMPRESSED_SIZE;
    }

    /**
     * Verify a DER signature (~72 bytes).
     * If this public key is not fully valid, the return value will be false.
     */
    bool Verify(const uint256& hash, const std::vector<unsigned char>& vchSig) const;

    /**
     * Check whether a signature is normalized (lower-S).
     */
    static bool CheckLowS(const std::vector<unsigned char>& vchSig);

    //! Recover a public key from a compact signature.
    bool RecoverCompact(const uint256& hash, const std::vector<unsigned char>& vchSig);

    //! Turn this public key into an uncompressed public key.
    bool Decompress();

    //! Derive BIP32 child pubkey.
    bool Derive(CPubKey& pubkeyChild, ChainCode &ccChild, unsigned int nChild, const ChainCode& cc) const;

///////////////////////////
// BLS
////////////////////////////

    bool VerifyBLS(const uint256 &hash, const std::vector<uint8_t> &vchSig) const;
};

/** An ElligatorSwift-encoded public key. */
struct EllSwiftPubKey
{
private:
    static constexpr size_t SIZE = 64;
    std::array<std::byte, SIZE> m_pubkey;

public:
    /** Default constructor creates all-zero pubkey (which is valid). */
    EllSwiftPubKey() noexcept = default;

    /** Construct a new ellswift public key from a given serialization. */
    EllSwiftPubKey(Span<const std::byte> ellswift) noexcept;

    /** Decode to normal compressed CPubKey (for debugging purposes). */
    CPubKey Decode() const;

    // Read-only access for serialization.
    const std::byte* data() const { return m_pubkey.data(); }
    static constexpr size_t size() { return SIZE; }
    auto begin() const { return m_pubkey.cbegin(); }
    auto end() const { return m_pubkey.cend(); }

    bool friend operator==(const EllSwiftPubKey& a, const EllSwiftPubKey& b)
    {
        return a.m_pubkey == b.m_pubkey;
    }

    bool friend operator!=(const EllSwiftPubKey& a, const EllSwiftPubKey& b)
    {
        return a.m_pubkey != b.m_pubkey;
    }
};

struct CExtPubKey {
    unsigned char version[4];
    unsigned char nDepth;
    unsigned char vchFingerprint[4];
    unsigned int nChild;
    ChainCode chaincode;
    CPubKey pubkey;

    friend bool operator==(const CExtPubKey &a, const CExtPubKey &b)
    {
        return a.nDepth == b.nDepth &&
            memcmp(a.vchFingerprint, b.vchFingerprint, sizeof(vchFingerprint)) == 0 &&
            a.nChild == b.nChild &&
            a.chaincode == b.chaincode &&
            a.pubkey == b.pubkey;
    }

    friend bool operator!=(const CExtPubKey &a, const CExtPubKey &b)
    {
        return !(a == b);
    }

    friend bool operator<(const CExtPubKey &a, const CExtPubKey &b)
    {
        if (a.pubkey < b.pubkey) {
            return true;
        } else if (a.pubkey > b.pubkey) {
            return false;
        }
        return a.chaincode < b.chaincode;
    }

    void Encode(unsigned char code[BIP32_EXTKEY_SIZE]) const;
    void Decode(const unsigned char code[BIP32_EXTKEY_SIZE]);
    void EncodeWithVersion(unsigned char code[BIP32_EXTKEY_WITH_VERSION_SIZE]) const;
    void DecodeWithVersion(const unsigned char code[BIP32_EXTKEY_WITH_VERSION_SIZE]);
    bool Derive(CExtPubKey& out, unsigned int nChild) const;

    void Serialize(CSizeComputer& s) const
    {
        // Optimized implementation for ::GetSerializeSize that avoids copying.
        s.seek(BIP32_EXTKEY_SIZE + 1); // add one byte for the size (compact int)
    }
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        unsigned int len = BIP32_EXTKEY_SIZE;
        ::WriteCompactSize(s, len);
        unsigned char code[BIP32_EXTKEY_SIZE];
        Encode(code);
        s.write(AsBytes(Span{&code[0], len}));
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        unsigned int len = ::ReadCompactSize(s);
        unsigned char code[BIP32_EXTKEY_SIZE];
        if (len != BIP32_EXTKEY_SIZE)
            throw std::runtime_error("Invalid extended key size\n");
        s.read(AsWritableBytes(Span{&code[0], len}));
        Decode(code);
    }
};

#endif // BITCOIN_PUBKEY_H
