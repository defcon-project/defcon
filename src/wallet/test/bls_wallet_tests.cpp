// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls.h>
#include <coins.h>
#include <psbt.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <util/translation.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <memory>

namespace {
// The BLS keys of several wallets used to share one process-wide store: any
// wallet, and any provider at all, could sign with a key another wallet had
// loaded, and locking or loading one wallet emptied the store for all of them.

std::shared_ptr<CWallet> MakeWallet(NodeContext& node, const std::string& name, bool descriptors)
{
    auto wallet = std::make_shared<CWallet>(node.chain.get(), node.coinjoin_loader.get(), name, CreateDummyWalletDatabase());
    LOCK(wallet->cs_wallet);
    if (descriptors) {
        wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet->SetupDescriptorScriptPubKeyMans();
    } else {
        wallet->SetupLegacyScriptPubKeyMan();
    }
    return wallet;
}

// A new BLS key in `wallet`, as `bls generate` stores it; returns the
// pay-to-BLS-pubkey script of the key.
CScript NewBLSScript(CWallet& wallet, CPubKey* pubkey_out = nullptr)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    BOOST_REQUIRE(wallet.WriteToBLSWallet(sk.ToString()));
    const std::vector<uint8_t> pub = sk.GetPublicKey().ToByteVector(false);
    if (pubkey_out) pubkey_out->Set(pub.begin(), pub.end());
    return CScript() << pub << OP_CHECKSIG;
}

// The BLS public key a pay-to-BLS-pubkey script pays.
std::vector<unsigned char> BLSPubKey(const CScript& script)
{
    std::vector<std::vector<unsigned char>> solutions;
    BOOST_REQUIRE(Solver(script, solutions) == TxoutType::BLSPUBKEY);
    return solutions[0];
}

struct BLSSpend {
    CTransactionRef prev;
    CMutableTransaction tx;
    std::map<COutPoint, Coin> coins;
};

// A transaction spending one output to each of `scripts`, in order.
BLSSpend MakeSpend(const std::vector<CScript>& scripts)
{
    CMutableTransaction prev;
    prev.vin.emplace_back(COutPoint(uint256::ONE, 0));
    for (const CScript& script : scripts) prev.vout.emplace_back(COIN, script);
    BLSSpend spend;
    spend.prev = MakeTransactionRef(prev);
    for (uint32_t n = 0; n < scripts.size(); ++n) {
        spend.tx.vin.emplace_back(COutPoint(spend.prev->GetHash(), n));
        spend.coins.emplace(spend.tx.vin.back().prevout, Coin(spend.prev->vout[n], 1, false, false));
    }
    spend.tx.vout.emplace_back(COIN / 2, CScript() << OP_TRUE);
    return spend;
}

BLSSpend MakeSpend(const CScript& script) { return MakeSpend(std::vector<CScript>{script}); }

// Whether `wallet` completes the spend, as signrawtransactionwithwallet asks it to.
bool Signs(const CWallet& wallet, const CScript& script)
{
    BLSSpend spend = MakeSpend(script);
    std::map<int, bilingual_str> input_errors;
    return wallet.SignTransaction(spend.tx, spend.coins, SIGHASH_ALL, input_errors);
}

