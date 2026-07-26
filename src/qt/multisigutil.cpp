// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/multisigutil.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <pubkey.h>
#include <qt/guiutil.h>
#include <qt/qrimagewidget.h>
#include <qt/walletmodel.h>
#include <script/script.h>
#include <script/standard.h>
#include <util/strencodings.h>
#include <univalue.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h> /* for USE_QRCODE */
#endif

#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#include <variant>

namespace {
constexpr int SETUP_FORMAT_VERSION = 1;
const QString SETUP_FORMAT_TYPE = QStringLiteral("defcon-multisig-setup");

QString SettingsKey(const QString& wallet_name)
{
    // QSettings treats '/' as a group separator; encode the wallet name to keep one flat key.
    return QStringLiteral("MultisigTracked_") + QString::fromLatin1(wallet_name.toUtf8().toHex());
}

QJsonObject EntryToJson(const MultisigUtil::Entry& entry)
{
    QJsonObject obj;
    obj["label"] = entry.label;
    obj["address"] = entry.address;
    obj["redeem_script"] = entry.redeem_script_hex;
    obj["descriptor"] = entry.descriptor;
    obj["required"] = entry.required_sigs;
    obj["pubkeys"] = QJsonArray::fromStringList(entry.pubkeys);
    obj["cosigner_labels"] = QJsonArray::fromStringList(entry.cosigner_labels);
    obj["network"] = entry.network;
    return obj;
}

MultisigUtil::Entry EntryFromJson(const QJsonObject& obj)
{
    MultisigUtil::Entry entry;
    entry.label = obj["label"].toString();
    entry.address = obj["address"].toString();
    entry.redeem_script_hex = obj["redeem_script"].toString();
    entry.descriptor = obj["descriptor"].toString();
    entry.required_sigs = obj["required"].toInt();
    for (const QJsonValue& value : obj["pubkeys"].toArray()) {
        entry.pubkeys << value.toString();
    }
    for (const QJsonValue& value : obj["cosigner_labels"].toArray()) {
        entry.cosigner_labels << value.toString();
    }
    // Ignore stray labels beyond the number of keys.
    while (entry.cosigner_labels.size() > entry.pubkeys.size()) {
        entry.cosigner_labels.removeLast();
    }
    entry.network = obj["network"].toString();
    return entry;
}

void SaveEntries(const QString& wallet_name, const QList<MultisigUtil::Entry>& entries)
{
    QJsonArray array;
    for (const MultisigUtil::Entry& entry : entries) {
        array.append(EntryToJson(entry));
    }
    QSettings settings;
    settings.setValue(SettingsKey(wallet_name), QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

//! All stored entries regardless of network. Mutating operations must use this
//! (and not the filtered public LoadEntries), otherwise saving would silently
//! drop entries that belong to other networks.
QList<MultisigUtil::Entry> LoadAllEntries(const QString& wallet_name)
{
    QList<MultisigUtil::Entry> entries;
    QSettings settings;
    const QByteArray raw = settings.value(SettingsKey(wallet_name)).toString().toUtf8();
    if (raw.isEmpty()) return entries;

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) return entries;
    for (const QJsonValue& value : doc.array()) {
        MultisigUtil::Entry entry = EntryFromJson(value.toObject());
        if (entry.isValid()) entries << entry;
    }
    return entries;
}

//! Upper bound for QR payloads; QR version 40 stores ~2953 8-bit characters at
//! the lowest error correction level, keep a margin below that.
constexpr int MAX_QR_PAYLOAD = 2900;
} // namespace

namespace MultisigUtil {

QList<Entry> LoadEntries(const QString& wallet_name)
{
    // Only profiles usable on the current network are shown; entries saved by
    // older versions have no network recorded and are assumed to belong to it.
    QList<Entry> entries;
    const QString current = CurrentNetworkId();
    for (const Entry& entry : LoadAllEntries(wallet_name)) {
        if (entry.network.isEmpty() || entry.network == current) entries << entry;
    }
    return entries;
}

bool AddEntry(const QString& wallet_name, const Entry& entry)
{
    Entry stamped = entry;
    if (stamped.network.isEmpty()) stamped.network = CurrentNetworkId();
    QList<Entry> entries = LoadAllEntries(wallet_name);
    for (const Entry& existing : entries) {
        if (existing.address == stamped.address) return false;
    }
    entries << stamped;
    SaveEntries(wallet_name, entries);
    return true;
}

bool RemoveEntry(const QString& wallet_name, const QString& address)
{
    QList<Entry> entries = LoadAllEntries(wallet_name);
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).address == address) {
            entries.removeAt(i);
            SaveEntries(wallet_name, entries);
            return true;
        }
    }
    return false;
}

