// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/multisigpage.h>

#include <interfaces/node.h>
#include <psbt.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/multisigdialog.h>
#include <qt/multisigspenddialog.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#include <rpc/util.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <univalue.h>

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
enum TableColumn {
    COLUMN_LABEL = 0,
    COLUMN_ADDRESS,
    COLUMN_SCHEME,
    COLUMN_BALANCE,
    COLUMN_UTXOS,
    COLUMN_COUNT,
};

} // namespace

MultisigPage::MultisigPage(QWidget* parent) :
    QWidget(parent)
{
    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(tr("Saved multisig profiles of this wallet. Create a profile, share it with your cosigners, and spend from it with partially signed transactions."), this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* top_buttons = new QHBoxLayout();
    m_create_button = new QPushButton(tr("Create / Add…"), this);
    m_create_button->setToolTip(tr("Create a new multisignature address or add an existing one to the wallet"));
    m_import_button = new QPushButton(tr("Import setup…"), this);
    m_import_button->setToolTip(tr("Import a multisig setup file received from a cosigner"));
    m_refresh_button = new QPushButton(tr("Refresh"), this);
    top_buttons->addWidget(m_create_button);
    top_buttons->addWidget(m_import_button);
    top_buttons->addStretch(1);
    top_buttons->addWidget(m_refresh_button);
    root->addLayout(top_buttons);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(COLUMN_COUNT);
    m_table->setHorizontalHeaderLabels({tr("Profile"), tr("Address"), tr("Scheme"), tr("Balance"), tr("UTXOs")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(COLUMN_ADDRESS, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(COLUMN_LABEL, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(COLUMN_SCHEME, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(COLUMN_BALANCE, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(COLUMN_UTXOS, QHeaderView::ResizeToContents);
    root->addWidget(m_table, 1);

    m_empty_label = new QLabel(tr("No multisig profiles yet. Use \"Create / Add…\" to set one up, or \"Import setup…\" if a cosigner sent you a setup file."), this);
    m_empty_label->setWordWrap(true);
    m_empty_label->setAlignment(Qt::AlignCenter);
    root->addWidget(m_empty_label);

    // Details of the selected profile: scheme, network, labelled cosigners and
    // the raw data (kept collapsed so the default view stays simple).
    m_details_group = new QGroupBox(tr("Selected profile"), this);
    auto* details = new QVBoxLayout(m_details_group);
    m_details_header = new QLabel(m_details_group);
    m_details_header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(m_details_header);
    m_details_cosigners = new QLabel(m_details_group);
    m_details_cosigners->setWordWrap(true);
    m_details_cosigners->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(m_details_cosigners);

    auto* address_row = new QHBoxLayout();
    address_row->addWidget(new QLabel(tr("Address"), m_details_group));
    m_details_address = new QLineEdit(m_details_group);
    m_details_address->setReadOnly(true);
    auto* copy_address_details = new QPushButton(tr("Copy"), m_details_group);
    address_row->addWidget(m_details_address, 1);
    address_row->addWidget(copy_address_details);
    details->addLayout(address_row);

    auto* advanced_group = new QGroupBox(tr("Advanced (redeem script, descriptor)"), m_details_group);
    advanced_group->setCheckable(true);
    advanced_group->setChecked(false);
    auto* advanced_outer = new QVBoxLayout(advanced_group);
    auto* advanced_contents = new QWidget(advanced_group);
    auto* advanced_form = new QFormLayout(advanced_contents);
    const auto make_copy_row = [&](const QString& name, QLineEdit*& line_edit) {
        auto* row = new QHBoxLayout();
        line_edit = new QLineEdit(advanced_contents);
        line_edit->setReadOnly(true);
        auto* copy_button = new QPushButton(tr("Copy"), advanced_contents);
        row->addWidget(line_edit, 1);
        row->addWidget(copy_button);
        advanced_form->addRow(name, row);
        connect(copy_button, &QPushButton::clicked, this, [this, name, line = line_edit] {
            GUIUtil::setClipboard(line->text());
            setStatus(tr("%1 copied to clipboard.").arg(name), false);
        });
    };
    make_copy_row(tr("Redeem script"), m_details_redeem);
    make_copy_row(tr("Descriptor"), m_details_descriptor);
    advanced_outer->addWidget(advanced_contents);
    advanced_contents->setVisible(false);
    connect(advanced_group, &QGroupBox::toggled, advanced_contents, &QWidget::setVisible);
    details->addWidget(advanced_group);

    auto* details_actions = new QHBoxLayout();
    m_edit_labels_button = new QPushButton(tr("Edit cosigner labels…"), m_details_group);
    m_edit_labels_button->setToolTip(tr("Name each cosigner key (e.g. \"Alice hardware wallet\") so signature progress is easy to read"));
    m_watchonly_button = new QPushButton(tr("Import as watch-only"), m_details_group);
    m_show_qr_button = new QPushButton(tr("Show QR…"), m_details_group);
    m_show_qr_button->setToolTip(tr("Show the setup as a QR code so a cosigner can copy it. Scanning QR codes is not available in this build."));
    details_actions->addWidget(m_edit_labels_button);
    details_actions->addWidget(m_watchonly_button);
    details_actions->addWidget(m_show_qr_button);
    details_actions->addStretch(1);
    details->addLayout(details_actions);
    m_details_group->setVisible(false);
    root->addWidget(m_details_group);

    auto* bottom_buttons = new QHBoxLayout();
    m_spend_button = new QPushButton(tr("Spend…"), this);
    m_spend_button->setToolTip(tr("Build a transaction spending from the selected multisig address"));
    m_continue_button = new QPushButton(tr("Continue transaction"), this);
    m_continue_button->setToolTip(tr("Load a partially signed transaction to sign, merge signatures or broadcast it"));
    auto* continue_menu = new QMenu(m_continue_button);
    continue_menu->addAction(tr("From clipboard"), [this] { continuePsbt(/*from_clipboard=*/true); });
    continue_menu->addAction(tr("From file…"), [this] { continuePsbt(/*from_clipboard=*/false); });
    m_continue_button->setMenu(continue_menu);
    m_export_button = new QPushButton(tr("Export setup…"), this);
    m_export_button->setToolTip(tr("Save the selected setup to a file you can share with your cosigners"));
    m_copy_address_button = new QPushButton(tr("Copy address"), this);
    m_remove_button = new QPushButton(tr("Remove from list"), this);
    m_remove_button->setToolTip(tr("Remove the selected entry from this list. The wallet itself keeps tracking the address."));
    bottom_buttons->addWidget(m_spend_button);
    bottom_buttons->addWidget(m_continue_button);
    bottom_buttons->addWidget(m_export_button);
    bottom_buttons->addWidget(m_copy_address_button);
    bottom_buttons->addStretch(1);
    bottom_buttons->addWidget(m_remove_button);
    root->addLayout(bottom_buttons);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    root->addWidget(m_status_label);

    connect(m_create_button, &QPushButton::clicked, this, &MultisigPage::createMultisig);
    connect(m_import_button, &QPushButton::clicked, this, &MultisigPage::importSetup);
    connect(m_refresh_button, &QPushButton::clicked, this, &MultisigPage::refresh);
    connect(m_spend_button, &QPushButton::clicked, this, &MultisigPage::spendSelected);
    connect(m_export_button, &QPushButton::clicked, this, &MultisigPage::exportSetup);
    connect(m_copy_address_button, &QPushButton::clicked, this, &MultisigPage::copyAddress);
    connect(m_remove_button, &QPushButton::clicked, this, &MultisigPage::removeSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MultisigPage::updateButtonStates);
    connect(m_table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem*) { spendSelected(); });
    connect(copy_address_details, &QPushButton::clicked, this, &MultisigPage::copyAddress);
    connect(m_edit_labels_button, &QPushButton::clicked, this, &MultisigPage::editCosignerLabels);
    connect(m_watchonly_button, &QPushButton::clicked, this, &MultisigPage::importSelectedWatchOnly);
    connect(m_show_qr_button, &QPushButton::clicked, this, &MultisigPage::showProfileQr);

    updateButtonStates();
}

void MultisigPage::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
}

void MultisigPage::setWalletModel(WalletModel* wallet_model)
{
    m_wallet_model = wallet_model;
    m_is_descriptor_wallet.reset();
    refresh();
}

void MultisigPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void MultisigPage::refresh()
{
    m_entries = MultisigUtil::LoadEntries(m_wallet_model ? m_wallet_model->getWalletName() : QString());

    QHash<QString, CAmount> balances;
    QHash<QString, int> utxo_counts;
    QString balance_error;
    const bool have_balances = !m_entries.isEmpty() && queryBalances(balances, utxo_counts, balance_error);

    const int display_unit = (m_wallet_model && m_wallet_model->getOptionsModel())
        ? m_wallet_model->getOptionsModel()->getDisplayUnit()
        : BitcoinUnits::DASH;

    m_table->setRowCount(m_entries.size());
    for (int row = 0; row < m_entries.size(); ++row) {
        const MultisigUtil::Entry& entry = m_entries.at(row);
        auto set_item = [&](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            m_table->setItem(row, column, item);
        };
        set_item(COLUMN_LABEL, entry.label);
        set_item(COLUMN_ADDRESS, entry.address);
        set_item(COLUMN_SCHEME, tr("%1-of-%2").arg(entry.required_sigs).arg(entry.totalKeys()));
        if (have_balances) {
            set_item(COLUMN_BALANCE, BitcoinUnits::formatWithUnit(display_unit, balances.value(entry.address, 0)));
            set_item(COLUMN_UTXOS, QString::number(utxo_counts.value(entry.address, 0)));
        } else {
            set_item(COLUMN_BALANCE, QStringLiteral("—"));
            set_item(COLUMN_UTXOS, QStringLiteral("—"));
        }
    }

    m_table->setVisible(!m_entries.isEmpty());
    m_empty_label->setVisible(m_entries.isEmpty());

    if (!m_entries.isEmpty() && !have_balances && !balance_error.isEmpty()) {
        setStatus(tr("Could not query balances: %1").arg(balance_error), true);
    } else {
        m_status_label->setVisible(false);
    }
    updateButtonStates();
}

void MultisigPage::updateButtonStates()
{
    const bool has_selection = selectedEntry().isValid();
    m_spend_button->setEnabled(has_selection && m_wallet_model != nullptr);
    m_export_button->setEnabled(has_selection);
    m_copy_address_button->setEnabled(has_selection);
    m_remove_button->setEnabled(has_selection);
    m_continue_button->setEnabled(m_wallet_model != nullptr);
    m_import_button->setEnabled(m_client_model != nullptr);
    updateDetailsPanel();
}

MultisigUtil::Entry MultisigPage::selectedEntry() const
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_entries.size() || m_table->selectedItems().isEmpty()) return MultisigUtil::Entry{};
    return m_entries.at(row);
}

void MultisigPage::setStatus(const QString& message, bool is_error)
{
    m_status_label->setText(message);
    m_status_label->setStyleSheet(GUIUtil::getThemedStyleQString(is_error ? GUIUtil::ThemedStyle::TS_ERROR : GUIUtil::ThemedStyle::TS_PRIMARY));
    m_status_label->setVisible(true);
}

bool MultisigPage::queryBalances(QHash<QString, CAmount>& balances, QHash<QString, int>& utxo_counts, QString& error) const
{
    if (!m_wallet_model) {
        error = tr("No wallet loaded.");
        return false;
    }

    UniValue addresses(UniValue::VARR);
    for (const MultisigUtil::Entry& entry : m_entries) {
        addresses.push_back(entry.address.toStdString());
    }
    UniValue params(UniValue::VARR);
    params.push_back(UniValue(0));
    params.push_back(UniValue(9999999));
    params.push_back(addresses);

    try {
        const UniValue result = m_wallet_model->node().executeRpc("listunspent", params, MultisigUtil::WalletRpcUri(*m_wallet_model));
        if (!result.isArray()) {
            error = tr("Unexpected RPC response type.");
            return false;
        }
        for (size_t i = 0; i < result.size(); ++i) {
            const UniValue& utxo = result[i];
            const UniValue& address = find_value(utxo, "address");
            const UniValue& amount = find_value(utxo, "amount");
            if (!address.isStr() || amount.isNull()) continue;
            const QString address_str = QString::fromStdString(address.get_str());
            balances[address_str] += AmountFromValue(amount);
            utxo_counts[address_str] += 1;
        }
        return true;
    } catch (const UniValue& obj_error) {
        error = QString::fromStdString(find_value(obj_error, "message").isStr() ? find_value(obj_error, "message").get_str() : obj_error.write());
        return false;
    } catch (const std::exception& e) {
        error = QString::fromStdString(e.what());
        return false;
    }
}

void MultisigPage::createMultisig()
{
    MultisigDialog dlg(this, m_wallet_model, m_client_model);
    dlg.exec();
    refresh();
}

void MultisigPage::importSetup()
{
    if (!m_client_model) return;

    const QString filename = GUIUtil::getOpenFileName(this,
        tr("Import Multisig Setup"), QString(),
        tr("Multisig Setup") + QLatin1String(" (*.json)"), nullptr);
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus(tr("Could not open file %1.").arg(filename), true);
        return;
    }

    QString parse_error;
    const MultisigUtil::Entry entry = MultisigUtil::EntryFromSetupJson(file.readAll(), parse_error);
    if (!entry.isValid()) {
        setStatus(tr("Invalid setup file: %1").arg(parse_error), true);
        return;
    }

    // Cross-check: recompute the address from the keys so a tampered or corrupted
    // setup file cannot make the user track (and receive funds on) the wrong address.
    try {
        UniValue keys(UniValue::VARR);
        for (const QString& key : entry.pubkeys) keys.push_back(key.toStdString());
        UniValue params(UniValue::VARR);
        params.push_back(UniValue(entry.required_sigs));
        params.push_back(keys);
        const UniValue result = m_client_model->node().executeRpc("createmultisig", params, "");
        const UniValue address = find_value(result, "address");
        if (!address.isStr() || QString::fromStdString(address.get_str()) != entry.address) {
            setStatus(tr("Setup file rejected: the address does not match the keys it contains."), true);
            return;
        }
    } catch (const UniValue& obj_error) {
        setStatus(tr("Setup file verification failed: %1").arg(QString::fromStdString(obj_error.write())), true);
        return;
    } catch (const std::exception& e) {
        setStatus(tr("Setup file verification failed: %1").arg(QString::fromStdString(e.what())), true);
        return;
    }

    if (m_wallet_model) {
        const QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Track address"),
            tr("Import %1 into the wallet as watch-only so its balance and history are tracked?\n\n"
               "Note: transactions received before now will only appear after a rescan.").arg(entry.address),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (reply == QMessageBox::Yes) {
            QString import_error;
            if (!MultisigUtil::ImportWatchOnly(*m_wallet_model, entry, import_error)) {
                setStatus(tr("Watch-only import failed: %1").arg(import_error), true);
                return;
            }
        }
    }

    if (!MultisigUtil::AddEntry(m_wallet_model ? m_wallet_model->getWalletName() : QString(), entry)) {
        setStatus(tr("This multisig address is already tracked."), true);
        return;
    }
    refresh();
    setStatus(tr("Multisig setup %1 imported.").arg(entry.address), false);
}

void MultisigPage::exportSetup()
{
    const MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid()) return;

    const QString suggested = (entry.label.isEmpty() ? entry.address : entry.label) + QLatin1String("-multisig-setup.json");
    const QString filename = GUIUtil::getSaveFileName(this,
        tr("Export Multisig Setup"), suggested,
        tr("Multisig Setup") + QLatin1String(" (*.json)"), nullptr);
    if (filename.isEmpty()) return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(tr("Could not write file %1.").arg(filename), true);
        return;
    }
    file.write(MultisigUtil::EntryToSetupJson(entry));
    setStatus(tr("Setup saved to %1. Share this file with your cosigners; it contains no private keys.").arg(filename), false);
}

