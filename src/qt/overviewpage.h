// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include <interfaces/wallet.h>

#include <QWidget>
#include <QSizePolicy>
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
class QSpacerItem;
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
    bool eventFilter(QObject* watched, QEvent* event) override;
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
    //! Keep form constraints separately from polished CSS floors. On leaving
    //! Abyss the current stylesheet must restore its own minimums, including
    //! for pages constructed while hidden or before any stylesheet was loaded.
    std::map<QWidget*, int> m_form_minimum_widths;
    std::map<QWidget*, int> m_inherited_minimum_widths;
    bool m_classic_widths_pending{false};
    //! The form's own stretch weights on the row of cards, kept for the same
    //! reason: only the modern theme redistributes them.
    std::map<int, int> m_inherited_stretch;
    std::map<QSpacerItem*, std::pair<QSize, QSizePolicy>> m_inherited_spacers;
    std::map<int, int> m_inherited_grid_stretch;
    QSizePolicy m_total_size_policy;
    bool m_balance_update_pending{false};
    bool m_applying_balance_widths{false};
    bool m_modern_presentation{false};
    void scheduleBalanceWidths();
    void fitTotalFont();

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