Entry FindEntry(const QString& wallet_name, const QString& address)
{
    for (const Entry& entry : LoadAllEntries(wallet_name)) {
        if (entry.address == address) return entry;
    }
    return Entry{};
}

bool UpdateEntry(const QString& wallet_name, const Entry& entry)
{
    QList<Entry> entries = LoadAllEntries(wallet_name);
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).address == entry.address) {
            entries[i] = entry;
            SaveEntries(wallet_name, entries);
            return true;
        }
    }
    return false;
}

KeyTokenType ClassifyKeyToken(const QString& token)
{
    const std::string str = token.toStdString();
    if (IsHex(str)) {
        const std::vector<unsigned char> data = ParseHex(str);
        if (data.size() == CPubKey::COMPRESSED_SIZE || data.size() == CPubKey::SIZE) {
            CPubKey pubkey(data);
            if (pubkey.IsFullyValid()) return KeyTokenType::PUBKEY;
        }
    }
    if (IsValidDestinationString(str)) return KeyTokenType::ADDRESS;
    return KeyTokenType::INVALID;
}

std::string WalletRpcUri(const WalletModel& wallet_model)
{
    const QByteArray encoded_name = QUrl::toPercentEncoding(wallet_model.getWalletName());
    return "/wallet/" + std::string(encoded_name.constData(), encoded_name.length());
}

WalletStorageType GetWalletStorageType(WalletModel& wallet_model)
{
    try {
        const UniValue info = wallet_model.node().executeRpc("getwalletinfo", UniValue(UniValue::VARR), WalletRpcUri(wallet_model));
        const UniValue descriptors = find_value(info, "descriptors");
        if (!descriptors.isBool()) return WalletStorageType::UNKNOWN;
        return descriptors.get_bool() ? WalletStorageType::DESCRIPTOR : WalletStorageType::LEGACY;
    } catch (...) {
        return WalletStorageType::UNKNOWN;
    }
}

bool ImportWatchOnly(WalletModel& wallet_model, const Entry& entry, QString& error)
{
    switch (GetWalletStorageType(wallet_model)) {
    case WalletStorageType::DESCRIPTOR:
        error = QObject::tr("Descriptor wallets do not support the legacy importmulti path. Import the profile descriptor manually with importdescriptors.");
        return false;
    case WalletStorageType::UNKNOWN:
        error = QObject::tr("Wallet type could not be determined. No legacy watch-only import was attempted.");
        return false;
    case WalletStorageType::LEGACY:
        break;
    }

    try {
        UniValue request(UniValue::VOBJ);
        UniValue script_pub_key(UniValue::VOBJ);
        script_pub_key.pushKV("address", entry.address.toStdString());
        request.pushKV("scriptPubKey", script_pub_key);
        if (!entry.redeem_script_hex.isEmpty()) request.pushKV("redeemscript", entry.redeem_script_hex.toStdString());
        UniValue pubkeys(UniValue::VARR);
        for (const QString& key : entry.pubkeys) {
            if (ClassifyKeyToken(key) == KeyTokenType::PUBKEY) pubkeys.push_back(key.toStdString());
        }
        request.pushKV("pubkeys", pubkeys);
        request.pushKV("timestamp", "now");
        request.pushKV("watchonly", true);
        request.pushKV("label", entry.label.toStdString());
        request.pushKV("keypool", false);
        UniValue requests(UniValue::VARR);
        requests.push_back(request);
        UniValue params(UniValue::VARR);
        params.push_back(requests);

        const UniValue result = wallet_model.node().executeRpc("importmulti", params, WalletRpcUri(wallet_model));
        if (result.isArray() && result.size() == 1) {
            const UniValue& item = result[0];
            const UniValue success = find_value(item, "success");
            if (success.isBool() && !success.get_bool()) {
                const UniValue item_error = find_value(item, "error");
                error = QString::fromStdString(item_error.isObject() ? item_error.write() : "importmulti failed");
                return false;
            }
        }
        return true;
    } catch (const UniValue& obj_error) {
        error = QString::fromStdString(obj_error.write());
        return false;
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
        return false;
    }
}

