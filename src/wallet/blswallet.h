#ifndef WALLET_BLSWALLET_H
#define WALLET_BLSWALLET_H

#include <bls/bls.h>

struct BlsWalletEntry {
    unsigned int id;
    CBLSSecretKey sk;
    CBLSPublicKey pk;
    BlsWalletEntry() {
        id = 0;
        sk.Reset();
        pk.Reset();
    };
};

#endif // WALLET_BLSWALLET_H
