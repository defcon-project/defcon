// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_OVERVIEWPAGE_H
#define BITCOIN_QT_OVERVIEWPAGE_H

#include <interfaces/wallet.h>

#include <QDateTime>
#include <QWidget>
#include <QSizePolicy>
#include <map>
#include <memory>
#include <optional>
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
class QProgressBar;
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

    //! What the Abyss network bar shows. updateNetworkState() gathers it from
    //! the client model; setNetworkStatus() only draws it, so a test can hand
    //! it any state the node would take a long while to reach.
    struct NetworkStatus {
        int height{0};
        qint64 block_age{0};            //!< seconds since the tip was made
        bool chain_synced{false};
        bool data_synced{false};        //!< the masternode data, after the chain
        double progress{0};             //!< of whichever of the two still runs
        bool chain_locked{false};       //!< the tip itself carries a ChainLock
        int peers{0};
        std::optional<int> masternodes; //!< valid ones; unknown until the chain is synced
        std::optional<long> mempool;    //!< transactions; unknown until first polled
    };
    void setNetworkStatus(const NetworkStatus& status);

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
    //! Replacing this context disconnects only the Abyss bar's old subscriptions,
    //! including queued deliveries, without affecting other model listeners.
    std::unique_ptr<QObject> m_network_model_context;
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
    //! One entry of the network bar: a glyph, then its words. `value`, when
    //! the entry has one, is the bold number, before or after `text` as the
    //! entry reads.
    struct NetworkItem {
        QWidget* widget{nullptr};
        QLabel* icon{nullptr};
        QLabel* value{nullptr};
        QLabel* text{nullptr};
    };
    NetworkItem m_net_block, m_net_age, m_net_synced, m_net_chainlock, m_net_syncing,
        m_net_peers, m_net_masternodes, m_net_mempool;
    QProgressBar* m_net_progress{nullptr};
    int m_block_height{0};
    QDateTime m_block_time;
    double m_verification_progress{0};
    double m_data_progress{0};
    int m_chainlock_height{-1};
    int m_peer_count{0};
    std::optional<int> m_masternode_count;
    std::optional<long> m_mempool_count;
    //! Draw the bar's glyphs in the active theme's colors.
    void paintNetworkIcons();
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
    int m_total_minimum_height{0};
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
