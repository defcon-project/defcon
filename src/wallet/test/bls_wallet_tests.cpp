// Copyright (c) 2026 The DeFCoN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <base58.h>
#include <bls/bls.h>
#include <clientversion.h>
#include <coins.h>
#include <key.h>
#include <policy/policy.h>
#include <psbt.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <span.h>
#include <streams.h>
#include <util/translation.h>
#include <version.h>
#include <wallet/crypter.h>
#include <wallet/db.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
    // The same for an unencrypted legacy wallet, whose generic key lookup does reach its BLS secrets:
    // the ECDSA path must still not sign with one (an ECDSA signature from a BLS secret is invalid).
    {
        const CScript multisig = CScript() << OP_1 << BLSPubKey(held) << OP_1 << OP_CHECKMULTISIG;
        BLSSpend spend = MakeSpend(multisig);
        std::map<int, bilingual_str> input_errors;
        BOOST_CHECK(!holder->SignTransaction(spend.tx, spend.coins, SIGHASH_ALL, input_errors));
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

namespace {
// `psbtx` through its serialized form and back, as the next step of a PSBT workflow reads it.
std::optional<PartiallySignedTransaction> ReadBack(const PartiallySignedTransaction& psbtx, std::string& error)
{
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << psbtx;
    PartiallySignedTransaction back;
    if (!DecodeRawPSBT(back, MakeByteSpan(stream), error)) return std::nullopt;
    return back;
}
} // namespace

// walletprocesspsbt with finalize=false left a signed BLS input as a partial signature. That record holds the
// signer's public key, and the reader takes only the 33 or 65 bytes of an ECDSA key: with the 48 of a BLS key the
// next step refused the PSBT this node had written, its own decodepsbt, combinepsbt and finalizepsbt included. What a
// wallet writes has to read back; an ECDSA input stays a partial signature, as before.
BOOST_AUTO_TEST_CASE(an_unfinalized_psbt_with_bls_inputs_reads_back)
{
    auto descriptor = MakeWallet(m_node, "bls-roundtrip-descriptor", /*descriptors=*/true);
    auto legacy = MakeWallet(m_node, "bls-roundtrip-legacy", /*descriptors=*/false);
    CTxDestination dest;
    bilingual_str error;
    BOOST_REQUIRE(descriptor->GetNewDestination("", dest, error));
    const CScript ecdsa = GetScriptForDestination(dest);
    const CScript descriptor_bls = NewBLSScript(*descriptor);
    const CScript legacy_bls = NewBLSScript(*legacy);

    for (const bool p2sh : {false, true}) {
        const std::string variant{p2sh ? "pay-to-script-hash" : "bare"};
        const auto paid = [&](const CScript& bls) { return p2sh ? GetScriptForDestination(ScriptHash(bls)) : bls; };
        const BLSSpend spend = MakeSpend({ecdsa, paid(descriptor_bls), paid(legacy_bls)});
        PartiallySignedTransaction psbtx{spend.tx};
        for (auto& input : psbtx.inputs) input.non_witness_utxo = spend.prev;
        if (p2sh) {
            psbtx.inputs[1].redeem_script = descriptor_bls;
            psbtx.inputs[2].redeem_script = legacy_bls;
        }

        // Each wallet signs what it holds without finalizing, as two walletprocesspsbt calls with finalize=false,
        // and each result is read back before the next step.
        bool complete{false};
        std::string read_error;
        BOOST_REQUIRE(descriptor->FillPSBT(psbtx, complete, SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/true,
                                           /*n_signed=*/nullptr, /*finalize=*/false) == TransactionError::OK);
        auto first = ReadBack(psbtx, read_error);
        BOOST_REQUIRE_MESSAGE(first, variant << ": the descriptor wallet's PSBT does not read back: " << read_error);
        BOOST_REQUIRE(legacy->FillPSBT(*first, complete, SIGHASH_ALL, /*sign=*/true, /*bip32derivs=*/true,
                                       /*n_signed=*/nullptr, /*finalize=*/false) == TransactionError::OK);
        auto second = ReadBack(*first, read_error);
        BOOST_REQUIRE_MESSAGE(second, variant << ": the legacy wallet's PSBT does not read back: " << read_error);

        // The ECDSA input is a partial signature still; each BLS input is final, with no partial signature left.
        BOOST_CHECK(!PSBTInputSigned(second->inputs[0]));
        BOOST_CHECK_EQUAL(second->inputs[0].partial_sigs.size(), 1U);
        for (const size_t i : {size_t{1}, size_t{2}}) {
            BOOST_CHECK_MESSAGE(PSBTInputSigned(second->inputs[i]), variant << ": BLS input " << i << " is not final");
            BOOST_CHECK(second->inputs[i].partial_sigs.empty());
        }

        // The finalizer completes the PSBT, and every input of the transaction it gives verifies.
        CMutableTransaction final_tx;
        BOOST_REQUIRE_MESSAGE(FinalizeAndExtractPSBT(*second, final_tx), variant << ": the PSBT does not finalize");
        for (unsigned int i = 0; i < final_tx.vin.size(); ++i) {
            const CTxOut& prev = spend.prev->vout[i];
            ScriptError err{SCRIPT_ERR_OK};
            BOOST_CHECK_MESSAGE(VerifyScript(final_tx.vin[i].scriptSig, prev.scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS,
                                             MutableTransactionSignatureChecker(&final_tx, i, prev.nValue, MissingDataBehavior::FAIL),
                                             &err),
                                variant << ": input " << i << ": " << ScriptErrorString(err));
        }
    }
}

namespace {
isminetype Mine(const CWallet& wallet, const CScript& script)
{
    LOCK(wallet.cs_wallet);
    return wallet.IsMine(script);
}

CPubKey PubKeyOf(const CBLSSecretKey& sk)
{
    const std::vector<uint8_t> pub = sk.GetPublicKey().ToByteVector(false);
    return CPubKey(pub);
}

CScript ScriptOf(const CPubKey& pubkey) { return CScript() << ToByteVector(pubkey) << OP_CHECKSIG; }

// An encrypted legacy wallet as it is when its file has just been read: the master key record of
// `passphrase`, and nothing unlocked. Its database keeps what the test writes, so BLSWalletInit
// reads the BLS records as it does when a wallet opens.
std::shared_ptr<CWallet> MakeEncryptedWalletFile(NodeContext& node, const std::string& name, const SecureString& passphrase, CKeyingMaterial& master_key,
                                                 std::unique_ptr<WalletDatabase> database = CreateMockWalletDatabase())
{
    auto wallet = std::make_shared<CWallet>(node.chain.get(), node.coinjoin_loader.get(), name, std::move(database));
    wallet->LoadWallet();
    master_key.assign(WALLET_CRYPTO_KEY_SIZE, 0x2a);
    CMasterKey record;
    record.vchSalt.assign(WALLET_CRYPTO_SALT_SIZE, 0x07);
    CCrypter crypter;
    BOOST_REQUIRE(crypter.SetKeyFromPassphrase(passphrase, record.vchSalt, record.nDeriveIterations, record.nDerivationMethod));
    BOOST_REQUIRE(crypter.Encrypt(master_key, record.vchCryptedKey));
    LOCK(wallet->cs_wallet);
    wallet->SetupLegacyScriptPubKeyMan();
    wallet->mapMasterKeys[++wallet->nMasterKeyMaxID] = record;
    BOOST_REQUIRE(wallet->IsCrypted());
    BOOST_REQUIRE(wallet->IsLocked());
    return wallet;
}

// A `cblskey` record: `pubkey` in the clear, and the secret of `sk`, as its hex string, encrypted
// to it. WriteToBLSWallet writes one with the secret's own public key.
void WriteCryptedRecord(CWallet& wallet, int64_t id, const CKeyingMaterial& master_key, const CBLSSecretKey& sk, const CPubKey& pubkey)
{
    const std::string hex = sk.ToString();
    std::vector<unsigned char> crypted;
    BOOST_REQUIRE(EncryptSecret(master_key, CKeyingMaterial(hex.begin(), hex.end()), pubkey.GetHash(), crypted));
    BOOST_REQUIRE(WalletBatch(wallet.GetDatabase()).WriteCryptedBLSKey(id, pubkey, crypted));
}
} // namespace

// Locking used to empty the wallet's BLS key list, public keys and all, and an encrypted wallet
// opened with an empty one: until a full unlock the wallet did not know the outputs paid to its
// own BLS keys, so it did not record a payment that arrived meanwhile.
BOOST_AUTO_TEST_CASE(a_locked_wallet_still_knows_its_bls_outputs)
{
    const SecureString passphrase{"bls-wallet-test-passphrase"};
    auto wallet = MakeWallet(m_node, "bls-locked", /*descriptors=*/false);
    BOOST_REQUIRE(wallet->EncryptWallet(passphrase));
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    CPubKey pubkey;
    const CScript held = NewBLSScript(*wallet, &pubkey);
    const std::map<unsigned int, std::string> addresses = wallet->GetBLSAddresses();
    BOOST_REQUIRE_EQUAL(addresses.size(), 1U);
    BOOST_CHECK_EQUAL(addresses.at(0), EncodeBase58Check(ToByteVector(pubkey)));
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
    BOOST_CHECK(Signs(*wallet, held));

    // Locking takes the secret away and leaves the public key.
    BOOST_REQUIRE(wallet->Lock());
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
    BOOST_CHECK(wallet->GetBLSAddresses() == addresses);
    BOOST_CHECK(!Signs(*wallet, held));
    BOOST_CHECK(!FillsSignature(*wallet, held, /*sign=*/true));
    CKey secret;
    BOOST_CHECK(!wallet->GetBLSKey(pubkey.GetID(), secret));
    LegacyScriptPubKeyMan* spk_man = wallet->GetLegacyScriptPubKeyMan();
    BOOST_REQUIRE(spk_man);
    BOOST_CHECK(!spk_man->HaveBLSKey(pubkey.GetID()));
    CPubKey reported;
    BOOST_CHECK(spk_man->GetPubKey(pubkey.GetID(), reported));
    BOOST_CHECK(reported == pubkey);

    // Unlocked for mixing and staking only: the BLS secrets stay away, the outputs stay known.
    BOOST_REQUIRE(wallet->Unlock(passphrase, /*fForMixingOnly=*/true));
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
    BOOST_CHECK(!Signs(*wallet, held));

    // A full unlock brings the secret back to the same key, not to a second one.
    BOOST_REQUIRE(wallet->Lock());
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    BOOST_CHECK(Signs(*wallet, held));
    BOOST_CHECK(wallet->GetBLSAddresses() == addresses);
    BOOST_CHECK(wallet->GetBLSKey(pubkey.GetID(), secret));

    // A key added now is the second one, and both are known once the wallet is locked again.
    const CScript second = NewBLSScript(*wallet);
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);
    BOOST_REQUIRE(wallet->Lock());
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
    BOOST_CHECK_EQUAL(Mine(*wallet, second), ISMINE_SPENDABLE);
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);
    BOOST_CHECK(!Signs(*wallet, second));
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    BOOST_CHECK(Signs(*wallet, held));
    BOOST_CHECK(Signs(*wallet, second));
}