QString WatchOnlyImportErrorMessage(const QString& error)
{
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(error.toUtf8(), &parse_error);
    if (document.isObject() && document.object().value(QStringLiteral("code")).toInt() == -13) {
        return QObject::tr("Wallet is locked. Unlock the wallet before importing the multisig setup.");
    }
    return QObject::tr("Watch-only import failed: %1").arg(error);
}

QByteArray EntryToSetupJson(const Entry& entry, bool compact)
{
    QJsonObject obj = EntryToJson(entry);
    obj["type"] = SETUP_FORMAT_TYPE;
    obj["version"] = SETUP_FORMAT_VERSION;
    return QJsonDocument(obj).toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented);
}

Entry EntryFromSetupJson(const QByteArray& json, QString& error)
{
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse_error);
    if (doc.isNull() || !doc.isObject()) {
        error = parse_error.errorString();
        return Entry{};
    }
    const QJsonObject obj = doc.object();
    if (obj["type"].toString() != SETUP_FORMAT_TYPE) {
        error = QObject::tr("Not a multisig setup file (unexpected type).");
        return Entry{};
    }
    if (obj["version"].toInt() > SETUP_FORMAT_VERSION) {
        error = QObject::tr("Setup file was created by a newer version.");
        return Entry{};
    }
    Entry entry = EntryFromJson(obj);
    if (!entry.isValid() || entry.pubkeys.isEmpty() || entry.required_sigs < 1 ||
        entry.required_sigs > entry.pubkeys.size() || entry.pubkeys.size() > 20) {
        error = QObject::tr("Setup file is missing required fields or is inconsistent.");
        return Entry{};
    }
    QSet<QString> seen_keys;
    for (int i = 0; i < entry.pubkeys.size(); ++i) {
        if (ClassifyKeyToken(entry.pubkeys.at(i)) != KeyTokenType::PUBKEY) {
            error = QObject::tr("Setup file key #%1 is not a valid hex public key.").arg(i + 1);
            return Entry{};
        }
        const QString normalized = entry.pubkeys.at(i).toLower();
        if (seen_keys.contains(normalized)) {
            error = QObject::tr("Setup file key #%1 is a duplicate.").arg(i + 1);
            return Entry{};
        }
        seen_keys.insert(normalized);
    }
    if (!entry.network.isEmpty() && entry.network != CurrentNetworkId()) {
        error = QObject::tr("This setup is for %1 but this node runs on %2.")
                    .arg(NetworkDisplayName(entry.network), NetworkDisplayName(CurrentNetworkId()));
        return Entry{};
    }
    return entry;
}

QString CurrentNetworkId()
{
    return QString::fromStdString(Params().NetworkIDString());
}

QString NetworkDisplayName(const QString& network_id)
{
    const std::string id = network_id.toStdString();
    if (id == CBaseChainParams::MAIN) return QObject::tr("Mainnet");
    if (id == CBaseChainParams::TESTNET) return QObject::tr("Testnet");
    if (id == CBaseChainParams::REGTEST) return QObject::tr("Regtest");
    if (id == CBaseChainParams::DEVNET) return QObject::tr("Devnet");
    return network_id;
}

QString ShortenKey(const QString& hex)
{
    if (hex.length() <= 20) return hex;
    return hex.left(12) + QStringLiteral("…") + hex.right(6);
}