void MultisigPage::spendSelected()
{
    const MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid() || !m_wallet_model) return;

    MultisigSpendDialog dlg(this, m_wallet_model, m_client_model, entry);
    dlg.exec();
    refresh();
}

void MultisigPage::continuePsbt(bool from_clipboard)
{
    std::vector<unsigned char> data;
    if (from_clipboard) {
        const std::string raw = QApplication::clipboard()->text().toStdString();
        auto result = DecodeBase64(raw);
        if (!result) {
            setStatus(tr("Unable to decode PSBT from clipboard (invalid base64)."), true);
            return;
        }
        data = std::move(*result);
    } else {
        const QString filename = GUIUtil::getOpenFileName(this,
            tr("Load Transaction Data"), QString(),
            tr("Partially Signed Transaction (*.psbt)"), nullptr);
        if (filename.isEmpty()) return;
        if (GetFileSize(filename.toLocal8Bit().data(), MAX_FILE_SIZE_PSBT) == MAX_FILE_SIZE_PSBT) {
            setStatus(tr("PSBT file must be smaller than 100 MiB."), true);
            return;
        }
        std::ifstream in{filename.toLocal8Bit().data(), std::ios::binary};
        data.assign(std::istream_iterator<unsigned char>{in}, {});
    }

    std::string error;
    PartiallySignedTransaction psbtx;
    if (!DecodeRawPSBT(psbtx, MakeByteSpan(data), error)) {
        setStatus(tr("Unable to decode PSBT: %1").arg(QString::fromStdString(error)), true);
        return;
    }

    MultisigSpendDialog dlg(this, m_wallet_model, m_client_model, psbtx);
    dlg.exec();
    refresh();
}