// A wallet file with one BLS key, in the three states the tests below open it in.
struct OpenedWalletSetup : public WalletTestingSetup {
    const SecureString passphrase{"bls-wallet-test-passphrase"};
    CKeyingMaterial master_key;
    CBLSSecretKey sk;
    CPubKey pubkey;
    CScript held;
    OpenedWalletSetup()
    {
        sk.MakeNewKey();
        pubkey = PubKeyOf(sk);
        held = ScriptOf(pubkey);
    }
};

BOOST_FIXTURE_TEST_CASE(an_encrypted_wallet_knows_its_bls_outputs_when_it_opens, OpenedWalletSetup)
{
    // An encrypted record: its public key needs no passphrase.
    {
        auto wallet = MakeEncryptedWalletFile(m_node, "bls-opened", passphrase, master_key);
        WriteCryptedRecord(*wallet, 0, master_key, sk, pubkey);
        BOOST_REQUIRE(wallet->BLSWalletInit());
        BOOST_REQUIRE(wallet->IsLocked());
        BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
        const std::map<unsigned int, std::string> addresses = wallet->GetBLSAddresses();
        const std::map<unsigned int, std::string> expected{{0U, EncodeBase58Check(ToByteVector(pubkey))}};
        BOOST_CHECK(addresses == expected);
        BOOST_CHECK(!Signs(*wallet, held));

        // The unlock gives that key its secret, and locking takes only the secret back.
        BOOST_REQUIRE(wallet->Unlock(passphrase));
        BOOST_CHECK(Signs(*wallet, held));
        BOOST_CHECK(wallet->GetBLSAddresses() == addresses);
        BOOST_REQUIRE(wallet->Lock());
        BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
        BOOST_CHECK(!Signs(*wallet, held));
    }

    // A record whose public key is no BLS key is not the wallet's, and the record before it still is.
    {
        auto wallet = MakeEncryptedWalletFile(m_node, "bls-opened-bad-record", passphrase, master_key);
        WriteCryptedRecord(*wallet, 0, master_key, sk, pubkey);
        const CPubKey no_key(std::vector<unsigned char>(CPubKey::BLS_PUBLIC_KEY_SIZE, 0x01));
        BOOST_REQUIRE(no_key.IsBLS());
        BOOST_REQUIRE(WalletBatch(wallet->GetDatabase()).WriteCryptedBLSKey(1, no_key, std::vector<unsigned char>(80, 0x5a)));
        BOOST_REQUIRE(wallet->BLSWalletInit());
        BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
        BOOST_CHECK_EQUAL(Mine(*wallet, ScriptOf(no_key)), ISMINE_NO);
        BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 1U);
    }
}

