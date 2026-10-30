#ifndef WALLET_BLSWALLET_H
#define WALLET_BLSWALLET_H

#include <bls/bls.h>
#include <key.h>
#include <pubkey.h>

/** A BLS key held by one wallet (CWallet::blsKeyRecords). There is no
 *  process-wide BLS key store: each wallet signs with its own keys only. */
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

#endif // WALLET_BLSWALLET_H
