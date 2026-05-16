// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/multisigdialog.h>

#include <interfaces/node.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/walletmodel.h>
#include <univalue.h>

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<std::string> ParseKeys(const QString& input)
{
    std::vector<std::string> keys;
    const QStringList parts = input.split(QRegularExpression("[,\\n\\r\\t ]+"), Qt::SkipEmptyParts);
    keys.reserve(parts.size());
    for (const QString& part : parts) {
        const QString trimmed = part.trimmed();
        if (!trimmed.isEmpty()) keys.emplace_back(trimmed.toStdString());
    }
    return keys;
}

QString FormatRpcError(UniValue& obj_error)
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
    if (!isAddToWalletSupported()) {
        m_mode_combo->removeItem(static_cast<int>(Mode::ADD_TO_WALLET));
        m_mode_combo->setToolTip(tr("Add-to-wallet mode is only available for legacy wallets."));
    }
    form->addRow(tr("Mode"), m_mode_combo);

    m_required_spin = new QSpinBox(form_group);
    m_required_spin->setRange(1, 20);
    m_required_spin->setValue(2);
    form->addRow(tr("Required signatures"), m_required_spin);

    m_keys_edit = new QPlainTextEdit(form_group);
    m_keys_edit->setPlaceholderText(tr("Paste keys/addresses here. Supported separators: newline, comma, or space."));
    m_keys_edit->setMinimumHeight(120);
    form->addRow(tr("Keys / addresses"), m_keys_edit);

    m_label_edit = new QLineEdit(form_group);
    m_label_edit->setPlaceholderText(tr("Optional wallet label"));
    form->addRow(tr("Wallet label"), m_label_edit);

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

    m_address_out = new QLineEdit(output_group);
    m_address_out->setReadOnly(true);
    auto* copy_address = new QPushButton(tr("Copy"), output_group);
    output->addWidget(new QLabel(tr("Address"), output_group), 0, 0);
    output->addWidget(m_address_out, 0, 1);
    output->addWidget(copy_address, 0, 2);

    m_descriptor_out = new QLineEdit(output_group);
    m_descriptor_out->setReadOnly(true);
    auto* copy_descriptor = new QPushButton(tr("Copy"), output_group);
    output->addWidget(new QLabel(tr("Descriptor"), output_group), 1, 0);
    output->addWidget(m_descriptor_out, 1, 1);
    output->addWidget(copy_descriptor, 1, 2);

    m_redeem_out = new QPlainTextEdit(output_group);
    m_redeem_out->setReadOnly(true);
    m_redeem_out->setMaximumBlockCount(1);
    auto* copy_redeem = new QPushButton(tr("Copy"), output_group);
    output->addWidget(new QLabel(tr("Redeem script"), output_group), 2, 0);
    output->addWidget(m_redeem_out, 2, 1);
    output->addWidget(copy_redeem, 2, 2);

    m_raw_out = new QPlainTextEdit(output_group);
    m_raw_out->setReadOnly(true);
    m_raw_out->setMinimumHeight(180);
    auto* copy_raw = new QPushButton(tr("Copy"), output_group);
    output->addWidget(new QLabel(tr("Raw RPC result"), output_group), 3, 0, Qt::AlignTop);
    output->addWidget(m_raw_out, 3, 1);
    output->addWidget(copy_raw, 3, 2, Qt::AlignTop);

    root->addWidget(output_group);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    root->addWidget(m_status_label);

    connect(m_mode_combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateModeUi(); });
    connect(m_execute_button, &QPushButton::clicked, this, [this] { executeCommand(); });
    connect(clear_button, &QPushButton::clicked, this, [this] { clearResult(); });
    connect(close_button, &QPushButton::clicked, this, &QDialog::close);

    connect(copy_address, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_address_out->text()); });
    connect(copy_descriptor, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_descriptor_out->text()); });
    connect(copy_redeem, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_redeem_out->toPlainText()); });
    connect(copy_raw, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_raw_out->toPlainText()); });

    updateModeUi();
}

void MultisigDialog::updateModeUi()
{
    const bool add_mode = currentMode() == Mode::ADD_TO_WALLET && isAddToWalletSupported();
    m_label_edit->setEnabled(add_mode);
    m_label_edit->setVisible(add_mode);
    m_execute_button->setText(add_mode ? tr("Add multisig to wallet") : tr("Create multisig"));
}

void MultisigDialog::clearResult()
{
    m_address_out->clear();
    m_descriptor_out->clear();
    m_redeem_out->clear();
    m_raw_out->clear();
    m_status_label->clear();
    m_status_label->setVisible(false);
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

bool MultisigDialog::isAddToWalletSupported() const
{
    return m_wallet_model && m_wallet_model->wallet().isLegacy();
}

void MultisigDialog::executeCommand()
{
    clearResult();

    if (!m_client_model) {
        setStatus(tr("Client model is not available."), true);
        return;
    }

    const std::vector<std::string> keys = ParseKeys(m_keys_edit->toPlainText());
    if (keys.empty()) {
        setStatus(tr("Please provide at least one key/address."), true);
        return;
    }

    const int required = m_required_spin->value();
    if (required > static_cast<int>(keys.size())) {
        setStatus(tr("Required signatures cannot exceed the number of provided keys/addresses."), true);
        return;
    }

    const Mode mode = currentMode();
    if (mode == Mode::ADD_TO_WALLET && !isAddToWalletSupported()) {
        setStatus(tr("Add-to-wallet mode is not supported for descriptor wallets."), true);
        return;
    }

    if (mode == Mode::ADD_TO_WALLET && !m_wallet_model) {
        setStatus(tr("No wallet is currently selected. Select a wallet first, or use create-only mode."), true);
        return;
    }

    UniValue params(UniValue::VARR);
    params.push_back(UniValue(required));

    UniValue key_array(UniValue::VARR);
    for (const std::string& key : keys) {
        key_array.push_back(UniValue(key));
    }
    params.push_back(key_array);

    std::string command = mode == Mode::ADD_TO_WALLET ? "addmultisigaddress" : "createmultisig";
    std::string uri;

    if (mode == Mode::ADD_TO_WALLET) {
        const QString label = m_label_edit->text().trimmed();
        if (!label.isEmpty()) {
            params.push_back(UniValue(label.toStdString()));
        }

        QByteArray encoded_name = QUrl::toPercentEncoding(m_wallet_model->getWalletName());
        uri = "/wallet/" + std::string(encoded_name.constData(), encoded_name.length());
    }

    try {
        interfaces::Node& node = mode == Mode::ADD_TO_WALLET ? m_wallet_model->node() : m_client_model->node();
        UniValue result = node.executeRpc(command, params, uri);

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
        if (redeem_script.isStr()) m_redeem_out->setPlainText(QString::fromStdString(redeem_script.get_str()));

        m_raw_out->setPlainText(QString::fromStdString(result.write(2)));
        setStatus(tr("Multisig command completed successfully."), false);
    } catch (UniValue& obj_error) {
        setStatus(FormatRpcError(obj_error), true);
    } catch (const std::exception& e) {
        setStatus(QString("Error: %1").arg(QString::fromStdString(e.what())), true);
    }
}