BOOST_FIXTURE_TEST_CASE(an_encrypted_wallet_knows_a_bls_key_left_in_plaintext_when_it_opens, OpenedWalletSetup)
{
    // A key an older version left in plaintext in an encrypted wallet, which the next full unlock
    // moves to an encrypted record: known as well, and its secret is not loaded.
    {
        auto wallet = MakeEncryptedWalletFile(m_node, "bls-opened-plaintext-record", passphrase, master_key);
        std::string hex = sk.ToString();
        BOOST_REQUIRE(WalletBatch(wallet->GetDatabase()).WriteBLSKey(0, hex));
        BOOST_REQUIRE(wallet->BLSWalletInit());
        BOOST_REQUIRE(wallet->IsLocked());
        BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
        BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 1U);
        BOOST_CHECK(!Signs(*wallet, held));
        CKey secret;
        BOOST_CHECK(!wallet->GetBLSKey(pubkey.GetID(), secret));
    }
}

BOOST_FIXTURE_TEST_CASE(a_bls_record_naming_another_keys_public_key_does_not_unlock, OpenedWalletSetup)
{
    // A record that names another key's public key over this secret. The locked wallet goes by
    // the record; the unlock finds that the secret is not that key's, fails as it does for a
    // record that does not decrypt, and leaves no secret behind.
    {
        CBLSSecretKey other_sk;
        other_sk.MakeNewKey();
        const CPubKey other_pubkey = PubKeyOf(other_sk);
        auto wallet = MakeEncryptedWalletFile(m_node, "bls-opened-mismatch", passphrase, master_key);
        WriteCryptedRecord(*wallet, 0, master_key, sk, other_pubkey);
        BOOST_REQUIRE(wallet->BLSWalletInit());
        BOOST_CHECK_EQUAL(Mine(*wallet, ScriptOf(other_pubkey)), ISMINE_SPENDABLE);
        BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_NO);
        BOOST_CHECK(!wallet->Unlock(passphrase));
        BOOST_CHECK(wallet->IsLocked());
        BOOST_CHECK(!Signs(*wallet, held));
        BOOST_CHECK(!Signs(*wallet, ScriptOf(other_pubkey)));
        BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_NO);
        BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 1U);
    }
}

