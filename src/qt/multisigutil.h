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
    QString label;
    QString address;
    QString redeem_script_hex;
    QString descriptor;
    int required_sigs{0};
    QStringList pubkeys;

    int totalKeys() const { return pubkeys.size(); }
    bool isValid() const { return !address.isEmpty() && required_sigs > 0; }
};

/** Load all tracked multisig entries for a wallet (by wallet name). */
QList<Entry> LoadEntries(const QString& wallet_name);
/** Add an entry; returns false (and does nothing) if the address is already tracked. */
bool AddEntry(const QString& wallet_name, const Entry& entry);
/** Remove the entry with the given address; returns false if not found. */
bool RemoveEntry(const QString& wallet_name, const QString& address);
/** Find a tracked entry by address. Returned entry is invalid (isValid() == false) if not found. */
Entry FindEntry(const QString& wallet_name, const QString& address);

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

/** Serialize an entry to the portable multisig setup JSON exchanged between cosigners. */
QByteArray EntryToSetupJson(const Entry& entry);
/** Parse a setup JSON; on failure returns an invalid entry and sets error. */
Entry EntryFromSetupJson(const QByteArray& json, QString& error);

} // namespace MultisigUtil

#endif // BITCOIN_QT_MULTISIGUTIL_H
