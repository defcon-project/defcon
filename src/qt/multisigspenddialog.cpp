// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/multisigspenddialog.h>

#include <core_io.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/psbt.h>
#include <node/transaction.h>
#include <pubkey.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#include <rpc/util.h>
#include <script/script.h>
#include <script/standard.h>
#include <streams.h>
#include <util/error.h>
#include <util/strencodings.h>
#include <util/system.h>
#include <util/translation.h>
#include <univalue.h>
#include <version.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
//! Multisig signature state of one PSBT input.
struct InputSigStatus {
    bool known{false};     //!< redeem script found and is a standard multisig
    bool finalized{false}; //!< input already carries a final scriptSig
    int required{0};
    std::vector<std::pair<CPubKey, bool>> keys; //!< pubkey, has partial signature
    int have{0};
};

InputSigStatus StatusForInput(const PSBTInput& input, const CScript& fallback_redeem_script)
{
    InputSigStatus status;
    if (!input.final_script_sig.empty()) {
        status.finalized = true;
        status.known = true;
        return status;
    }

    const CScript& redeem_script = !input.redeem_script.empty() ? input.redeem_script : fallback_redeem_script;
    std::vector<std::vector<unsigned char>> solutions;
    if (redeem_script.empty() || Solver(redeem_script, solutions) != TxoutType::MULTISIG) {
        return status;
    }

    status.known = true;
    status.required = solutions.front()[0];
    for (size_t i = 1; i + 1 < solutions.size(); ++i) {
        CPubKey pubkey(solutions[i]);
        const bool have_sig = input.partial_sigs.count(pubkey.GetID()) > 0;
        status.keys.emplace_back(pubkey, have_sig);
        if (have_sig) ++status.have;
    }
    return status;
}

QString ShortKey(const CPubKey& pubkey)
{
    const QString hex = QString::fromStdString(HexStr(pubkey));
    return hex.left(16) + QStringLiteral("…") + hex.right(8);
}
} // namespace

MultisigSpendDialog::MultisigSpendDialog(QWidget* parent, WalletModel* wallet_model, ClientModel* client_model, const MultisigUtil::Entry& entry)
    : QDialog(parent, GUIUtil::dialog_flags),
      m_wallet_model(wallet_model),
      m_client_model(client_model),
      m_entry(entry)
{
    setupUi(/*with_build_stage=*/true);
    refreshAvailableBalance();
}

MultisigSpendDialog::MultisigSpendDialog(QWidget* parent, WalletModel* wallet_model, ClientModel* client_model, const PartiallySignedTransaction& psbtx)
    : QDialog(parent, GUIUtil::dialog_flags),
      m_wallet_model(wallet_model),
      m_client_model(client_model)
{
    setupUi(/*with_build_stage=*/false);
    setPsbt(psbtx);
}