namespace {
// The record types the wallet database refuses to write, as a full or failing disk refuses a write.
struct RefusedRecords {
    std::set<std::string> types;
    int refusals{0};
    //! The write of a refused record throws instead of returning false, as one that runs out of memory.
    bool throws{false};
    //! How many writes of a refused type still go through before the refusals start.
    int let_through{0};
    //! Run once, while the database is answering the next write it refuses.
    std::function<void()> on_refusal;

    //! Whether `key` names a record of a refused type (a record's key starts with its type); counts the refusal.
    bool Refuses(const CDataStream& key)
    {
        for (const std::string& type : types) {
            CDataStream prefix(SER_DISK, CLIENT_VERSION);
            prefix << type;
            if (key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin())) {
                if (let_through > 0) {
                    --let_through;
                    return false;
                }
                ++refusals;
                if (on_refusal) {
                    const std::function<void()> hook = std::move(on_refusal);
                    on_refusal = nullptr;
                    hook();
                }
                return true;
            }
        }
        return false;
    }
};

// A key or a value that is serialized already: written and read as it is.
struct RawRecord {
    std::vector<std::byte> bytes;

    RawRecord() = default;
    explicit RawRecord(const CDataStream& stream) : bytes(stream.begin(), stream.end()) {}
    template <typename Stream> void Serialize(Stream& s) const { s.write(Span<const std::byte>{bytes.data(), bytes.size()}); }
    template <typename Stream> void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        s.read(Span<std::byte>{bytes.data(), bytes.size()});
    }
};

// The real database's batch, except that it refuses to write the records RefusedRecords names.
class RefusingBatch : public DatabaseBatch
{
    const std::unique_ptr<DatabaseBatch> m_real;
    const std::shared_ptr<RefusedRecords> m_refused;

