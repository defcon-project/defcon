// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MULTISIGSPENDDIALOG_H
#define BITCOIN_QT_MULTISIGSPENDDIALOG_H

#include <consensus/amount.h>
#include <psbt.h>
#include <qt/multisigutil.h>

#include <QDialog>

#include <optional>

class BitcoinAmountField;
class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QCheckBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QVBoxLayout;
QT_END_NAMESPACE

/**
 * Dialog driving a spend from a multisig address via PSBT: builds a funded
 * transaction from the address' UTXOs, shows m-of-n signature progress per
 * cosigner, signs with this wallet's keys, merges partially signed
 * transactions received from cosigners, and broadcasts once complete.
 */
class MultisigSpendDialog : public QDialog
{
    Q_OBJECT

public:
    //! Spend flow: build a new transaction from the entry's UTXOs.
    MultisigSpendDialog(QWidget* parent, WalletModel* wallet_model, ClientModel* client_model, const MultisigUtil::Entry& entry);
    //! Continue flow: manage an existing partially signed transaction.
    MultisigSpendDialog(QWidget* parent, WalletModel* wallet_model, ClientModel* client_model, const PartiallySignedTransaction& psbtx);

private Q_SLOTS:
    void useAvailableBalance();
    void buildTransaction();
    void signTransaction();
    void copyPsbt();
    void savePsbt();
    void showPsbtQr();
    void mergePsbt(bool from_clipboard);
    void broadcastTransaction();
    void copyTxid();

private:
    WalletModel* m_wallet_model;
    ClientModel* m_client_model;
    MultisigUtil::Entry m_entry; //!< invalid in continue flow
    std::optional<PartiallySignedTransaction> m_psbt;
    CAmount m_available{0};

    QLabel* m_step_labels[3]{nullptr, nullptr, nullptr};

    QGroupBox* m_build_group{nullptr};
    QLabel* m_balance_label{nullptr};
    QLineEdit* m_destination_edit{nullptr};
    BitcoinAmountField* m_amount_field{nullptr};
    QCheckBox* m_subtract_fee_checkbox{nullptr};
    QPushButton* m_build_button{nullptr};
    QVBoxLayout* m_validation_layout{nullptr};

    QGroupBox* m_preview_group;
    QLabel* m_preview_label;

    QGroupBox* m_manage_group;
    QLabel* m_progress_label;
    QTableWidget* m_signers_table;
    QPushButton* m_sign_button;
    QPushButton* m_copy_button;
    QPushButton* m_save_button;
    QPushButton* m_qr_button;
    QPushButton* m_merge_button;

    QGroupBox* m_finalize_group;
    QPushButton* m_broadcast_button;
    QLineEdit* m_txid_out;
    QPushButton* m_copy_txid_button;

    QLabel* m_status_label;

    void setupUi(bool with_build_stage);
    void setPsbt(const PartiallySignedTransaction& psbtx);
    void updateManageUi();
    void refreshAvailableBalance();
    void populateValidationPanel();
    //! Highlight the current step (1-based) of Create -> Collect -> Finalize.
    void updateStepIndicator(int current_step);
    void setStatus(const QString& message, bool is_error);
    bool transactionComplete() const;
};

#endif // BITCOIN_QT_MULTISIGSPENDDIALOG_H