void MultisigPage::copyAddress()
{
    const MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid()) return;
    GUIUtil::setClipboard(entry.address);
    setStatus(tr("Address %1 copied to clipboard.").arg(entry.address), false);
}

void MultisigPage::removeSelected()
{
    const MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid()) return;

    const QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Remove multisig entry"),
        tr("Remove %1 from this list?\n\nOnly the list entry is removed. If the address was imported into the wallet it remains tracked there, and funds on it are not affected.").arg(entry.address),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    MultisigUtil::RemoveEntry(m_wallet_model ? m_wallet_model->getWalletName() : QString(), entry.address);
    refresh();
}

QString MultisigPage::walletName() const
{
    return m_wallet_model ? m_wallet_model->getWalletName() : QString();
}

void MultisigPage::updateDetailsPanel()
{
    const MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid()) {
        m_details_group->setVisible(false);
        return;
    }
    m_details_group->setVisible(true);

    const QString network = entry.network.isEmpty() ? MultisigUtil::CurrentNetworkId() : entry.network;
    m_details_header->setText(tr("%1 — %2-of-%3 multisig • %4")
        .arg(entry.label.isEmpty() ? tr("(unnamed profile)") : entry.label)
        .arg(entry.required_sigs)
        .arg(entry.totalKeys())
        .arg(MultisigUtil::NetworkDisplayName(network)));

    QStringList cosigner_lines;
    for (int i = 0; i < entry.pubkeys.size(); ++i) {
        const QString label = entry.cosignerLabel(i);
        QString line = QStringLiteral("• ");
        line += label.isEmpty() ? tr("Cosigner %1").arg(i + 1) : label;
        line += QStringLiteral(" — ") + MultisigUtil::ShortenKey(entry.pubkeys.at(i));
        if (m_wallet_model && MultisigUtil::WalletHasKey(*m_wallet_model, entry.pubkeys.at(i))) {
            line += QLatin1Char(' ') + tr("(key in this wallet)");
        }
        cosigner_lines << line;
    }
    m_details_cosigners->setText(cosigner_lines.join(QLatin1Char('\n')));

    m_details_address->setText(entry.address);
    m_details_redeem->setText(entry.redeem_script_hex);
    m_details_descriptor->setText(entry.descriptor);

    // Watch-only import goes through importmulti, which only exists for legacy
    // wallets. Descriptor wallets would need an importdescriptors-based path
    // (not implemented in the multisig GUI yet), so the action is disabled
    // there with an explanation.
    bool watchonly_possible = m_wallet_model != nullptr;
    if (watchonly_possible) {
        if (!m_is_descriptor_wallet.has_value()) {
            m_is_descriptor_wallet = MultisigUtil::IsDescriptorWallet(*m_wallet_model);
        }
        watchonly_possible = !*m_is_descriptor_wallet;
    }
    m_watchonly_button->setEnabled(watchonly_possible);
    m_watchonly_button->setToolTip(watchonly_possible
        ? tr("Import the address into this wallet as watch-only so its balance and history are tracked")
        : (m_wallet_model
            ? tr("Not available for descriptor wallets: watch-only import uses importmulti, which is legacy-only. Import the profile's descriptor manually via the importdescriptors RPC instead.")
            : tr("No wallet loaded")));
    m_edit_labels_button->setEnabled(true);
}