    bool ReadKey(CDataStream&& key, CDataStream& value) override
    {
        RawRecord found;
        if (!m_real->Read(RawRecord{key}, found)) return false;
        value.clear();
        value.write(Span<const std::byte>{found.bytes.data(), found.bytes.size()});
        return true;
    }
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) override
    {
        if (m_refused->Refuses(key)) {
            if (m_refused->throws) throw std::bad_alloc();
            return false;
        }
        return m_real->Write(RawRecord{key}, RawRecord{value}, overwrite);
    }
    bool EraseKey(CDataStream&& key) override { return m_real->Erase(RawRecord{key}); }
    bool HasKey(CDataStream&& key) override { return m_real->Exists(RawRecord{key}); }

public:
    RefusingBatch(std::unique_ptr<DatabaseBatch> real, std::shared_ptr<RefusedRecords> refused) : m_real{std::move(real)}, m_refused{std::move(refused)} {}

    void Flush() override { m_real->Flush(); }
    void Close() override { m_real->Close(); }
    bool StartCursor() override { return m_real->StartCursor(); }
    bool ReadAtCursor(CDataStream& key, CDataStream& value, bool& complete) override { return m_real->ReadAtCursor(key, value, complete); }
    void CloseCursor() override { m_real->CloseCursor(); }
    bool TxnBegin() override { return m_real->TxnBegin(); }
    bool TxnCommit() override { return m_real->TxnCommit(); }
    bool TxnAbort() override { return m_real->TxnAbort(); }
};

// The real wallet database behind batches that can refuse a write.
class RefusingDatabase : public WalletDatabase
{
    const std::unique_ptr<WalletDatabase> m_real;
    const std::shared_ptr<RefusedRecords> m_refused;

public:
    RefusingDatabase(std::unique_ptr<WalletDatabase> real, std::shared_ptr<RefusedRecords> refused) : m_real{std::move(real)}, m_refused{std::move(refused)} {}

    void Open() override { m_real->Open(); }
    void AddRef() override { m_real->AddRef(); }
    void RemoveRef() override { m_real->RemoveRef(); }
    bool Rewrite(const char* skip = nullptr) override { return m_real->Rewrite(skip); }
    bool Backup(const std::string& dest) const override { return m_real->Backup(dest); }
    void Flush() override { m_real->Flush(); }
    void Close() override { m_real->Close(); }
    bool PeriodicFlush() override { return m_real->PeriodicFlush(); }
    void IncrementUpdateCounter() override
    {
        ++nUpdateCounter;
        m_real->IncrementUpdateCounter();
    }
    void ReloadDbEnv() override { m_real->ReloadDbEnv(); }
    std::string Filename() override { return m_real->Filename(); }
    std::string Format() override { return m_real->Format(); }
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override
    {
        return std::make_unique<RefusingBatch>(m_real->MakeBatch(flush_on_close), m_refused);
    }
    bool SupportsAutoBackup() override { return m_real->SupportsAutoBackup(); }
};

// The wallet holds the BLS keys it listed as `addresses` and not the key of `pubkey`: neither among its keys in
// memory, nor in record `id` of its database, which is where the key would have been written.
void CheckNotHeld(CWallet& wallet, const std::map<unsigned int, std::string>& addresses, const CPubKey& pubkey, int64_t id, const std::string& when)
{
    const CScript script = ScriptOf(pubkey);
    BOOST_CHECK_MESSAGE(wallet.GetBLSAddresses() == addresses, when << ": the wallet lists " << wallet.GetBLSAddresses().size() << " BLS addresses");
    BOOST_CHECK_MESSAGE(Mine(wallet, script) == ISMINE_NO, when << ": the wallet takes the key's outputs for its own");
    BOOST_CHECK_MESSAGE(!Signs(wallet, script), when << ": the wallet signs with the key");
    CKey secret;
    BOOST_CHECK_MESSAGE(!wallet.GetBLSKey(pubkey.GetID(), secret), when << ": the wallet gives the key's secret");
    WalletBatch batch(wallet.GetDatabase());
    std::string plain;
    CPubKey stored_pubkey;
    std::vector<unsigned char> crypted;
    BOOST_CHECK_MESSAGE(!batch.ReadBLSKey(id, plain), when << ": the database holds a plaintext record " << id);
    BOOST_CHECK_MESSAGE(!batch.ReadCryptedBLSKey(id, stored_pubkey, crypted), when << ": the database holds an encrypted record " << id);
}
} // namespace

