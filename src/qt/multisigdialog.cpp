// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/multisigdialog.h>

#include <interfaces/node.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>
#include <univalue.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>

#include <stdexcept>
#include <string>

namespace {
QStringList ParseKeyTokens(const QString& input)
{
    return input.split(QRegularExpression("[,\\n\\r\\t ]+"), Qt::SkipEmptyParts);
}

QString FormatRpcError(const UniValue& obj_error)
{
    try {
        const int code = find_value(obj_error, "code").get_int();
        const std::string message = find_value(obj_error, "message").get_str();
        return QString::fromStdString(message) + QString(" (code %1)").arg(code);
    } catch (const std::runtime_error&) {
        return QString::fromStdString(obj_error.write());
    }
}
} // namespace

MultisigDialog::MultisigDialog(QWidget* parent, WalletModel* wallet_model, ClientModel* client_model)
    : QDialog(parent, GUIUtil::dialog_flags),
      m_wallet_model(wallet_model),
      m_client_model(client_model)
{
    setWindowTitle(tr("Create / Add Multisig"));
    resize(920, 720);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr("Create a multisignature address (create-only), or add a multisignature address to the currently selected wallet."));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form_group = new QGroupBox(tr("Input"), this);
    auto* form = new QFormLayout(form_group);

    m_mode_combo = new QComboBox(form_group);
    m_mode_combo->addItem(tr("Create only (createmultisig)"));
    m_mode_combo->addItem(tr("Add to current wallet (addmultisigaddress)"));
    form->addRow(tr("Mode"), m_mode_combo);

    m_required_spin = new QSpinBox(form_group);
    m_required_spin->setRange(1, 20);
    m_required_spin->setValue(2);
    form->addRow(tr("Required signatures"), m_required_spin);

    m_keys_edit = new QPlainTextEdit(form_group);
    m_keys_edit->setPlaceholderText(tr("Paste one key per line (or separated by comma/space). Create-only mode accepts hex public keys; add-to-wallet mode also accepts addresses known to this wallet."));
    m_keys_edit->setMinimumHeight(120);
    form->addRow(tr("Keys / addresses"), m_keys_edit);

    m_validation_label = new QLabel(form_group);
    m_validation_label->setWordWrap(true);
    form->addRow(QString(), m_validation_label);

    m_label_edit = new QLineEdit(form_group);
    m_label_edit->setPlaceholderText(tr("Optional profile name, e.g. \"Treasury 2-of-3\", shown in the wallet and on the Multisig tab"));
    form->addRow(tr("Label"), m_label_edit);

    m_track_checkbox = new QCheckBox(tr("Track in this wallet (import as watch-only, so balance and history are shown)"), form_group);
    m_track_checkbox->setChecked(true);
    form->addRow(QString(), m_track_checkbox);

    root->addWidget(form_group);

    auto* actions = new QHBoxLayout();
    m_execute_button = new QPushButton(tr("Create multisig"), this);
    auto* clear_button = new QPushButton(tr("Clear result"), this);
    auto* close_button = new QPushButton(tr("Close"), this);
    actions->addWidget(m_execute_button);
    actions->addWidget(clear_button);
    actions->addStretch(1);
    actions->addWidget(close_button);
    root->addLayout(actions);

    auto* output_group = new QGroupBox(tr("Result"), this);
    auto* output = new QGridLayout(output_group);

    auto add_output_row = [&](int row, const QString& text, QLineEdit*& line_edit) {
        line_edit = new QLineEdit(output_group);
        line_edit->setReadOnly(true);
        auto* copy_button = new QPushButton(tr("Copy"), output_group);
        output->addWidget(new QLabel(text, output_group), row, 0);
        output->addWidget(line_edit, row, 1);
        output->addWidget(copy_button, row, 2);
        connect(copy_button, &QPushButton::clicked, this, [this, line_edit, text] {
            GUIUtil::setClipboard(line_edit->text());
            setStatus(tr("%1 copied to clipboard.").arg(text), false);
        });
    };
    add_output_row(0, tr("Address"), m_address_out);
    add_output_row(1, tr("Descriptor"), m_descriptor_out);
    add_output_row(2, tr("Redeem script"), m_redeem_out);

    m_export_setup_button = new QPushButton(tr("Export setup…"), output_group);
    m_export_setup_button->setToolTip(tr("Save the setup (keys, address, redeem script) to a file you can share with your cosigners"));
    m_export_setup_button->setEnabled(false);
    output->addWidget(m_export_setup_button, 3, 1, Qt::AlignLeft);

    auto* raw_group = new QGroupBox(tr("Show raw RPC result"), output_group);
    raw_group->setCheckable(true);
    raw_group->setChecked(false);
    auto* raw_layout = new QVBoxLayout(raw_group);
    m_raw_out = new QPlainTextEdit(raw_group);
    m_raw_out->setReadOnly(true);
    m_raw_out->setMinimumHeight(140);
    m_raw_out->setVisible(false);
    raw_layout->addWidget(m_raw_out);
    connect(raw_group, &QGroupBox::toggled, m_raw_out, &QPlainTextEdit::setVisible);
    output->addWidget(raw_group, 4, 0, 1, 3);

    root->addWidget(output_group);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    root->addWidget(m_status_label);

    connect(m_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MultisigDialog::updateModeUi);
    connect(m_keys_edit, &QPlainTextEdit::textChanged, this, &MultisigDialog::validateInputs);
    connect(m_required_spin, qOverload<int>(&QSpinBox::valueChanged), this, &MultisigDialog::validateInputs);
    connect(m_execute_button, &QPushButton::clicked, this, &MultisigDialog::executeCommand);
    connect(m_export_setup_button, &QPushButton::clicked, this, &MultisigDialog::exportSetup);
    connect(clear_button, &QPushButton::clicked, this, [this] { clearResult(); });
    connect(close_button, &QPushButton::clicked, this, &QDialog::close);

    updateModeUi();
}

