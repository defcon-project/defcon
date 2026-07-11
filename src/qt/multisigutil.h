// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MULTISIGUTIL_H
#define BITCOIN_QT_MULTISIGUTIL_H

#include <QList>
#include <QString>
#include <QStringList>

#include <string>

class WalletModel;

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

/**
 * Helpers shared by the multisig GUI: a per-wallet registry of tracked
 * multisig addresses (stored in QSettings), key validation and the
 * portable setup-file format used to exchange a multisig configuration
 * between cosigners.
 *
 * The registry is GUI-side convenience metadata only; the wallet itself
 * (imported redeem script / addmultisigaddress) remains the source of
 * truth for tracking and signing.
 */
namespace MultisigUtil {

struct Entry {
    QString label; //!< profile name, e.g. "Treasury 2-of-3"
    QString address;
    QString redeem_script_hex;
    QString descriptor;
    int required_sigs{0};
    QStringList pubkeys;
    QStringList cosigner_labels; //!< parallel to pubkeys; may be shorter or empty
    QString network;             //!< chain id (main/test/regtest/...); empty in entries saved by older versions

    int totalKeys() const { return pubkeys.size(); }
    bool isValid() const { return !address.isEmpty() && required_sigs > 0; }
    //! Label of the i-th cosigner; empty if not set.
    QString cosignerLabel(int index) const
    {
        return index >= 0 && index < cosigner_labels.size() ? cosigner_labels.at(index).trimmed() : QString();
    }
};

/** Load all tracked multisig entries for a wallet (by wallet name). */
QList<Entry> LoadEntries(const QString& wallet_name);
/** Add an entry; returns false (and does nothing) if the address is already tracked. */
bool AddEntry(const QString& wallet_name, const Entry& entry);
/** Remove the entry with the given address; returns false if not found. */
bool RemoveEntry(const QString& wallet_name, const QString& address);
/** Find a tracked entry by address. Returned entry is invalid (isValid() == false) if not found. */
Entry FindEntry(const QString& wallet_name, const QString& address);
/** Replace the stored entry with the same address; returns false if the address is not tracked. */
bool UpdateEntry(const QString& wallet_name, const Entry& entry);

enum class KeyTokenType {
    PUBKEY,   //!< hex-encoded compressed/uncompressed public key
    ADDRESS,  //!< valid base58 address for the current network
    INVALID,
};

/** Classify a single key/address token typed by the user. */
KeyTokenType ClassifyKeyToken(const QString& token);

/** RPC URI selecting the wallet of the given model, for interfaces::Node::executeRpc. */
std::string WalletRpcUri(const WalletModel& wallet_model);

/** Whether the wallet is a descriptor wallet (importmulti/addmultisigaddress are legacy-only). */
bool IsDescriptorWallet(WalletModel& wallet_model);

/**
 * Import the entry into the wallet as solvable watch-only via importmulti
 * (legacy wallets only), without rescan. Returns false and sets error on failure.
 */
bool ImportWatchOnly(WalletModel& wallet_model, const Entry& entry, QString& error);

/** Serialize an entry to the portable multisig setup JSON exchanged between cosigners.
 *  Contains public data only (name, m/n, pubkeys, cosigner labels, scripts, address, network) — never keys. */
QByteArray EntryToSetupJson(const Entry& entry, bool compact = false);
/** Parse and validate a setup JSON; on failure returns an invalid entry and sets error. */
Entry EntryFromSetupJson(const QByteArray& json, QString& error);

/** Chain id of the running node (Params().NetworkIDString()). */
QString CurrentNetworkId();
/** Human-readable network name for a chain id; falls back to the raw id. */
QString NetworkDisplayName(const QString& network_id);

/** Shorten a hex key for display (first 12 and last 6 chars). */
QString ShortenKey(const QString& hex);
/** Cosigner display name: the stored label when set, otherwise the shortened pubkey. */
QString CosignerDisplayName(const Entry& entry, const QString& pubkey_hex);

/** Best-effort check whether this wallet holds the private key of a hex pubkey
 *  (getaddressinfo on the derived P2PKH address; false on RPC failure). */
bool WalletHasKey(WalletModel& wallet_model, const QString& pubkey_hex);

enum class ScriptAddressMatch {
    MATCH,
    MISMATCH,
    UNKNOWN, //!< no redeem script stored, or the address is not P2SH
};
/** Local check that the stored redeem script hashes to the P2SH address. */
ScriptAddressMatch RedeemScriptMatchesAddress(const Entry& entry);

/** One row of the pre-flight validation panel. */
struct ValidationCheck {
    enum class State { OK, WARN, FAIL, UNKNOWN };
    State state{State::UNKNOWN};
    QString text;   //!< short check description, e.g. "Address valid"
    QString detail; //!< optional extra information
};
/** Pre-flight checks for an entry: address, redeem script/address match, m-of-n,
 *  network, wallet tracking and signing capability. wallet_model may be null. */
QList<ValidationCheck> BuildValidationReport(WalletModel* wallet_model, const Entry& entry);

/** Return whether an entry may be used to build a spend. Explicit validation
 *  failures are fatal; warnings and unknown legacy metadata remain usable.
 *  On failure, error contains a human-readable summary. */
bool IsProfileSpendable(const Entry& entry, QString& error);

/** Show a payload as a QR code in a small dialog (save/copy supported). Falls back
 *  to a friendly message when the payload exceeds QR capacity or QR support is not
 *  compiled in. Scanning QR codes (import) is not offered anywhere yet: the project
 *  has no camera/scanner dependency, and adding one is out of scope — import stays
 *  file/clipboard based until a scanner backend exists. */
void ShowQrDialog(QWidget* parent, const QString& title, const QString& payload, const QString& note);

} // namespace MultisigUtil

#endif // BITCOIN_QT_MULTISIGUTIL_H