QString CosignerDisplayName(const Entry& entry, const QString& pubkey_hex)
{
    for (int i = 0; i < entry.pubkeys.size(); ++i) {
        if (entry.pubkeys.at(i).compare(pubkey_hex, Qt::CaseInsensitive) == 0) {
            const QString label = entry.cosignerLabel(i);
            if (!label.isEmpty()) return label;
            break;
        }
    }
    return ShortenKey(pubkey_hex.toLower());
}

bool WalletHasKey(WalletModel& wallet_model, const QString& pubkey_hex)
{
    if (ClassifyKeyToken(pubkey_hex) != KeyTokenType::PUBKEY) return false;
    const CPubKey pubkey(ParseHex(pubkey_hex.toStdString()));
    try {
        UniValue params(UniValue::VARR);
        params.push_back(EncodeDestination(PKHash(pubkey)));
        const UniValue info = wallet_model.node().executeRpc("getaddressinfo", params, WalletRpcUri(wallet_model));
        const UniValue ismine = find_value(info, "ismine");
        return ismine.isBool() && ismine.get_bool();
    } catch (...) {
        return false;
    }
}

ScriptAddressMatch RedeemScriptMatchesAddress(const Entry& entry)
{
    if (entry.redeem_script_hex.isEmpty() || !IsHex(entry.redeem_script_hex.toStdString())) {
        return ScriptAddressMatch::UNKNOWN;
    }
    const CTxDestination dest = DecodeDestination(entry.address.toStdString());
    const ScriptHash* script_hash = std::get_if<ScriptHash>(&dest);
    if (script_hash == nullptr) return ScriptAddressMatch::UNKNOWN;
    const std::vector<unsigned char> data = ParseHex(entry.redeem_script_hex.toStdString());
    const CScript redeem_script(data.begin(), data.end());
    return ScriptHash(redeem_script) == *script_hash ? ScriptAddressMatch::MATCH : ScriptAddressMatch::MISMATCH;
}

QList<ValidationCheck> BuildValidationReport(WalletModel* wallet_model, const Entry& entry)
{
    using State = ValidationCheck::State;
    QList<ValidationCheck> checks;

    const bool address_ok = IsValidDestinationString(entry.address.toStdString());
    checks.append({address_ok ? State::OK : State::FAIL, QObject::tr("Address valid"),
                   address_ok ? QString() : QObject::tr("not a valid address on this network")});

    switch (RedeemScriptMatchesAddress(entry)) {
    case ScriptAddressMatch::MATCH:
        checks.append({State::OK, QObject::tr("Redeem script matches the address"), QString()});
        break;
    case ScriptAddressMatch::MISMATCH:
        checks.append({State::FAIL, QObject::tr("Redeem script does not match the address"),
                       QObject::tr("do not use this profile")});
        break;
    case ScriptAddressMatch::UNKNOWN:
        checks.append({State::UNKNOWN, QObject::tr("Redeem script"), QObject::tr("not stored in this profile")});
        break;
    }

    const bool scheme_ok = entry.required_sigs >= 1 && entry.required_sigs <= entry.totalKeys();
    checks.append({scheme_ok ? State::OK : State::FAIL, QObject::tr("Required signatures"),
                   QObject::tr("%1 of %2").arg(entry.required_sigs).arg(entry.totalKeys())});

    if (entry.network.isEmpty()) {
        checks.append({State::UNKNOWN, QObject::tr("Network"), QObject::tr("not recorded in this profile")});
    } else if (entry.network == CurrentNetworkId()) {
        checks.append({State::OK, QObject::tr("Network"), NetworkDisplayName(entry.network)});
    } else {
        checks.append({State::FAIL, QObject::tr("Network"),
                       QObject::tr("profile is for %1, this node runs on %2")
                           .arg(NetworkDisplayName(entry.network), NetworkDisplayName(CurrentNetworkId()))});
    }

    if (wallet_model == nullptr) {
        checks.append({State::UNKNOWN, QObject::tr("Wallet can sign"), QObject::tr("no wallet loaded")});
        return checks;
    }

    bool tracked = false;
    bool tracked_known = false;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(entry.address.toStdString());
        const UniValue info = wallet_model->node().executeRpc("getaddressinfo", params, WalletRpcUri(*wallet_model));
        const UniValue ismine = find_value(info, "ismine");
        const UniValue iswatchonly = find_value(info, "iswatchonly");
        tracked = (ismine.isBool() && ismine.get_bool()) || (iswatchonly.isBool() && iswatchonly.get_bool());
        tracked_known = true;
    } catch (...) {
    }
    if (tracked_known) {
        checks.append({tracked ? State::OK : State::WARN, QObject::tr("Tracked by wallet"),
                       tracked ? QString() : QObject::tr("import as watch-only to see the balance and build spends")});
    } else {
        checks.append({State::UNKNOWN, QObject::tr("Tracked by wallet"), QObject::tr("could not query the wallet")});
    }

    if (wallet_model->wallet().privateKeysDisabled()) {
        checks.append({State::WARN, QObject::tr("Wallet can sign"), QObject::tr("no — private keys are disabled")});
    } else {
        QStringList ours;
        for (int i = 0; i < entry.pubkeys.size(); ++i) {
            if (WalletHasKey(*wallet_model, entry.pubkeys.at(i))) {
                const QString label = entry.cosignerLabel(i);
                ours << (label.isEmpty() ? ShortenKey(entry.pubkeys.at(i)) : label);
            }
        }
        if (!ours.isEmpty()) {
            checks.append({State::OK, QObject::tr("Wallet can sign"),
                           QObject::tr("yes — holds %1 of %2 keys (%3)")
                               .arg(ours.size()).arg(entry.totalKeys()).arg(ours.join(QLatin1String(", ")))});
        } else {
            checks.append({State::WARN, QObject::tr("Wallet can sign"),
                           QObject::tr("no cosigner key in this wallet — you can still track it and pass the PSBT on")});
        }
    }
    return checks;
}