void MultisigDialog::updateModeUi()
{
    const bool add_mode = currentMode() == Mode::ADD_TO_WALLET;
    m_track_checkbox->setVisible(!add_mode);
    bool tracking_available = m_wallet_model != nullptr;
    if (!add_mode && m_wallet_model) {
        const MultisigUtil::WalletStorageType wallet_type = MultisigUtil::GetWalletStorageType(*m_wallet_model);
        tracking_available = wallet_type == MultisigUtil::WalletStorageType::LEGACY;
        if (!tracking_available) {
            m_track_checkbox->setChecked(false);
            m_track_checkbox->setToolTip(wallet_type == MultisigUtil::WalletStorageType::DESCRIPTOR
                ? tr("Descriptor wallets require manual import with importdescriptors.")
                : tr("Wallet type could not be determined, so legacy watch-only import is disabled for safety."));
        }
    }
    m_track_checkbox->setEnabled(tracking_available);
    if (tracking_available) {
        m_track_checkbox->setToolTip(tr("Import as watch-only so the wallet can show balance and history."));
    } else if (!m_wallet_model) {
        m_track_checkbox->setToolTip(tr("No wallet loaded."));
    }
    m_execute_button->setText(add_mode ? tr("Add multisig to wallet") : tr("Create multisig"));
    validateInputs();
}

QStringList MultisigDialog::validatedKeys(QString& error) const
{
    const QStringList tokens = ParseKeyTokens(m_keys_edit->toPlainText());
    if (tokens.isEmpty()) {
        error = tr("Provide at least one key.");
        return {};
    }

    const bool addresses_allowed = currentMode() == Mode::ADD_TO_WALLET;
    QSet<QString> seen;
    for (int i = 0; i < tokens.size(); ++i) {
        const MultisigUtil::KeyTokenType type = MultisigUtil::ClassifyKeyToken(tokens.at(i));
        switch (type) {
        case MultisigUtil::KeyTokenType::PUBKEY:
            break;
        case MultisigUtil::KeyTokenType::ADDRESS:
            if (!addresses_allowed) {
                error = tr("Entry #%1 is an address. Create-only mode needs hex public keys; use \"Add to current wallet\" mode for addresses.").arg(i + 1);
                return {};
            }
            break;
        case MultisigUtil::KeyTokenType::INVALID:
            error = tr("Entry #%1 is not a valid public key%2.").arg(i + 1).arg(addresses_allowed ? tr(" or address") : QString());
            return {};
        }
        const QString normalized = type == MultisigUtil::KeyTokenType::PUBKEY
            ? tokens.at(i).toLower()
            : tokens.at(i);
        if (seen.contains(normalized)) {
            error = tr("Entry #%1 is a duplicate.").arg(i + 1);
            return {};
        }
        seen.insert(normalized);
    }

    if (m_required_spin->value() > tokens.size()) {
        error = tr("Required signatures (%1) cannot exceed the number of keys (%2).").arg(m_required_spin->value()).arg(tokens.size());
        return {};
    }
    return tokens;
}

