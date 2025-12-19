#ifndef WALLET_BLSWALLET_H
#define WALLET_BLSWALLET_H

#include <bls/bls.h>
#include <key.h>
#include <pubkey.h>

struct BlsWalletEntry {
    unsigned int id;
    CKeyID keyID;
    CBLSSecretKey sk;
    CBLSPublicKey pk;
    BlsWalletEntry() {
        id = 0;
        sk.Reset();
        pk.Reset();
        keyID = CKeyID();
    };
};

void AddBLSRelated(const CKeyID& keyID);
bool IsBLSRelated(const CKeyID& keyID);

bool HaveBLSKey(const CKeyID &address);
bool GetBLSKey(const CKeyID &address, CKey& key);
bool AddBLSKey(const CKeyID &address, CKey& key);

#endif // WALLET_BLSWALLET_H