// WriteToBLSWallet took a new key among the wallet's keys before it wrote the key's record, and left it there when
// the database refused the write. The caller got its error, but the wallet went on listing the address as its own
// and counting what was paid to it until the next start, when the key was gone: a BLS key does not come back from
// the seed.
BOOST_AUTO_TEST_CASE(a_bls_key_the_database_refused_is_not_among_the_wallets_keys)
{
    const auto refused = std::make_shared<RefusedRecords>();
    auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "bls-refused",
                                            std::make_unique<RefusingDatabase>(CreateMockWalletDatabase(), refused));
    wallet->LoadWallet();
    {
        LOCK(wallet->cs_wallet);
        wallet->SetupLegacyScriptPubKeyMan();
    }
    CBLSSecretKey first_sk;
    first_sk.MakeNewKey();
    const CScript first = ScriptOf(PubKeyOf(first_sk));
    BOOST_REQUIRE(wallet->WriteToBLSWallet(first_sk.ToString()));
    const std::map<unsigned int, std::string> addresses = wallet->GetBLSAddresses();
    BOOST_REQUIRE_EQUAL(addresses.size(), 1U);

    CBLSSecretKey sk;
    sk.MakeNewKey();
    const CPubKey pubkey = PubKeyOf(sk);
    refused->types = {DBKeys::BLSKEY, DBKeys::CRYPTED_BLSKEY};
    // While the database answers the write it refuses, the key store is held: a reader on another thread, which takes
    // the key store's lock and not the wallet's, cannot get in until the outcome, and then finds no key (review r4,
    // R40). The other thread tries the lock once and the test waits for its answer, so no timing is involved (r5, R48).
    LegacyScriptPubKeyMan* const key_store = wallet->GetLegacyScriptPubKeyMan();
    BOOST_REQUIRE(key_store);
    bool held_while_writing{false};
    refused->on_refusal = [&] {
        held_while_writing = std::async(std::launch::async, [&] {
            const bool got = key_store->cs_KeyStore.try_lock();
            if (got) key_store->cs_KeyStore.unlock();
            return !got;
        }).get();
    };
    BOOST_CHECK(!wallet->WriteToBLSWallet(sk.ToString()));
    BOOST_CHECK_EQUAL(refused->refusals, 1);
    BOOST_CHECK(held_while_writing);
    {
        CKey secret;
        BOOST_CHECK(!std::async(std::launch::async, [&] { return wallet->GetBLSKey(pubkey.GetID(), secret); }).get());
    }
    CheckNotHeld(*wallet, addresses, pubkey, 1, "after the refused write");

    // The BLS change address of a transaction is a new key saved the same way: none is made.
    BOOST_CHECK(wallet->GenerateNewBLSChangeAddress().empty());
    BOOST_CHECK_EQUAL(refused->refusals, 2);
    BOOST_CHECK(wallet->GetBLSAddresses() == addresses);

    // A write that throws, as one that runs out of memory: the key is taken back the same way, and the error goes on.
    refused->throws = true;
    BOOST_CHECK_THROW(wallet->WriteToBLSWallet(sk.ToString()), std::bad_alloc);
    BOOST_CHECK_EQUAL(refused->refusals, 3);
    CheckNotHeld(*wallet, addresses, pubkey, 1, "after the write that threw");
    refused->throws = false;

    // A key the wallet holds already keeps its place when its write is refused: its record holds it.
    BOOST_CHECK(!wallet->WriteToBLSWallet(first_sk.ToString()));
    BOOST_CHECK_EQUAL(refused->refusals, 4);
    BOOST_CHECK(wallet->GetBLSAddresses() == addresses);
    BOOST_CHECK(Signs(*wallet, first));

    // What the next start reads from the database is the same.
    BOOST_REQUIRE(wallet->BLSWalletInit());
    CheckNotHeld(*wallet, addresses, pubkey, 1, "after the records are read again");
    BOOST_CHECK(Signs(*wallet, first));

    // Once the database writes, the same key is saved as the wallet's second one.
    refused->types.clear();
    BOOST_REQUIRE(wallet->WriteToBLSWallet(sk.ToString()));
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);
    BOOST_CHECK(Signs(*wallet, ScriptOf(pubkey)));
    std::string record;
    BOOST_CHECK(WalletBatch(wallet->GetDatabase()).ReadBLSKey(1, record));
    BOOST_CHECK_EQUAL(record, sk.ToString());

    // A key the wallet holds already is written to its own record, not to a new one.
    BOOST_CHECK(wallet->WriteToBLSWallet(first_sk.ToString()));
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);
    BOOST_CHECK(WalletBatch(wallet->GetDatabase()).ReadBLSKey(0, record));
    BOOST_CHECK_EQUAL(record, first_sk.ToString());
    BOOST_CHECK(!WalletBatch(wallet->GetDatabase()).ReadBLSKey(2, record));

    BOOST_REQUIRE(wallet->BLSWalletInit());
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);
    BOOST_CHECK(Signs(*wallet, first));
    BOOST_CHECK(Signs(*wallet, ScriptOf(pubkey)));
}