void MultisigDialog::validateInputs()
{
    QString error;
    const QStringList keys = validatedKeys(error);

    if (m_keys_edit->toPlainText().trimmed().isEmpty()) {
        m_validation_label->clear();
        m_execute_button->setEnabled(false);
        return;
    }

    if (keys.isEmpty()) {
        m_validation_label->setText(error);
        m_validation_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));
        m_execute_button->setEnabled(false);
    } else {
        m_validation_label->setText(tr("Valid %1-of-%2 multisig configuration.").arg(m_required_spin->value()).arg(keys.size()));
        m_validation_label->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_PRIMARY));
        m_execute_button->setEnabled(true);
    }
}

void MultisigDialog::clearResult()
{
    m_address_out->clear();
    m_descriptor_out->clear();
    m_redeem_out->clear();
    m_raw_out->clear();
    m_status_label->clear();
    m_status_label->setVisible(false);
    m_export_setup_button->setEnabled(false);
    m_last_entry = MultisigUtil::Entry{};
}

void MultisigDialog::setStatus(const QString& message, bool is_error)
{
    m_status_label->setText(message);
    m_status_label->setStyleSheet(GUIUtil::getThemedStyleQString(is_error ? GUIUtil::ThemedStyle::TS_ERROR : GUIUtil::ThemedStyle::TS_PRIMARY));
    m_status_label->setVisible(true);
}

MultisigDialog::Mode MultisigDialog::currentMode() const
{
    return m_mode_combo->currentIndex() == static_cast<int>(Mode::ADD_TO_WALLET)
        ? Mode::ADD_TO_WALLET
        : Mode::CREATE_ONLY;
}

QString MultisigDialog::resolveToPubkey(const QString& token) const
{
    if (!m_wallet_model || MultisigUtil::ClassifyKeyToken(token) == MultisigUtil::KeyTokenType::PUBKEY) return token;
    try {
        UniValue params(UniValue::VARR);
        params.push_back(token.toStdString());
        const UniValue result = m_wallet_model->node().executeRpc("getaddressinfo", params, MultisigUtil::WalletRpcUri(*m_wallet_model));
        const UniValue pubkey = find_value(result, "pubkey");
        if (pubkey.isStr()) return QString::fromStdString(pubkey.get_str());
    } catch (...) {
        // fall through: keep the address token; setup export verification will flag it
    }
    return token;
}