void MultisigSpendDialog::setupUi(bool with_build_stage)
{
    setWindowTitle(with_build_stage ? tr("Spend from Multisig") : tr("Multisig Transaction"));
    resize(920, with_build_stage ? 760 : 620);

    auto* root = new QVBoxLayout(this);

    if (with_build_stage) {
        m_build_group = new QGroupBox(tr("Transaction"), this);
        auto* form = new QFormLayout(m_build_group);

        auto* from_edit = new QLineEdit(m_entry.address, m_build_group);
        from_edit->setReadOnly(true);
        form->addRow(tr("From"), from_edit);

        m_balance_label = new QLabel(m_build_group);
        form->addRow(tr("Available"), m_balance_label);

        m_destination_edit = new QLineEdit(m_build_group);
        m_destination_edit->setPlaceholderText(tr("Destination address"));
        form->addRow(tr("Pay to"), m_destination_edit);

        auto* amount_row = new QHBoxLayout();
        m_amount_field = new BitcoinAmountField(m_build_group);
        if (m_wallet_model && m_wallet_model->getOptionsModel()) {
            m_amount_field->setDisplayUnit(m_wallet_model->getOptionsModel()->getDisplayUnit());
        }
        auto* use_available_button = new QPushButton(tr("Use available balance"), m_build_group);
        amount_row->addWidget(m_amount_field);
        amount_row->addWidget(use_available_button);
        amount_row->addStretch(1);
        form->addRow(tr("Amount"), amount_row);

        m_subtract_fee_checkbox = new QCheckBox(tr("Subtract fee from amount"), m_build_group);
        form->addRow(QString(), m_subtract_fee_checkbox);

        m_build_button = new QPushButton(tr("Build transaction"), m_build_group);
        form->addRow(QString(), m_build_button);

        root->addWidget(m_build_group);

        connect(use_available_button, &QPushButton::clicked, this, &MultisigSpendDialog::useAvailableBalance);
        connect(m_build_button, &QPushButton::clicked, this, &MultisigSpendDialog::buildTransaction);
    }

    m_manage_group = new QGroupBox(tr("Signatures"), this);
    auto* manage_layout = new QVBoxLayout(m_manage_group);

    m_summary_label = new QLabel(m_manage_group);
    m_summary_label->setWordWrap(true);
    m_summary_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    manage_layout->addWidget(m_summary_label);

    m_progress_label = new QLabel(m_manage_group);
    m_progress_label->setWordWrap(true);
    manage_layout->addWidget(m_progress_label);

    m_signers_table = new QTableWidget(m_manage_group);
    m_signers_table->setColumnCount(2);
    m_signers_table->setHorizontalHeaderLabels({tr("Public key"), tr("Signature")});
    m_signers_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_signers_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_signers_table->verticalHeader()->setVisible(false);
    m_signers_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_signers_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_signers_table->setMinimumHeight(140);
    manage_layout->addWidget(m_signers_table);

    auto* manage_buttons = new QHBoxLayout();
    m_sign_button = new QPushButton(tr("Sign with this wallet"), m_manage_group);
    m_copy_button = new QPushButton(tr("Copy PSBT"), m_manage_group);
    m_save_button = new QPushButton(tr("Save PSBT…"), m_manage_group);
    m_merge_button = new QPushButton(tr("Merge signatures"), m_manage_group);
    m_merge_button->setToolTip(tr("Combine a partially signed copy of this transaction received from a cosigner"));
    auto* merge_menu = new QMenu(m_merge_button);
    merge_menu->addAction(tr("From clipboard"), [this] { mergePsbt(/*from_clipboard=*/true); });
    merge_menu->addAction(tr("From file…"), [this] { mergePsbt(/*from_clipboard=*/false); });
    m_merge_button->setMenu(merge_menu);
    m_broadcast_button = new QPushButton(tr("Broadcast"), m_manage_group);
    manage_buttons->addWidget(m_sign_button);
    manage_buttons->addWidget(m_merge_button);
    manage_buttons->addWidget(m_copy_button);
    manage_buttons->addWidget(m_save_button);
    manage_buttons->addStretch(1);
    manage_buttons->addWidget(m_broadcast_button);
    manage_layout->addLayout(manage_buttons);

    root->addWidget(m_manage_group, 1);

    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setVisible(false);
    root->addWidget(m_status_label);

    auto* bottom = new QHBoxLayout();
    auto* close_button = new QPushButton(tr("Close"), this);
    bottom->addStretch(1);
    bottom->addWidget(close_button);
    root->addLayout(bottom);

    connect(m_sign_button, &QPushButton::clicked, this, &MultisigSpendDialog::signTransaction);
    connect(m_copy_button, &QPushButton::clicked, this, &MultisigSpendDialog::copyPsbt);
    connect(m_save_button, &QPushButton::clicked, this, &MultisigSpendDialog::savePsbt);
    connect(m_broadcast_button, &QPushButton::clicked, this, &MultisigSpendDialog::broadcastTransaction);
    connect(close_button, &QPushButton::clicked, this, &QDialog::close);

    m_manage_group->setEnabled(false);
    updateManageUi();
}