// The same in an encrypted wallet, where the key's record is the encrypted one. A lock leaves the public keys the
// wallet holds, so the key that was not saved would have stayed listed with no secret left to unlock.
BOOST_FIXTURE_TEST_CASE(an_encrypted_bls_key_the_database_refused_is_not_among_the_wallets_keys, OpenedWalletSetup)
{
    const auto refused = std::make_shared<RefusedRecords>();
    auto wallet = MakeEncryptedWalletFile(m_node, "bls-refused-encrypted", passphrase, master_key,
                                          std::make_unique<RefusingDatabase>(CreateMockWalletDatabase(), refused));
    CBLSSecretKey first_sk;
    first_sk.MakeNewKey();
    const CScript first = ScriptOf(PubKeyOf(first_sk));
    WriteCryptedRecord(*wallet, 0, master_key, first_sk, PubKeyOf(first_sk));
    BOOST_REQUIRE(wallet->BLSWalletInit());
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    BOOST_REQUIRE(Signs(*wallet, first));
    const std::map<unsigned int, std::string> addresses = wallet->GetBLSAddresses();
    BOOST_REQUIRE_EQUAL(addresses.size(), 1U);

    refused->types = {DBKeys::BLSKEY, DBKeys::CRYPTED_BLSKEY};
    BOOST_CHECK(!wallet->WriteToBLSWallet(sk.ToString()));
    BOOST_CHECK_EQUAL(refused->refusals, 1);
    CheckNotHeld(*wallet, addresses, pubkey, 1, "after the refused write");

    refused->throws = true;
    BOOST_CHECK_THROW(wallet->WriteToBLSWallet(sk.ToString()), std::bad_alloc);
    BOOST_CHECK_EQUAL(refused->refusals, 2);
    CheckNotHeld(*wallet, addresses, pubkey, 1, "after the write that threw");
    refused->throws = false;

    BOOST_REQUIRE(wallet->Lock());
    BOOST_CHECK(wallet->GetBLSAddresses() == addresses);
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_NO);
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    CheckNotHeld(*wallet, addresses, pubkey, 1, "after a lock and an unlock");

    BOOST_REQUIRE(wallet->BLSWalletInit());
    CheckNotHeld(*wallet, addresses, pubkey, 1, "after the records are read again");
    BOOST_CHECK(Signs(*wallet, first));

    // Once the database writes, the same key is saved as the wallet's second one, in an encrypted record that
    // carries its public key: locked, the wallet knows the key's outputs, and the unlock brings its secret back.
    refused->types.clear();
    BOOST_REQUIRE(wallet->WriteToBLSWallet(sk.ToString()));
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);
    CPubKey stored_pubkey;
    std::vector<unsigned char> crypted;
    std::string plain;
    BOOST_CHECK(WalletBatch(wallet->GetDatabase()).ReadCryptedBLSKey(1, stored_pubkey, crypted));
    BOOST_CHECK(stored_pubkey == pubkey);
    BOOST_CHECK(!WalletBatch(wallet->GetDatabase()).ReadBLSKey(1, plain));
    BOOST_REQUIRE(wallet->Lock());
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
    BOOST_CHECK(!Signs(*wallet, held));
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    BOOST_CHECK(Signs(*wallet, held));
    BOOST_CHECK(Signs(*wallet, first));
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);

    // As the next start reads the records: the saved key is there, and the unlock gives it its secret.
    BOOST_REQUIRE(wallet->BLSWalletInit());
    BOOST_REQUIRE(wallet->Lock());
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    BOOST_CHECK(Signs(*wallet, held));
    BOOST_CHECK_EQUAL(wallet->GetBLSAddresses().size(), 2U);
}

