// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MULTISIGPAGE_H
#define BITCOIN_QT_MULTISIGPAGE_H

#include <consensus/amount.h>
#include <qt/multisigutil.h>

#include <QHash>
#include <QWidget>

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QTableWidget;
QT_END_NAMESPACE

/**
 * Wallet tab dedicated to multisig addresses: lists the tracked multisig
 * setups with their balances and gives access to the whole lifecycle —
 * creating a setup, exchanging it with cosigners, spending via PSBT and
 * continuing partially signed transactions.
 */
class MultisigPage : public QWidget
{
    Q_OBJECT

public:
    explicit MultisigPage(QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);
    void setWalletModel(WalletModel* wallet_model);

protected:
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void refresh();
    void createMultisig();
    void importSetup();
    void exportSetup();
    void spendSelected();
    void copyAddress();
    void removeSelected();
    void updateButtonStates();

private:
    ClientModel* m_client_model{nullptr};
    WalletModel* m_wallet_model{nullptr};

    QTableWidget* m_table;
    QLabel* m_empty_label;
    QLabel* m_status_label;
    QPushButton* m_create_button;
    QPushButton* m_import_button;
    QPushButton* m_refresh_button;
    QPushButton* m_spend_button;
    QPushButton* m_continue_button;
    QPushButton* m_export_button;
    QPushButton* m_copy_address_button;
    QPushButton* m_remove_button;

    QList<MultisigUtil::Entry> m_entries;

    MultisigUtil::Entry selectedEntry() const;
    void continuePsbt(bool from_clipboard);
    void setStatus(const QString& message, bool is_error);
    //! Query per-address balances/UTXO counts for all tracked addresses via a single listunspent call.
    bool queryBalances(QHash<QString, CAmount>& balances, QHash<QString, int>& utxo_counts, QString& error) const;
};

#endif // BITCOIN_QT_MULTISIGPAGE_H
