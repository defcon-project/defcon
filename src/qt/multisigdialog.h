// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MULTISIGDIALOG_H
#define BITCOIN_QT_MULTISIGDIALOG_H

#include <QDialog>

class ClientModel;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class WalletModel;

class MultisigDialog : public QDialog
{
public:
    explicit MultisigDialog(QWidget* parent, WalletModel* wallet_model, ClientModel* client_model);

private:
    enum class Mode {
        CREATE_ONLY = 0,
        ADD_TO_WALLET = 1,
    };

    WalletModel* m_wallet_model;
    ClientModel* m_client_model;

    QComboBox* m_mode_combo;
    QSpinBox* m_required_spin;
    QPlainTextEdit* m_keys_edit;
    QLineEdit* m_label_edit;
    QPushButton* m_execute_button;

    QLineEdit* m_address_out;
    QLineEdit* m_descriptor_out;
    QPlainTextEdit* m_redeem_out;
    QPlainTextEdit* m_raw_out;
    QLabel* m_status_label;

    void updateModeUi();
    void clearResult();
    void setStatus(const QString& message, bool is_error);
    void executeCommand();
    Mode currentMode() const;
};

#endif // BITCOIN_QT_MULTISIGDIALOG_H
