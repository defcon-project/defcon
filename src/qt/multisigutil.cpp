// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/multisigutil.h>

#include <interfaces/node.h>
#include <key_io.h>
#include <pubkey.h>
#include <qt/walletmodel.h>
#include <util/strencodings.h>
#include <univalue.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QUrl>

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
} // namespace

namespace MultisigUtil {

QList<Entry> LoadEntries(const QString& wallet_name)
{
    QList<Entry> entries;
    QSettings settings;
    const QByteArray raw = settings.value(SettingsKey(wallet_name)).toString().toUtf8();
    if (raw.isEmpty()) return entries;

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (!doc.isArray()) return entries;
    for (const QJsonValue& value : doc.array()) {
        Entry entry = EntryFromJson(value.toObject());
        if (entry.isValid()) entries << entry;
    }
    return entries;
}

bool AddEntry(const QString& wallet_name, const Entry& entry)
{
    QList<Entry> entries = LoadEntries(wallet_name);
    for (const Entry& existing : entries) {
        if (existing.address == entry.address) return false;
    }
    entries << entry;
    SaveEntries(wallet_name, entries);
    return true;
}

bool RemoveEntry(const QString& wallet_name, const QString& address)
{
    QList<Entry> entries = LoadEntries(wallet_name);
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
    for (const Entry& entry : LoadEntries(wallet_name)) {
        if (entry.address == address) return entry;
    }
    return Entry{};
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

bool IsDescriptorWallet(WalletModel& wallet_model)
{
    try {
        const UniValue info = wallet_model.node().executeRpc("getwalletinfo", UniValue(UniValue::VARR), WalletRpcUri(wallet_model));
        const UniValue descriptors = find_value(info, "descriptors");
        return descriptors.isBool() && descriptors.get_bool();
    } catch (...) {
        return false;
    }
}

bool ImportWatchOnly(WalletModel& wallet_model, const Entry& entry, QString& error)
{
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

QByteArray EntryToSetupJson(const Entry& entry)
{
    QJsonObject obj = EntryToJson(entry);
    obj["type"] = SETUP_FORMAT_TYPE;
    obj["version"] = SETUP_FORMAT_VERSION;
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
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
    if (!entry.isValid() || entry.pubkeys.isEmpty() || entry.required_sigs > entry.pubkeys.size()) {
        error = QObject::tr("Setup file is missing required fields or is inconsistent.");
        return Entry{};
    }
    return entry;
}

} // namespace MultisigUtil
