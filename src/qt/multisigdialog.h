// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MULTISIGDIALOG_H
#define BITCOIN_QT_MULTISIGDIALOG_H

#include <qt/multisigutil.h>

#include <QDialog>

class ClientModel;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class WalletModel;

class MultisigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MultisigDialog(QWidget* parent, WalletModel* wallet_model, ClientModel* client_model);

private Q_SLOTS:
    void updateModeUi();
    void validateInputs();
    void executeCommand();
    void exportSetup();

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
    QLabel* m_validation_label;
    QLineEdit* m_label_edit;
    QCheckBox* m_track_checkbox;
    QPushButton* m_execute_button;

    QLineEdit* m_address_out;
    QLineEdit* m_descriptor_out;
    QLineEdit* m_redeem_out;
    QPlainTextEdit* m_raw_out;
    QPushButton* m_export_setup_button;
    QLabel* m_status_label;

    //! Result of the last successful command; drives "Export setup".
    MultisigUtil::Entry m_last_entry;

    void clearResult();
    void setStatus(const QString& message, bool is_error);
    Mode currentMode() const;
    //! Parsed key tokens from the keys edit; empty result and non-empty error on validation failure.
    QStringList validatedKeys(QString& error) const;
    //! Resolve an address token to the hex pubkey known by the wallet (getaddressinfo); returns the token itself on failure.
    QString resolveToPubkey(const QString& token) const;
};

#endif // BITCOIN_QT_MULTISIGDIALOG_H