void MultisigPage::editCosignerLabels()
{
    MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid()) return;

    QDialog dialog(this, GUIUtil::dialog_flags);
    dialog.setWindowTitle(tr("Cosigner labels"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel(tr("Give each cosigner key a recognizable name, e.g. \"Alice hardware wallet\". Labels are stored locally and included in exported setup files."), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto* form = new QFormLayout();
    QList<QLineEdit*> edits;
    for (int i = 0; i < entry.pubkeys.size(); ++i) {
        auto* edit = new QLineEdit(entry.cosignerLabel(i), &dialog);
        edit->setPlaceholderText(tr("Cosigner %1").arg(i + 1));
        form->addRow(MultisigUtil::ShortenKey(entry.pubkeys.at(i)), edit);
        edits << edit;
    }
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    entry.cosigner_labels.clear();
    for (const QLineEdit* edit : edits) {
        entry.cosigner_labels << edit->text().trimmed();
    }
    MultisigUtil::UpdateEntry(walletName(), entry);
    refresh();
    setStatus(tr("Cosigner labels updated."), false);
}

void MultisigPage::importSelectedWatchOnly()
{
    const MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid() || !m_wallet_model) return;

    QString error;
    if (!MultisigUtil::ImportWatchOnly(*m_wallet_model, entry, error)) {
        setStatus(tr("Watch-only import failed: %1").arg(error), true);
        return;
    }
    setStatus(tr("Address %1 is now tracked as watch-only. Transactions received before now appear only after a rescan.").arg(entry.address), false);
    refresh();
}

void MultisigPage::showProfileQr()
{
    const MultisigUtil::Entry entry = selectedEntry();
    if (!entry.isValid()) return;
    MultisigUtil::ShowQrDialog(this, tr("Multisig setup as QR code"),
        QString::fromUtf8(MultisigUtil::EntryToSetupJson(entry, /*compact=*/true)),
        tr("Contains the profile name, public keys, address and redeem script — no private keys."));
}