void MultisigSpendDialog::setStatus(const QString& message, bool is_error)
{
    m_status_label->setText(message);
    m_status_label->setStyleSheet(GUIUtil::getThemedStyleQString(is_error ? GUIUtil::ThemedStyle::TS_ERROR : GUIUtil::ThemedStyle::TS_PRIMARY));
    m_status_label->setVisible(true);
}

void MultisigSpendDialog::refreshAvailableBalance()
{
    if (!m_wallet_model || !m_entry.isValid()) return;

    m_available = 0;
    int utxo_count = 0;
    try {
        UniValue addresses(UniValue::VARR);
        addresses.push_back(m_entry.address.toStdString());
        UniValue params(UniValue::VARR);
        params.push_back(UniValue(0));
        params.push_back(UniValue(9999999));
        params.push_back(addresses);
        const UniValue result = m_wallet_model->node().executeRpc("listunspent", params, MultisigUtil::WalletRpcUri(*m_wallet_model));
        for (size_t i = 0; i < result.size(); ++i) {
            m_available += AmountFromValue(find_value(result[i], "amount"));
            ++utxo_count;
        }
    } catch (const std::exception& e) {
        setStatus(tr("Could not query UTXOs: %1").arg(QString::fromStdString(e.what())), true);
        return;
    } catch (const UniValue& obj_error) {
        setStatus(tr("Could not query UTXOs: %1").arg(QString::fromStdString(obj_error.write())), true);
        return;
    }

    const int display_unit = (m_wallet_model && m_wallet_model->getOptionsModel())
        ? m_wallet_model->getOptionsModel()->getDisplayUnit()
        : BitcoinUnits::DASH;
    m_balance_label->setText(tr("%1 in %2 UTXOs")
        .arg(BitcoinUnits::formatWithUnit(display_unit, m_available))
        .arg(utxo_count));
    if (m_build_button) m_build_button->setEnabled(m_available > 0);
    if (m_available == 0) setStatus(tr("This multisig address has no spendable UTXOs known to the wallet."), true);
}

void MultisigSpendDialog::useAvailableBalance()
{
    m_amount_field->setValue(m_available);
    m_subtract_fee_checkbox->setChecked(true);
}

