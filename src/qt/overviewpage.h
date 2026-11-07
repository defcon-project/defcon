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
class EmptyStateWidget;
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
class QPushButton;
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
    //! The sky level this page asks for: its spendable total's level, or
    //! 0 in discreet mode or without a wallet.
    int skyTier() const;

    //! What the Abyss staking card shows. The window fills the node's part
    //! from the staking thread, the wallet view adds its wallet's part, and
    //! setStakingStatus() only draws it, so a test can hand it any state.
    struct StakingStatus {
        bool configured{true};   //!< the node was started with staking (-staking)
        bool minter{true};       //!< the staking thread is inside its loop right now
        bool syncing{false};     //!< the thread's own gates: the chain, the masternode data, the peers
        bool enabled{false};     //!< this wallet's own staking switch
        int wallet_state{0};     //!< the thread's last word on this wallet (CWallet::m_is_staking)
        bool searching{true};    //!< the thread's own flag (fIsStaking): its last pass reached the search, not a gate or a failed block template
        bool locked{false};      //!< encrypted and fully locked: the thread cannot sign
        bool keyless{false};     //!< holds no private keys, so it can never stake
    };
    //! The node's side of the staking thread's gates, as the window reads them.
    struct StakingGates {
        bool mainnet{false};
        bool masternodes_synced{false};
        bool initial_download{false};
        size_t peers{0};
        int height{0};
        int last_pow_block{0};
        int64_t tip_time{0};      //!< the active tip's block time
        int64_t adjusted_time{0}; //!< the node's network-adjusted clock
    };
    //! Whether the thread waits on one of them (pos/minter.cpp): below the last
    //! proof-of-work block, the masternode data not in, a tip stamped later
    //! than the node's clock, and on the main network only, fewer peers than
    //! it needs or the initial download.
    static bool stakingWaitsForSync(const StakingGates& gates);
    //! The one word the card says, in the order the checks are made.
    enum class StakingState { Unavailable, Disabled, Locked, Off, Syncing, Ready, Staking };
    void setStakingStatus(const StakingStatus& status);
    const StakingStatus& stakingStatus() const { return m_staking; }
    StakingState stakingState() const;

public Q_SLOTS:
    void coinJoinStatus(bool fForce = false);
    void setBalance(const interfaces::WalletBalances& balances);
    void setPrivacy(bool privacy);

Q_SIGNALS:
    void transactionClicked(const QModelIndex &index);
    void outOfSyncWarningClicked();
    //! The empty transaction list's Receive button was clicked.
    void receiveClicked();
    //! The level in skyTier() changed; the window hands it to the sky.
    void skyTierChanged(int tier);
    //! The staking card's button: put this wallet's staking switch into the
    //! position the button offered, whatever the switch is by now.
    void stakingToggleRequested(bool enable);
    //! The card's or the banner's button on a locked wallet: unlock it for
    //! staking, and turn staking on if it is off.
    void stakingUnlockRequested();

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
    //! coinJoinStatus() state, per page: as function-local statics it was
    //! shared by every page, so a second one (another wallet, a recreated
    //! window) skipped its first update and kept the advanced CoinJoin widgets
    //! as the form left them.
    bool m_cj_widgets_initial{true};
    bool m_cj_widgets_last_visible{false};
    bool m_cj_widgets_last_show_advanced{false};
    int64_t m_cj_last_progress_time{0};
    int cachedNumISLocks;

    TxViewDelegate *txdelegate;
    std::unique_ptr<TransactionFilterProxy> filter;
    //! Abyss's message over the recent transactions while there are none.
    EmptyStateWidget* m_transactions_empty{nullptr};
    bool m_out_of_sync{true};
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
    //! The sky's level as a pill, outside the bar's layout: see placeSkyBadge().
    NetworkItem m_net_sky;
    int m_sky_tier{0};        //!< The balance's own level, with hysteresis.
    int m_sky_tier_shown{-1}; //!< What skyTierChanged() last announced.
    bool m_sky_badge_wanted{false}; //!< A level above 0, in Abyss, out of discreet mode.
    void updateSkyBadge();
    //! Put the badge in the free middle of the bar, or hide it if it does not fit there.
    void placeSkyBadge();
    //! Under the total: the balance the next sky level starts at, or that this is the top.
    QLabel* m_next_level{nullptr};
    void updateNextLevel();
    //! The staking card: a word for the state, a line about it, one button.
    QFrame* m_staking_card{nullptr};
    QLabel* m_staking_icon{nullptr};
    QLabel* m_staking_state{nullptr};
    QLabel* m_staking_detail{nullptr};
    QPushButton* m_staking_button{nullptr};
    //! Over the network bar while the wallet is locked: staking is paused, with the way out.
    QFrame* m_staking_banner{nullptr};
    QPushButton* m_staking_banner_button{nullptr};
    StakingStatus m_staking;
    //! The dot last drawn (its RGBA and pixel ratio): a tick that changes
    //! neither paints nothing.
    unsigned int m_staking_dot_rgba{0};
    qreal m_staking_dot_dpr{0};
    void updateStakingCard();
    //! The CoinJoin card is shown in Abyss only while it has something to say
    //! (mixing, or mixed coins), or once this row's link has opened it.
    QWidget* m_coinjoin_row{nullptr};
    //! Until coinJoinStatus() has looked, the card's visibility is not this
    //! page's to change: the classic themes leave it as the form and the
    //! status leave it.
    bool m_coinjoin_known{false};
    bool m_coinjoin_enabled{false};
    //! The card as the classic themes have it: hidden from the start, then
    //! shown whenever coinJoinStatus() finds CoinJoin on and hidden when off;
    //! disabled leaves it as the last of these left it. Kept apart from what
    //! Abyss shows, so a trip through Abyss does not change it.
    bool m_coinjoin_classic_card{false};
    //! coinJoinStatus() disabled it (a masternode, or automatic backups
    //! off or failed): the classic themes leave the card as it stands and it
    //! explains itself; Abyss keeps it behind the row's link.
    bool m_coinjoin_disabled{false};
    bool m_coinjoin_mixing{false};
    bool m_coinjoin_card_pinned{false};
    void updateCoinJoinCard();
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
    //! The masternode and mempool entries in words where the bar has the room,
    //! short where it has not. The page's minimum width stays that of the short forms.
    void fitNetworkWords();
    std::optional<int> m_shown_masternodes;
    int m_coinjoin_form_spacing{0};
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
    void updateSyncStatusText();
    void updateEmptyState();
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