// An unlocked wallet can know a key by its public key alone: two keys an older version left in plaintext in an
// encrypted wallet, the first unlock moving the first to an encrypted record and refused the second's, and the next
// unlock stopping the move where the first plaintext record is gone (inherited, r4 R26) and loading the first's secret
// alone. The second key's import then gives that key its secret; a refused write has to take the secret back, or the
// wallet signs with a key whose record it does not have, and takes the next import for a duplicate (review r4, R39).
BOOST_FIXTURE_TEST_CASE(a_bls_key_known_by_its_public_key_keeps_no_refused_secret, OpenedWalletSetup)
{
    const auto refused = std::make_shared<RefusedRecords>();
    auto wallet = MakeEncryptedWalletFile(m_node, "bls-refused-public-only", passphrase, master_key,
                                          std::make_unique<RefusingDatabase>(CreateMockWalletDatabase(), refused));
    CBLSSecretKey first_sk;
    first_sk.MakeNewKey();
    const CScript first = ScriptOf(PubKeyOf(first_sk));
    {
        std::string first_hex = first_sk.ToString();
        std::string hex = sk.ToString();
        WalletBatch batch(wallet->GetDatabase());
        BOOST_REQUIRE(batch.WriteBLSKey(0, first_hex));
        BOOST_REQUIRE(batch.WriteBLSKey(1, hex));
    }
    BOOST_REQUIRE(wallet->BLSWalletInit());
    BOOST_REQUIRE(wallet->IsLocked());
    BOOST_REQUIRE_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);

    refused->types = {DBKeys::CRYPTED_BLSKEY};
    refused->let_through = 1;
    BOOST_CHECK(!wallet->Unlock(passphrase));
    BOOST_CHECK_EQUAL(refused->refusals, 1);
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    BOOST_REQUIRE(Signs(*wallet, first));
    BOOST_REQUIRE(!Signs(*wallet, held));
    const std::map<unsigned int, std::string> addresses = wallet->GetBLSAddresses();
    BOOST_REQUIRE_EQUAL(addresses.size(), 2U);

    BOOST_CHECK(!wallet->WriteToBLSWallet(sk.ToString()));
    BOOST_CHECK_EQUAL(refused->refusals, 2);
    BOOST_CHECK(!Signs(*wallet, held));
    CKey secret;
    BOOST_CHECK(!wallet->GetBLSKey(pubkey.GetID(), secret));
    // The wallet's own entry never took the secret (review r7, R50: nothing in it to take back).
    BOOST_CHECK(!wallet->GetBLSSigningProvider().GetKey(pubkey.GetID(), secret));
    BOOST_CHECK_EQUAL(Mine(*wallet, held), ISMINE_SPENDABLE);
    BOOST_CHECK(wallet->GetBLSAddresses() == addresses);
    BOOST_CHECK(Signs(*wallet, first));

    // Once the database writes, the import saves the key in its own encrypted record, its entry takes the secret, and
    // the wallet signs with it.
    refused->types.clear();
    BOOST_REQUIRE(wallet->WriteToBLSWallet(sk.ToString()));
    BOOST_CHECK(Signs(*wallet, held));
    BOOST_CHECK(wallet->GetBLSSigningProvider().GetKey(pubkey.GetID(), secret));
    BOOST_CHECK(wallet->GetBLSAddresses() == addresses);
    CPubKey stored_pubkey;
    std::vector<unsigned char> crypted;
    BOOST_CHECK(WalletBatch(wallet->GetDatabase()).ReadCryptedBLSKey(1, stored_pubkey, crypted));
    BOOST_CHECK(stored_pubkey == pubkey);
    BOOST_REQUIRE(wallet->Lock());
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    BOOST_CHECK(Signs(*wallet, held));
    BOOST_CHECK(Signs(*wallet, first));
}

// A wallet made without private keys took a BLS secret key all the same: `bls generate` saved one in it, and a
// legacy one that paid a BLS address made one for its change.
BOOST_AUTO_TEST_CASE(a_wallet_without_private_keys_takes_no_bls_key)
{
    for (const bool descriptors : {false, true}) {
        const std::string kind{descriptors ? "descriptor" : "legacy"};
        auto wallet = std::make_shared<CWallet>(m_node.chain.get(), m_node.coinjoin_loader.get(), "bls-no-private-keys-" + kind, CreateMockWalletDatabase());
        wallet->LoadWallet();
        {
            LOCK(wallet->cs_wallet);
            wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
            if (descriptors) {
                wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            } else {
                wallet->SetupLegacyScriptPubKeyMan();
            }
        }
        CBLSSecretKey sk;
        sk.MakeNewKey();
        const std::map<unsigned int, std::string> no_addresses;
        BOOST_CHECK_MESSAGE(!wallet->WriteToBLSWallet(sk.ToString()), kind << ": the key was written");
        BOOST_CHECK_MESSAGE(wallet->GenerateNewBLSChangeAddress().empty(), kind << ": a change key was made");
        CheckNotHeld(*wallet, no_addresses, PubKeyOf(sk), 0, kind);
        BOOST_REQUIRE(wallet->BLSWalletInit());
        CheckNotHeld(*wallet, no_addresses, PubKeyOf(sk), 0, kind + ", after the records are read again");
    }
}

BOOST_AUTO_TEST_SUITE_END()