void MultisigSpendDialog::buildTransaction()
{
    if (!m_wallet_model) return;

    const QString destination = m_destination_edit->text().trimmed();
    if (!m_wallet_model->validateAddress(destination)) {
        setStatus(tr("The destination address is not valid."), true);
        return;
    }

    bool amount_valid = false;
    const CAmount amount = m_amount_field->value(&amount_valid);
    if (!amount_valid || amount <= 0) {
        setStatus(tr("The amount to pay must be larger than 0."), true);
        return;
    }
    if (amount > m_available) {
        setStatus(tr("The amount exceeds the available balance."), true);
        return;
    }

    try {
        // Spend all UTXOs of the multisig address; change returns to the same
        // address, so cosigners only ever need to verify a single address.
        UniValue inputs(UniValue::VARR);
        {
            UniValue addresses(UniValue::VARR);
            addresses.push_back(m_entry.address.toStdString());
            UniValue params(UniValue::VARR);
            params.push_back(UniValue(0));
            params.push_back(UniValue(9999999));
            params.push_back(addresses);
            const UniValue unspent = m_wallet_model->node().executeRpc("listunspent", params, MultisigUtil::WalletRpcUri(*m_wallet_model));
            for (size_t i = 0; i < unspent.size(); ++i) {
                UniValue input(UniValue::VOBJ);
                input.pushKV("txid", find_value(unspent[i], "txid").get_str());
                input.pushKV("vout", find_value(unspent[i], "vout").get_int());
                inputs.push_back(input);
            }
        }
        if (inputs.empty()) {
            setStatus(tr("This multisig address has no spendable UTXOs known to the wallet."), true);
            return;
        }

        UniValue output(UniValue::VOBJ);
        output.pushKV(destination.toStdString(), ValueFromAmount(amount));
        UniValue outputs(UniValue::VARR);
        outputs.push_back(output);

        UniValue options(UniValue::VOBJ);
        options.pushKV("includeWatching", true);
        options.pushKV("changeAddress", m_entry.address.toStdString());
        if (m_subtract_fee_checkbox->isChecked()) {
            UniValue subtract(UniValue::VARR);
            subtract.push_back(UniValue(0));
            options.pushKV("subtractFeeFromOutputs", subtract);
        }

        UniValue params(UniValue::VARR);
        params.push_back(inputs);
        params.push_back(outputs);
        params.push_back(UniValue(0)); // locktime
        params.push_back(options);

        const UniValue result = m_wallet_model->node().executeRpc("walletcreatefundedpsbt", params, MultisigUtil::WalletRpcUri(*m_wallet_model));
        const std::string psbt_base64 = find_value(result, "psbt").get_str();

        PartiallySignedTransaction psbtx;
        std::string decode_error;
        if (!DecodeBase64PSBT(psbtx, psbt_base64, decode_error)) {
            setStatus(tr("Could not decode the created PSBT: %1").arg(QString::fromStdString(decode_error)), true);
            return;
        }

        m_build_group->setEnabled(false);
        setPsbt(psbtx);
        setStatus(tr("Transaction built. Sign it with this wallet, then pass the PSBT to your cosigners until enough signatures are collected."), false);
    } catch (const UniValue& obj_error) {
        QString message = QString::fromStdString(find_value(obj_error, "message").isStr() ? find_value(obj_error, "message").get_str() : obj_error.write());
        if (message.contains(QLatin1String("solving"), Qt::CaseInsensitive)) {
            message += QLatin1Char(' ') + tr("Hint: import the multisig setup (with redeem script) into this wallet so it can estimate the transaction size.");
        }
        setStatus(message, true);
    } catch (const std::exception& e) {
        setStatus(QString::fromStdString(e.what()), true);
    }
}

void MultisigSpendDialog::setPsbt(const PartiallySignedTransaction& psbtx)
{
    m_psbt = psbtx;

    // Let the wallet attach missing UTXO/script metadata so signature progress
    // and fee analysis have full information.
    if (m_wallet_model) {
        bool complete = false;
        m_wallet_model->wallet().fillPSBT(SIGHASH_ALL, false /* sign */, true /* bip32derivs */, nullptr, *m_psbt, complete);
    }

    m_manage_group->setEnabled(true);
    updateManageUi();
}

bool MultisigSpendDialog::transactionComplete() const
{
    if (!m_psbt) return false;
    PartiallySignedTransaction copy = *m_psbt;
    return FinalizePSBT(copy);
}