// Whether `wallet` puts a signature into the PSBT, as walletprocesspsbt asks it to. With `p2sh`,
// the input pays to the hash of `script` and the PSBT carries `script` as its redeem script.
bool FillsSignature(const CWallet& wallet, const CScript& script, bool sign, bool p2sh = false)
{
    const BLSSpend spend = MakeSpend(p2sh ? GetScriptForDestination(ScriptHash(script)) : script);
    PartiallySignedTransaction psbtx{spend.tx};
    psbtx.inputs[0].non_witness_utxo = spend.prev;
    if (p2sh) psbtx.inputs[0].redeem_script = script;
    bool complete{false};
    BOOST_REQUIRE(wallet.FillPSBT(psbtx, complete, SIGHASH_ALL, sign, /*bip32derivs=*/false) == TransactionError::OK);
    return !psbtx.inputs[0].partial_sigs.empty() || PSBTInputSigned(psbtx.inputs[0]);
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(bls_wallet_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(a_wallet_signs_bls_inputs_with_its_own_keys_only)
{
    auto holder = MakeWallet(m_node, "bls-holder", /*descriptors=*/false);
    auto other = MakeWallet(m_node, "bls-other", /*descriptors=*/false);
    auto descriptor = MakeWallet(m_node, "bls-descriptor", /*descriptors=*/true);
    CPubKey held_pubkey;
    const CScript held = NewBLSScript(*holder, &held_pubkey);
    const CScript descriptor_held = NewBLSScript(*descriptor);

    // Each wallet signs its own BLS input; a descriptor wallet keeps its BLS
    // keys outside its descriptors and signs with them all the same.
    BOOST_CHECK(Signs(*holder, held));
    BOOST_CHECK(Signs(*descriptor, descriptor_held));

    // No wallet reaches a key that another wallet in the process holds.
    BOOST_CHECK(!Signs(*other, held));
    BOOST_CHECK(!Signs(*descriptor, held));
    BOOST_CHECK(!Signs(*holder, descriptor_held));

    // A provider without keys must not sign either: signrawtransactionwithkey
    // and the PSBT updater pass one.
    {
        BLSSpend spend = MakeSpend(held);
        std::map<int, bilingual_str> input_errors;
        FillableSigningProvider no_keys;
        BOOST_CHECK(!::SignTransaction(spend.tx, &no_keys, spend.coins, SIGHASH_ALL, input_errors));
        BOOST_CHECK(spend.tx.vin[0].scriptSig.empty());
    }

    // Nor the holder's own manager with its secrets hidden (walletprocesspsbt
    // with sign=false), while the same manager unhidden does sign.
    LegacyScriptPubKeyMan* spk_man = holder->GetLegacyScriptPubKeyMan();
    BOOST_REQUIRE(spk_man);
    {
        BLSSpend spend = MakeSpend(held);
        SignatureData sigdata;
        BOOST_CHECK(!ProduceSignature(HidingSigningProvider(spk_man, /*hide_secret=*/true, /*hide_origin=*/false),
                                      MutableTransactionSignatureCreator(&spend.tx, 0, COIN, SIGHASH_ALL), held, sigdata));
        BOOST_CHECK(sigdata.signatures.empty());
    }
    {
        BLSSpend spend = MakeSpend(held);
        SignatureData sigdata;
        BOOST_CHECK(ProduceSignature(*spk_man, MutableTransactionSignatureCreator(&spend.tx, 0, COIN, SIGHASH_ALL), held, sigdata));
    }

    // The manager reports the BLS public key stored with the key, not a
    // secp256k1 key derived from the BLS secret.
    CPubKey reported;
    BOOST_CHECK(spk_man->GetPubKey(held_pubkey.GetID(), reported));
    BOOST_CHECK(reported == held_pubkey);

    // The wallet's own BLS keys sign pay-to-BLS-pubkey spends only. A multisig
    // script that names one takes its signatures through the ECDSA path, where a
    // BLS key must not go: the input keeps no signature.
    {
        const CScript multisig = CScript() << OP_1 << BLSPubKey(descriptor_held) << OP_1 << OP_CHECKMULTISIG;
        BLSSpend spend = MakeSpend(multisig);
        std::map<int, bilingual_str> input_errors;
        BOOST_CHECK(!descriptor->SignTransaction(spend.tx, spend.coins, SIGHASH_ALL, input_errors));
        BOOST_CHECK(spend.tx.vin[0].scriptSig == (CScript() << OP_0 << OP_0));
    }
}

BOOST_AUTO_TEST_CASE(mixed_inputs_keep_other_signatures_and_report_each_input)
{
    auto descriptor = MakeWallet(m_node, "bls-mixed-descriptor", /*descriptors=*/true);
    auto legacy = MakeWallet(m_node, "bls-mixed-legacy", /*descriptors=*/false);
    CTxDestination dest;
    bilingual_str error;
    BOOST_REQUIRE(descriptor->GetNewDestination("", dest, error));
    const CScript descriptor_ecdsa = GetScriptForDestination(dest);
    const CScript descriptor_bls = NewBLSScript(*descriptor);
    const CScript legacy_bls = NewBLSScript(*legacy);
    CKey stranger;
    stranger.MakeNewKey(true);
    const CScript unknown = GetScriptForDestination(PKHash(stranger.GetPubKey()));

    // Each wallet signs what it holds the key for. The other wallet's signature is
    // kept as it was, and every input still open is reported, and only those.
    BLSSpend spend = MakeSpend({descriptor_ecdsa, descriptor_bls, legacy_bls, unknown});
    std::map<int, bilingual_str> input_errors;
    BOOST_CHECK(!legacy->SignTransaction(spend.tx, spend.coins, SIGHASH_ALL, input_errors));
    BOOST_CHECK(!spend.tx.vin[2].scriptSig.empty());
    BOOST_CHECK(input_errors.count(0) && input_errors.count(1) && !input_errors.count(2) && input_errors.count(3));
    const CScript legacy_signature = spend.tx.vin[2].scriptSig;

    input_errors.clear();
    BOOST_CHECK(!descriptor->SignTransaction(spend.tx, spend.coins, SIGHASH_ALL, input_errors));
    BOOST_CHECK_EQUAL(input_errors.size(), 1U);
    BOOST_CHECK(input_errors.count(3));
    BOOST_CHECK(!spend.tx.vin[0].scriptSig.empty());
    BOOST_CHECK(!spend.tx.vin[1].scriptSig.empty());
    BOOST_CHECK(spend.tx.vin[2].scriptSig == legacy_signature);

    // Without the input nobody can sign, the same two wallets complete the transaction.
    BLSSpend complete = MakeSpend({descriptor_ecdsa, descriptor_bls, legacy_bls});
    input_errors.clear();
    BOOST_CHECK(!legacy->SignTransaction(complete.tx, complete.coins, SIGHASH_ALL, input_errors));
    input_errors.clear();
    BOOST_CHECK(descriptor->SignTransaction(complete.tx, complete.coins, SIGHASH_ALL, input_errors));
    BOOST_CHECK(input_errors.empty());
}

BOOST_AUTO_TEST_CASE(locking_one_wallet_keeps_the_others_bls_keys)
{
    const SecureString passphrase{"bls-wallet-test-passphrase"};
    auto encrypted = MakeWallet(m_node, "bls-encrypted", /*descriptors=*/false);
    auto plain = MakeWallet(m_node, "bls-plain", /*descriptors=*/false);
    auto descriptor = MakeWallet(m_node, "bls-plain-descriptor", /*descriptors=*/true);
    const CScript plain_held = NewBLSScript(*plain);
    const CScript descriptor_held = NewBLSScript(*descriptor);

    // Encrypting locks the wallet on its way, twice.
    BOOST_REQUIRE(encrypted->EncryptWallet(passphrase));
    BOOST_REQUIRE(encrypted->Unlock(passphrase));
    const CScript encrypted_held = NewBLSScript(*encrypted);
    BOOST_CHECK(Signs(*encrypted, encrypted_held));
    BOOST_CHECK(Signs(*plain, plain_held));
    BOOST_CHECK(Signs(*descriptor, descriptor_held));

    // Locking one wallet takes away its own keys and nobody else's.
    BOOST_REQUIRE(encrypted->Lock());
    BOOST_CHECK(!Signs(*encrypted, encrypted_held));
    BOOST_CHECK(Signs(*plain, plain_held));
    BOOST_CHECK(Signs(*descriptor, descriptor_held));
    {
        LOCK(plain->cs_wallet);
        BOOST_CHECK_EQUAL(plain->IsMine(plain_held), ISMINE_SPENDABLE);
    }

    // Unlocking reloads the wallet's own keys, again without touching the others.
    BOOST_REQUIRE(encrypted->Unlock(passphrase));
    BOOST_CHECK(Signs(*encrypted, encrypted_held));
    BOOST_CHECK(Signs(*plain, plain_held));
    BOOST_CHECK(Signs(*descriptor, descriptor_held));
    BOOST_CHECK(!Signs(*encrypted, plain_held));
}

BOOST_AUTO_TEST_CASE(psbt_signs_bls_inputs_only_with_the_wallets_own_keys)
{
    auto holder = MakeWallet(m_node, "bls-psbt-holder", /*descriptors=*/false);
    auto other = MakeWallet(m_node, "bls-psbt-other", /*descriptors=*/false);
    auto descriptor = MakeWallet(m_node, "bls-psbt-descriptor", /*descriptors=*/true);
    const CScript held = NewBLSScript(*holder);
    const CScript descriptor_held = NewBLSScript(*descriptor);

    // sign=false only updates the PSBT and must add no signature.
    BOOST_CHECK(!FillsSignature(*holder, held, /*sign=*/false));
    BOOST_CHECK(!FillsSignature(*descriptor, descriptor_held, /*sign=*/false));
    BOOST_CHECK(FillsSignature(*holder, held, /*sign=*/true));
    BOOST_CHECK(FillsSignature(*descriptor, descriptor_held, /*sign=*/true));

    BOOST_CHECK(!FillsSignature(*other, held, /*sign=*/true));
    BOOST_CHECK(!FillsSignature(*descriptor, held, /*sign=*/true));
    BOOST_CHECK(!FillsSignature(*holder, descriptor_held, /*sign=*/true));

    // The same under a pay-to-script-hash input whose redeem script the PSBT carries.
    BOOST_CHECK(!FillsSignature(*holder, held, /*sign=*/false, /*p2sh=*/true));
    BOOST_CHECK(!FillsSignature(*descriptor, descriptor_held, /*sign=*/false, /*p2sh=*/true));
    BOOST_CHECK(FillsSignature(*holder, held, /*sign=*/true, /*p2sh=*/true));
    BOOST_CHECK(FillsSignature(*descriptor, descriptor_held, /*sign=*/true, /*p2sh=*/true));
    BOOST_CHECK(!FillsSignature(*other, held, /*sign=*/true, /*p2sh=*/true));
    BOOST_CHECK(!FillsSignature(*descriptor, held, /*sign=*/true, /*p2sh=*/true));
    BOOST_CHECK(!FillsSignature(*holder, descriptor_held, /*sign=*/true, /*p2sh=*/true));
}

BOOST_AUTO_TEST_SUITE_END()