bool IsProfileSpendable(const Entry& entry, QString& error)
{
    QStringList failures;
    for (const ValidationCheck& check : BuildValidationReport(nullptr, entry)) {
        if (check.state != ValidationCheck::State::FAIL) continue;
        QString failure = check.text;
        if (!check.detail.isEmpty()) failure += QStringLiteral(": ") + check.detail;
        failures << failure;
    }
    error = failures.join(QLatin1String("; "));
    return failures.isEmpty();
}

void ShowQrDialog(QWidget* parent, const QString& title, const QString& payload, const QString& note)
{
    QDialog dialog(parent, GUIUtil::dialog_flags);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);

    auto* qr_widget = new QRImageWidget(&dialog);
    layout->addWidget(qr_widget, 0, Qt::AlignCenter);

    bool encoded = false;
#ifdef USE_QRCODE
    if (payload.length() > MAX_QR_PAYLOAD) {
        qr_widget->setText(QObject::tr("The payload is too large for a QR code (%1 characters). Use file or clipboard transfer instead.").arg(payload.length()));
    } else {
        encoded = qr_widget->setQR(payload, QString(), MAX_QR_PAYLOAD);
    }
#else
    qr_widget->setText(QObject::tr("This build was compiled without QR code support."));
#endif

    auto* note_label = new QLabel(note, &dialog);
    note_label->setWordWrap(true);
    layout->addWidget(note_label);

    auto* buttons = new QHBoxLayout();
    auto* save_button = new QPushButton(QObject::tr("Save image…"), &dialog);
    auto* copy_button = new QPushButton(QObject::tr("Copy image"), &dialog);
    auto* close_button = new QPushButton(QObject::tr("Close"), &dialog);
    save_button->setEnabled(encoded);
    copy_button->setEnabled(encoded);
    buttons->addWidget(save_button);
    buttons->addWidget(copy_button);
    buttons->addStretch(1);
    buttons->addWidget(close_button);
    layout->addLayout(buttons);

    QObject::connect(save_button, &QPushButton::clicked, qr_widget, &QRImageWidget::saveImage);
    QObject::connect(copy_button, &QPushButton::clicked, qr_widget, &QRImageWidget::copyImage);
    QObject::connect(close_button, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
}

} // namespace MultisigUtil