void MultisigSpendDialog::updateManageUi()
{
    if (!m_psbt) {
        m_summary_label->setText(tr("No transaction loaded yet."));
        m_progress_label->clear();
        m_signers_table->setRowCount(0);
        m_sign_button->setEnabled(false);
        m_copy_button->setEnabled(false);
        m_save_button->setEnabled(false);
        m_merge_button->setEnabled(false);
        m_broadcast_button->setEnabled(false);
        return;
    }

    const int display_unit = (m_wallet_model && m_wallet_model->getOptionsModel())
        ? m_wallet_model->getOptionsModel()->getDisplayUnit()
        : BitcoinUnits::DASH;

    // Outputs and fee summary
    QStringList summary_lines;
    for (const CTxOut& out : m_psbt->tx->vout) {
        CTxDestination dest;
        ExtractDestination(out.scriptPubKey, dest);
        QString line = tr("Sends %1 to %2")
            .arg(BitcoinUnits::formatWithUnit(display_unit, out.nValue))
            .arg(QString::fromStdString(EncodeDestination(dest)));
        if (m_entry.isValid() && QString::fromStdString(EncodeDestination(dest)) == m_entry.address) {
            line += QLatin1Char(' ') + tr("(change back to the multisig address)");
        }
        summary_lines << line;
    }
    const PSBTAnalysis analysis = AnalyzePSBT(*m_psbt);
    if (analysis.fee) {
        summary_lines << tr("Fee: %1").arg(BitcoinUnits::formatWithUnit(display_unit, *analysis.fee));
    }
    m_summary_label->setText(summary_lines.join(QLatin1Char('\n')));

    // Signature progress across inputs
    CScript fallback_redeem_script;
    if (m_entry.isValid() && !m_entry.redeem_script_hex.isEmpty() && IsHex(m_entry.redeem_script_hex.toStdString())) {
        const std::vector<unsigned char> script_data = ParseHex(m_entry.redeem_script_hex.toStdString());
        fallback_redeem_script = CScript(script_data.begin(), script_data.end());
    }

    int min_have = -1;
    int required = 0;
    int finalized_inputs = 0;
    int unknown_inputs = 0;
    const InputSigStatus* table_source = nullptr;
    std::vector<InputSigStatus> statuses;
    statuses.reserve(m_psbt->inputs.size());
    for (const PSBTInput& input : m_psbt->inputs) {
        statuses.push_back(StatusForInput(input, fallback_redeem_script));
    }
    for (const InputSigStatus& status : statuses) {
        if (!status.known) { ++unknown_inputs; continue; }
        if (status.finalized) { ++finalized_inputs; continue; }
        required = std::max(required, status.required);
        if (min_have < 0 || status.have < min_have) min_have = status.have;
        if (!table_source) table_source = &status;
    }

    const bool complete = transactionComplete();
    if (complete) {
        m_progress_label->setText(tr("All required signatures collected — the transaction is ready to broadcast."));
    } else if (table_source) {
        QString text = tr("Signatures: %1 of %2 required.").arg(min_have < 0 ? 0 : min_have).arg(required);
        if (finalized_inputs > 0) text += QLatin1Char(' ') + tr("(%1 of %2 inputs already finalized)").arg(finalized_inputs).arg(m_psbt->inputs.size());
        m_progress_label->setText(text);
    } else if (unknown_inputs > 0) {
        m_progress_label->setText(tr("Signature progress is unknown: the transaction inputs are missing multisig redeem script information."));
    } else {
        m_progress_label->setText(tr("Nothing left to sign."));
    }
    m_progress_label->setStyleSheet(GUIUtil::getThemedStyleQString(complete ? GUIUtil::ThemedStyle::TS_SUCCESS : GUIUtil::ThemedStyle::TS_PRIMARY));

    // Per-cosigner table (from the first pending multisig input; all inputs of
    // a single-address spend share the same key set)
    if (table_source) {
        m_signers_table->setRowCount(table_source->keys.size());
        for (size_t row = 0; row < table_source->keys.size(); ++row) {
            const auto& [pubkey, have_sig] = table_source->keys.at(row);
            m_signers_table->setItem(int(row), 0, new QTableWidgetItem(ShortKey(pubkey)));
            auto* status_item = new QTableWidgetItem(have_sig ? tr("Signed") : tr("Missing"));
            m_signers_table->setItem(int(row), 1, status_item);
        }
        m_signers_table->setVisible(true);
    } else {
        m_signers_table->setRowCount(0);
        m_signers_table->setVisible(!complete);
    }

    const bool can_sign = m_wallet_model && !m_wallet_model->wallet().privateKeysDisabled();
    m_sign_button->setEnabled(!complete && can_sign);
    m_copy_button->setEnabled(true);
    m_save_button->setEnabled(true);
    m_merge_button->setEnabled(!complete);
    m_broadcast_button->setEnabled(complete && m_client_model != nullptr);
}