void MultisigDialog::executeCommand()
{
    clearResult();

    if (!m_client_model) {
        setStatus(tr("Client model is not available."), true);
        return;
    }

    QString validation_error;
    const QStringList keys = validatedKeys(validation_error);
    if (keys.isEmpty()) {
        setStatus(validation_error, true);
        return;
    }

    const Mode mode = currentMode();
    if ((mode == Mode::ADD_TO_WALLET || m_track_checkbox->isChecked()) && !m_wallet_model) {
        if (mode == Mode::ADD_TO_WALLET) {
            setStatus(tr("No wallet is currently selected. Select a wallet first, or use create-only mode."), true);
            return;
        }
    }
    if (m_wallet_model && (mode == Mode::ADD_TO_WALLET || m_track_checkbox->isChecked())) {
        const MultisigUtil::WalletStorageType wallet_type = MultisigUtil::GetWalletStorageType(*m_wallet_model);
        if (wallet_type == MultisigUtil::WalletStorageType::DESCRIPTOR) {
            setStatus(mode == Mode::ADD_TO_WALLET
                ? tr("This is a descriptor wallet; addmultisigaddress only works with legacy wallets. Use create-only mode without watch-only tracking, then import the descriptor manually with importdescriptors.")
                : tr("This is a descriptor wallet. Uncheck watch-only tracking and import the generated descriptor manually with importdescriptors."), true);
            return;
        }
        if (wallet_type == MultisigUtil::WalletStorageType::UNKNOWN) {
            setStatus(tr("Wallet type could not be determined. No legacy multisig import will be attempted."), true);
            return;
        }
    }

    const int required = m_required_spin->value();
    const QString label = m_label_edit->text().trimmed();

    UniValue params(UniValue::VARR);
    params.push_back(UniValue(required));
    UniValue key_array(UniValue::VARR);
    for (const QString& key : keys) {
        key_array.push_back(key.toStdString());
    }
    params.push_back(key_array);

    const std::string command = mode == Mode::ADD_TO_WALLET ? "addmultisigaddress" : "createmultisig";
    std::string uri;
    if (mode == Mode::ADD_TO_WALLET) {
        if (!label.isEmpty()) params.push_back(UniValue(label.toStdString()));
        uri = MultisigUtil::WalletRpcUri(*m_wallet_model);
    }

    try {
        interfaces::Node& node = mode == Mode::ADD_TO_WALLET ? m_wallet_model->node() : m_client_model->node();
        const UniValue result = node.executeRpc(command, params, uri);

        if (!result.isObject()) {
            m_raw_out->setPlainText(QString::fromStdString(result.write(2)));
            setStatus(tr("Unexpected RPC response type."), true);
            return;
        }

        const UniValue address = find_value(result, "address");
        const UniValue redeem_script = find_value(result, "redeemScript");
        const UniValue descriptor = find_value(result, "descriptor");

        if (address.isStr()) m_address_out->setText(QString::fromStdString(address.get_str()));
        if (descriptor.isStr()) m_descriptor_out->setText(QString::fromStdString(descriptor.get_str()));
        if (redeem_script.isStr()) m_redeem_out->setText(QString::fromStdString(redeem_script.get_str()));
        m_raw_out->setPlainText(QString::fromStdString(result.write(2)));

        if (!address.isStr()) {
            setStatus(tr("RPC result is missing the address."), true);
            return;
        }

        m_last_entry.label = label;
        m_last_entry.address = m_address_out->text();
        m_last_entry.redeem_script_hex = m_redeem_out->text();
        m_last_entry.descriptor = m_descriptor_out->text();
        m_last_entry.required_sigs = required;
        // Store hex pubkeys so cosigners can verify the setup file; resolve
        // address tokens through the wallet where possible.
        m_last_entry.pubkeys.clear();
        for (const QString& key : keys) {
            m_last_entry.pubkeys << resolveToPubkey(key);
        }
        m_export_setup_button->setEnabled(true);

        const QString wallet_name = m_wallet_model ? m_wallet_model->getWalletName() : QString();
        if (MultisigUtil::FindEntry(wallet_name, m_last_entry.address).isValid()) {
            setStatus(tr("Multisig address %1 was created, but its profile is already stored and was not changed.").arg(m_last_entry.address), true);
            return;
        }

        QStringList notes;
        if (mode == Mode::CREATE_ONLY && m_wallet_model && m_track_checkbox->isChecked()) {
            QString import_error;
            if (MultisigUtil::ImportWatchOnly(*m_wallet_model, m_last_entry, import_error)) {
                notes << tr("The address is now tracked as watch-only; transactions received before now require a rescan to appear.");
            } else {
                notes << MultisigUtil::WatchOnlyImportErrorMessage(import_error);
            }
        }

        if (!MultisigUtil::AddEntry(wallet_name, m_last_entry)) {
            setStatus(tr("Multisig address %1 was created, but its profile could not be stored.").arg(m_last_entry.address), true);
            return;
        }

        QString status = tr("Multisig address %1 created.").arg(m_last_entry.address);
        if (!notes.isEmpty()) status += QLatin1Char(' ') + notes.join(QLatin1Char(' '));
        status += QLatin1Char(' ') + tr("Use \"Export setup…\" to share the configuration with your cosigners.");
        setStatus(status, false);
    } catch (const UniValue& obj_error) {
        setStatus(FormatRpcError(obj_error), true);
    } catch (const std::exception& e) {
        setStatus(QString("Error: %1").arg(QString::fromStdString(e.what())), true);
    }
}

void MultisigDialog::exportSetup()
{
    if (!m_last_entry.isValid()) return;

    const QString suggested = (m_last_entry.label.isEmpty() ? m_last_entry.address : m_last_entry.label) + QLatin1String("-multisig-setup.json");
    const QString filename = GUIUtil::getSaveFileName(this,
        tr("Export Multisig Setup"), suggested,
        tr("Multisig Setup") + QLatin1String(" (*.json)"), nullptr);
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(tr("Could not write file %1.").arg(filename), true);
        return;
    }
    file.write(MultisigUtil::EntryToSetupJson(m_last_entry));
    setStatus(tr("Setup saved to %1. Share this file with your cosigners; it contains no private keys.").arg(filename), false);
}
