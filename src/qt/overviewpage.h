// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include <interfaces/wallet.h>

#include <QWidget>
#include <map>
#include <memory>
#include <vector>

class ClientModel;
class TransactionFilterProxy;
class TxViewDelegate;
class WalletModel;

namespace Ui {
    class OverviewPage;
}

QT_BEGIN_NAMESPACE
class QEvent;
class QFrame;
class QLabel;
class QModelIndex;
class QPaintEvent;
QT_END_NAMESPACE

/** Overview ("home") page widget */
class OverviewPage : public QWidget
{
    Q_OBJECT

public:
    explicit OverviewPage(QWidget* parent = nullptr);
    ~OverviewPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void showOutOfSyncWarning(bool fShow);

public Q_SLOTS:
    void coinJoinStatus(bool fForce = false);
    void setBalance(const interfaces::WalletBalances& balances);
    void setPrivacy(bool privacy);

Q_SIGNALS:
    void transactionClicked(const QModelIndex &index);
    void outOfSyncWarningClicked();

protected:
    void changeEvent(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
private:
    QTimer *timer;
    Ui::OverviewPage *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;
    interfaces::WalletBalances m_balances;
    bool m_privacy{false};
    int nDisplayUnit;
    bool fShowAdvancedCJUI;
    int cachedNumISLocks;

    TxViewDelegate *txdelegate;
    std::unique_ptr<TransactionFilterProxy> filter;
    QWidget* modernHeader{nullptr};
    QFrame* networkCard{nullptr};
    QLabel* labelNetworkStatus{nullptr};
    //! Minimum widths as the form and the shared stylesheet set them, kept so
    //! the themes that were laid out for those numbers get them back.
    std::map<QWidget*, int> m_inherited_minimum_widths;

    void SetupTransactionList(int nNumItems);
    void DisableCoinJoinCompletely();
    void updateThemePresentation();
    void updateNetworkState();
    //! The amount as the active theme wants it drawn.
    QString formatBalance(int unit, const CAmount& amount) const;
    //! Every label on the balances card that carries an amount.
    std::vector<QLabel*> balanceLabels() const;
    //! Let the amounts decide how narrow the balances card may become.
    void applyBalanceWidths();

private Q_SLOTS:
    void toggleCoinJoin();
    void updateDisplayUnit();
    void updateCoinJoinProgress();
    void updateAdvancedCJUI(bool fShowAdvancedCJUI);
    void handleTransactionClicked(const QModelIndex &index);
    void updateAlerts(const QString &warnings);
    void updateWatchOnlyLabels(bool showWatchOnly);
};

#endif // BITCOIN_QT_OVERVIEWPAGE_H
