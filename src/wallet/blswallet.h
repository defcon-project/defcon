#ifndef BITCOIN_WALLET_BLSWALLET_H
#define BITCOIN_WALLET_BLSWALLET_H

#include <bls/bls.h>
#include <key.h>
#include <pubkey.h>

/** A BLS key held by one wallet (CWallet::blsKeyRecords). There is no
 *  process-wide BLS key store: each wallet signs with its own keys only.
 *  An encrypted wallet keeps the entry while it is locked, without the secret
 *  (sk is not valid then): the public key is how it recognises its outputs. */
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

#endif // BITCOIN_WALLET_BLSWALLET_H