void MultisigSpendDialog::signTransaction()
{
    if (!m_psbt || !m_wallet_model) return;

    WalletModel::UnlockContext ctx(m_wallet_model->requestUnlock());

    bool complete = false;
    size_t n_signed = 0;
    const TransactionError err = m_wallet_model->wallet().fillPSBT(SIGHASH_ALL, true /* sign */, true /* bip32derivs */, &n_signed, *m_psbt, complete);
    updateManageUi();

    if (err != TransactionError::OK) {
        setStatus(tr("Failed to sign transaction: %1").arg(QString::fromStdString(TransactionErrorString(err).translated)), true);
        return;
    }
    if (!complete && !ctx.isValid()) {
        setStatus(tr("Cannot sign inputs while the wallet is locked."), true);
    } else if (!complete && n_signed < 1) {
        setStatus(tr("This wallet holds none of the missing keys — pass the PSBT to a cosigner instead."), true);
    } else if (!complete) {
        setStatus(tr("Signed with this wallet's key. Copy or save the PSBT and pass it to the next cosigner."), false);
    } else {
        setStatus(tr("All required signatures collected. You can broadcast the transaction now."), false);
    }
}

void MultisigSpendDialog::copyPsbt()
{
    if (!m_psbt) return;
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << *m_psbt;
    GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
    setStatus(tr("PSBT copied to clipboard. Send it to your cosigner."), false);
}

void MultisigSpendDialog::savePsbt()
{
    if (!m_psbt) return;
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << *m_psbt;

    const QString suggested = (m_entry.isValid() && !m_entry.label.isEmpty() ? m_entry.label : tr("multisig")) + QLatin1String(".psbt");
    const QString filename = GUIUtil::getSaveFileName(this,
        tr("Save Transaction Data"), suggested,
        //: Expanded name of the binary PSBT file format. See: BIP 174.
        tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), nullptr);
    if (filename.isEmpty()) return;

    std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
    out << ssTx.str();
    out.close();
    setStatus(tr("PSBT saved to %1.").arg(filename), false);
}

void MultisigSpendDialog::mergePsbt(bool from_clipboard)
{
    if (!m_psbt) return;

    PartiallySignedTransaction incoming;
    std::string decode_error;
    if (from_clipboard) {
        const std::string raw = QApplication::clipboard()->text().toStdString();
        if (!DecodeBase64PSBT(incoming, raw, decode_error)) {
            setStatus(tr("Unable to decode PSBT from clipboard: %1").arg(QString::fromStdString(decode_error)), true);
            return;
        }
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
        const std::vector<unsigned char> data{std::istream_iterator<unsigned char>{in}, {}};
        if (!DecodeRawPSBT(incoming, MakeByteSpan(data), decode_error)) {
            setStatus(tr("Unable to decode PSBT: %1").arg(QString::fromStdString(decode_error)), true);
            return;
        }
    }

    PartiallySignedTransaction merged;
    const TransactionError err = CombinePSBTs(merged, {*m_psbt, incoming});
    if (err != TransactionError::OK) {
        setStatus(tr("Could not merge: %1 Make sure the cosigner signed the same transaction.").arg(QString::fromStdString(TransactionErrorString(err).translated)), true);
        return;
    }

    setPsbt(merged);
    setStatus(tr("Signatures merged."), false);
}

void MultisigSpendDialog::broadcastTransaction()
{
    if (!m_psbt || !m_client_model) return;

    PartiallySignedTransaction copy = *m_psbt;
    CMutableTransaction mtx;
    if (!FinalizeAndExtractPSBT(copy, mtx)) {
        setStatus(tr("The transaction could not be finalized — signatures are missing or invalid."), true);
        return;
    }

    const CTransactionRef tx = MakeTransactionRef(mtx);
    bilingual_str err_string;
    const TransactionError error = m_client_model->node().broadcastTransaction(tx, DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK(), err_string);
    if (error == TransactionError::OK) {
        m_broadcast_button->setEnabled(false);
        m_sign_button->setEnabled(false);
        m_merge_button->setEnabled(false);
        setStatus(tr("Transaction broadcast successfully! Transaction ID: %1").arg(QString::fromStdString(tx->GetHash().GetHex())), false);
    } else {
        setStatus(tr("Transaction broadcast failed: %1").arg(QString::fromStdString(TransactionErrorString(error).translated)), true);
    }
}
