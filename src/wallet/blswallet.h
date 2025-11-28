#ifndef WALLET_BLSWALLET_H
#define WALLET_BLSWALLET_H

#include <bls/bls.h>
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

#endif // WALLET_BLSWALLET_H
