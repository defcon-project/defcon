// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/overviewpage.h>
#include <qt/forms/ui_overviewpage.h>

#include <qt/abyssunits.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/emptystatewidget.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/starfieldwidget.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionoverviewwidget.h>
#include <qt/transactiontablemodel.h>
#include <qt/utilitydialog.h>
#include <qt/walletmodel.h>

#include <coinjoin/options.h>
#include <evo/deterministicmns.h>
#include <interfaces/coinjoin.h>
#include <interfaces/node.h>
#include <pos/minter.h>
#include <wallet/wallet.h>

#include <QFrame>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>

#include <algorithm>
#include <array>
#include <map>
#include <cmath>

#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QHelpEvent>
#include <QIcon>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QRadialGradient>
#include <QSettings>
#include <QStatusTipEvent>
#include <QStyle>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#define ITEM_HEIGHT 54
#define NUM_ITEMS_DISABLED 5
#define NUM_ITEMS_ENABLED_NORMAL 6
#define NUM_ITEMS_ENABLED_ADVANCED 8

//! Abyss: the gap between the two columns, across while they stand side by
//! side, downwards while one stands under the other.
static constexpr int ABYSS_COLUMN_GAP{12};
//! The network bar: its padding, the room between its entries, and between
//! its lines where a narrow page breaks it.
static constexpr QMargins NETWORK_MARGINS{16, 12, 16, 12};
static constexpr int NETWORK_SPACING{18};
static constexpr int NETWORK_LINE_SPACING{8};

Q_DECLARE_METATYPE(interfaces::WalletBalances)

class TxViewDelegate : public QAbstractItemDelegate
{
    Q_OBJECT
public:
    explicit TxViewDelegate(QObject* parent = nullptr) :
        QAbstractItemDelegate(), unit(BitcoinUnits::DASH)
    {
        connect(this, &TxViewDelegate::width_changed, this, &TxViewDelegate::sizeHintChanged);
    }

    inline void paint(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index ) const override
    {
        painter->save();

        QRect mainRect = option.rect;
        int xspace = 8;
        int ypad = 8;
        int halfheight = (mainRect.height() - 2*ypad)/2;
        QRect rectTopHalf(mainRect.left() + xspace, mainRect.top() + ypad, mainRect.width() - xspace, halfheight);
        QRect rectBottomHalf(mainRect.left() + xspace, mainRect.top() + ypad + halfheight + 5, mainRect.width() - xspace, halfheight);
        QRect rectBounding;
        QColor colorForeground;
        qreal initialFontSize = painter->font().pointSizeF();

        // Grab model indexes for desired data from TransactionTableModel
        QModelIndex indexDate = index.sibling(index.row(), TransactionTableModel::Date);
        QModelIndex indexAmount = index.sibling(index.row(), TransactionTableModel::Amount);
        QModelIndex indexAddress = index.sibling(index.row(), TransactionTableModel::ToAddress);

        // Draw first line (with slightly bigger font than the second line will get)
        // Content: Date/Time, Optional IS indicator, Amount
        //
        // The two casts here and below are deliberate and preserve existing
        // behaviour. initialFontSize is a qreal, and while getScaledFontSize
        // took an int these arguments were truncated on the way in; widening
        // that parameter to double would silently change what this row renders.
        // Whether these call sites should pass their exact value is the same
        // question F-2026-030 answers for the font caches, but it is a separate,
        // unclassified UI change and is deliberately not made here.
        painter->setFont(GUIUtil::getFont(GUIUtil::FontWeight::Normal, false, GUIUtil::getScaledFontSize(static_cast<int>(initialFontSize * 1.17))));
        // Date/Time
        colorForeground = qvariant_cast<QColor>(indexDate.data(Qt::ForegroundRole));
        QString strDate = indexDate.data(Qt::DisplayRole).toString();
        painter->setPen(colorForeground);
        painter->drawText(rectTopHalf, Qt::AlignLeft | Qt::AlignVCenter, strDate, &rectBounding);
        // Optional IS indicator
        QIcon iconInstantSend = qvariant_cast<QIcon>(indexAddress.data(TransactionTableModel::RawDecorationRole));
        QRect rectInstantSend(rectBounding.right() + 5, rectTopHalf.top(), 16, halfheight);
        iconInstantSend.paint(painter, rectInstantSend);
        // Amount
        colorForeground = qvariant_cast<QColor>(indexAmount.data(Qt::ForegroundRole));
        // Note: do NOT use Qt::DisplayRole, have format properly here
        qint64 nAmount = index.data(TransactionTableModel::AmountRole).toLongLong();
        // Abyss: without trailing zeros (U3-C); the exact amount is in the row's tooltip.
        QString strAmount = GUIUtil::isModernTheme()
            ? AbyssUnits::format(unit, nAmount, true, true)
            : BitcoinUnits::floorWithUnit(unit, nAmount, true, BitcoinUnits::SeparatorStyle::ALWAYS);
        painter->setPen(colorForeground);
        QRect amount_bounding_rect;
        painter->drawText(rectTopHalf, Qt::AlignRight | Qt::AlignVCenter, strAmount, &amount_bounding_rect);

        // Draw second line (with the initial font)
        // Content: Address/label, Optional Watchonly indicator
        painter->setFont(GUIUtil::getFont(GUIUtil::FontWeight::Normal, false, GUIUtil::getScaledFontSize(static_cast<int>(initialFontSize))));
        // Address/Label
        colorForeground = qvariant_cast<QColor>(indexAddress.data(Qt::ForegroundRole));
        QString address = indexAddress.data(Qt::DisplayRole).toString();

        // Optional Watchonly indicator
        QRect addressRect{rectBottomHalf};
        if (index.data(TransactionTableModel::WatchonlyRole).toBool())
        {
            QIcon iconWatchonly = qvariant_cast<QIcon>(index.data(TransactionTableModel::WatchonlyDecorationRole));
            QRect watchonlyRect(rectBottomHalf.left(), rectBottomHalf.top(), rectBottomHalf.height(), halfheight);
            iconWatchonly.paint(painter, watchonlyRect);
            addressRect.setLeft(addressRect.left() + watchonlyRect.width() + 5);
        }

        painter->setPen(colorForeground);
        painter->drawText(addressRect, Qt::AlignLeft | Qt::AlignVCenter, address, &rectBounding);
        int address_rect_min_width = rectBounding.width();

        const int minimum_width = std::max(address_rect_min_width, amount_bounding_rect.width() /*+ date_bounding_rect.width() */);
        const auto search = m_minimum_width.find(index.row());
        if (search == m_minimum_width.end() || search->second != minimum_width) {
            m_minimum_width[index.row()] = minimum_width;
            Q_EMIT width_changed(index);
        }
        painter->restore();
    }

    inline QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto search = m_minimum_width.find(index.row());
        const int minimum_text_width = search == m_minimum_width.end() ? 0 : search->second;
        return {ITEM_HEIGHT + 8 + minimum_text_width, ITEM_HEIGHT};
    }

    //! The row's tooltip is the model's (status, type, address) and has no
    //! amount, while Abyss may show the amount cut to the "digits" setting:
    //! there the tooltip gets one more line, the exact amount (U3-C). The list
    //! is hidden in discreet mode, so no help event reaches it then.
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override
    {
        if (event == nullptr || view == nullptr || event->type() != QEvent::ToolTip || !index.isValid() ||
            !GUIUtil::isModernTheme()) {
            return QAbstractItemDelegate::helpEvent(event, view, option, index);
        }
        const qint64 amount = index.data(TransactionTableModel::AmountRole).toLongLong();
        const QString line = tr("Amount: %1").arg(AbyssUnits::exact(unit, amount, true));
        const QString own = index.data(Qt::ToolTipRole).toString();
        QToolTip::showText(event->globalPos(), own.isEmpty() ? line : own + QLatin1Char('\n') + line, view);
        return true;
    }

    int unit;

Q_SIGNALS:
    //! An intermediate signal for emitting from the `paint() const` member function.
    void width_changed(const QModelIndex& index) const;

private:
    mutable std::map<int, int> m_minimum_width;
};

#include <qt/overviewpage.moc>

OverviewPage::OverviewPage(QWidget* parent) :
    QWidget(parent),
    timer(nullptr),
    ui(new Ui::OverviewPage),
    clientModel(nullptr),
    walletModel(nullptr),
    cachedNumISLocks(-1),
    txdelegate(new TxViewDelegate(this))
{
    ui->setupUi(this);

    // The balance labels' own explanations (the form's tooltips): Abyss puts
    // the exact amount above them, a classic theme shows them as they are.
    for (QLabel* label : balanceLabels()) m_form_tooltips[label] = label->toolTip();
    m_form_tooltips[ui->labelAnonymized] = ui->labelAnonymized->toolTip();

    // Under the total, in Abyss: where the next sky level starts. It is part
    // of the balances card and so of the maps below; it never widens the
    // amounts' column (applyBalanceWidths leaves it out, and its own policy
    // lets the column decide).
    m_next_level = new QLabel(ui->frame);
    m_next_level->setObjectName("nextLevelLabel");
    m_next_level->setAlignment(Qt::AlignRight | Qt::AlignTrailing | Qt::AlignVCenter);
    m_next_level->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_next_level->hide();
    // Across the label and the amount columns, ending where the amounts end.
    ui->gridLayout->addWidget(m_next_level, 6, 0, 1, 2);

    // These are the .ui constraints, before CSS polish or measured Abyss widths.
    // The page itself has a 960px minimum in the form; restoring everything to
    // zero would lose that constraint even if the stylesheet restores the card.
    for (QLabel* label : ui->frame->findChildren<QLabel*>()) {
        m_form_minimum_widths.emplace(label, label->minimumWidth());
    }
    m_form_minimum_widths.emplace(ui->frame, ui->frame->minimumWidth());
    m_form_minimum_widths.emplace(this, minimumWidth());
    if (!GUIUtil::isModernTheme()) {
        // Keep the original form's screen bound without repolishing a fresh
        // classic page: unpolish resets the fonts registered by GUIUtil.
        if (const QScreen* display = screen()) {
            setMinimumWidth(std::min(minimumWidth(), display->availableGeometry().width()));
        }
    }

    modernHeader = new QWidget(this);
    modernHeader->setObjectName("modernOverviewHeader");
    auto* modernHeaderLayout = new QVBoxLayout(modernHeader);
    // With the page's 11px margin: the 16px every page's header stands off the corner.
    modernHeaderLayout->setContentsMargins(5, 5, 4, 10);
    modernHeaderLayout->setSpacing(4);
    auto* modernTitle = new QLabel(tr("Overview"), modernHeader);
    modernTitle->setObjectName("modernOverviewTitle");
    auto* modernSubtitle = new QLabel(tr("Wallet overview and recent activity."), modernHeader);
    modernSubtitle->setObjectName("modernOverviewSubtitle");
    modernHeaderLayout->addWidget(modernTitle);
    modernHeaderLayout->addWidget(modernSubtitle);
    ui->topLayout->insertWidget(0, modernHeader);

    // The network's pulse belongs on the front page, not only in the status
    // bar: one line above the cards, fed from the client model. The tip and
    // its age, then how far along the node is, then the company it keeps.
    networkCard = new QFrame(this);
    networkCard->setObjectName("networkCard");
    std::vector<QWidget*> network_words;
    std::vector<QWidget*> network_values;
    enum class Value { None, Before, After };
    // Each state is its own entry with its own colors in the stylesheet, and
    // only visibility changes: repolishing a label would drop the font GUIUtil
    // gave it.
    const auto entry = [&](NetworkItem& item, const char* name, const QString& text, Value value) {
        item.widget = new QWidget(networkCard);
        item.widget->setObjectName(name);
        auto* layout = new QHBoxLayout(item.widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        item.icon = new QLabel(item.widget);
        item.icon->setObjectName("networkIcon");
        item.icon->setFixedSize(18, 18);
        layout->addWidget(item.icon);
        item.text = new QLabel(text, item.widget);
        item.text->setObjectName("networkText");
        network_words.push_back(item.text);
        if (value != Value::None) {
            item.value = new QLabel(item.widget);
            item.value->setObjectName("networkValue");
            network_values.push_back(item.value);
        }
        layout->addWidget(value == Value::Before ? item.value : item.text);
        if (value != Value::None) layout->addWidget(value == Value::Before ? item.text : item.value);
    };
    entry(m_net_block, "networkBlock", tr("Block"), Value::After);
    entry(m_net_age, "networkAge", QString(), Value::None);
    m_net_separator = new QFrame(networkCard);
    m_net_separator->setObjectName("networkSeparator");
    m_net_separator->setFixedSize(1, 20);
    entry(m_net_synced, "networkSynced", tr("Synced"), Value::None);
    entry(m_net_chainlock, "networkChainLocked", tr("ChainLocked"), Value::None);
    entry(m_net_syncing, "networkSyncing", QString(), Value::None);
    m_net_progress = new QProgressBar(networkCard);
    m_net_progress->setObjectName("networkSyncProgress");
    m_net_progress->setRange(0, 1000);
    m_net_progress->setTextVisible(false);
    m_net_progress->setMinimumWidth(60);
    m_net_progress->setMaximumWidth(165);
    m_net_progress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    entry(m_net_peers, "networkPeers", QString(), Value::Before);
    entry(m_net_masternodes, "networkMasternodes", tr("MN"), Value::Before);
    entry(m_net_mempool, "networkMempool", tr("tx"), Value::Before);
    buildNetworkLayout();
    m_net_block.widget->setToolTip(tr("The newest block this wallet knows of"));
    m_net_age.widget->setToolTip(tr("How long ago that block was made"));
    m_net_synced.widget->setToolTip(tr("The blockchain and the masternode data are up to date"));
    m_net_chainlock.widget->setToolTip(tr("Masternodes have locked the newest block: it cannot be replaced"));
    m_net_peers.widget->setToolTip(tr("Connected peers"));
    m_net_masternodes.widget->setToolTip(tr("Valid masternodes"));
    m_net_mempool.widget->setToolTip(tr("Transactions in the memory pool"));
    // The sky's level, a pill with a glowing dot, from the first level up.
    // It is not in the bar's layout: it takes the bar's free middle when it
    // fits there, so it can never make the window wider than it shrinks to
    // now. Never in discreet mode: the level would give the balance away.
    m_net_sky.widget = new QWidget(networkCard);
    m_net_sky.widget->setObjectName("networkSky");
    m_net_sky.widget->setAttribute(Qt::WA_StyledBackground, true);
    auto* skyLayout = new QHBoxLayout(m_net_sky.widget);
    skyLayout->setContentsMargins(8, 2, 10, 2);
    skyLayout->setSpacing(6);
    m_net_sky.icon = new QLabel(m_net_sky.widget);
    m_net_sky.icon->setObjectName("networkIcon");
    m_net_sky.icon->setFixedSize(18, 18);
    skyLayout->addWidget(m_net_sky.icon);
    m_net_sky.text = new QLabel(m_net_sky.widget);
    m_net_sky.text->setObjectName("networkText");
    skyLayout->addWidget(m_net_sky.text);
    network_words.push_back(m_net_sky.text);
    m_net_sky.widget->hide();
    ui->topLayout->insertWidget(1, networkCard);
    networkCard->installEventFilter(this);
    m_net_masternodes.text->installEventFilter(this);
    setNetworkStatus({});

    // Over the bar while the wallet is locked: the one thing that stops a
    // staking wallet from earning, and the way out. It stays in discreet
    // mode, which hides amounts, not states.
    m_staking_banner = new QFrame(this);
    m_staking_banner->setObjectName("stakingBanner");
    auto* bannerLayout = new QHBoxLayout(m_staking_banner);
    bannerLayout->setContentsMargins(16, 8, 12, 8);
    bannerLayout->setSpacing(12);
    auto* bannerText = new QLabel(tr("Staking is paused while the wallet is locked."), m_staking_banner);
    bannerText->setObjectName("stakingBannerText");
    bannerText->setWordWrap(true);
    bannerLayout->addWidget(bannerText, 1);
    m_staking_banner_button = new QPushButton(tr("Unlock"), m_staking_banner);
    m_staking_banner_button->setObjectName("stakingBannerUnlock");
    m_staking_banner_button->setCursor(Qt::PointingHandCursor);
    m_staking_banner_button->setToolTip(tr("Unlock the wallet for staking. Sending stays locked."));
    bannerLayout->addWidget(m_staking_banner_button);
    m_staking_banner->hide();
    ui->topLayout->insertWidget(1, m_staking_banner);
    connect(m_staking_banner_button, &QPushButton::clicked, this, &OverviewPage::stakingUnlockRequested);

    // The staking card, under the balances: what the wallet's staking is
    // doing in one word, why in one line, and the one thing to do about it;
    // on a locked wallet only the word, the banner above has the rest.
    m_staking_card = new QFrame(this);
    m_staking_card->setObjectName("frameStaking");
    m_staking_card->setFrameShape(QFrame::StyledPanel);
    auto* stakingLayout = new QVBoxLayout(m_staking_card);
    stakingLayout->setContentsMargins(ui->VerticalLayout_PS1->contentsMargins());
    stakingLayout->setSpacing(ui->VerticalLayout_PS1->spacing());
    auto* stakingHeader = new QLabel(tr("Staking"), m_staking_card);
    stakingHeader->setObjectName("labelStakingHeader");
    stakingLayout->addWidget(stakingHeader);
    auto* stakingStateRow = new QHBoxLayout();
    stakingStateRow->setContentsMargins(0, 0, 0, 0);
    stakingStateRow->setSpacing(8);
    m_staking_icon = new QLabel(m_staking_card);
    m_staking_icon->setObjectName("stakingIcon");
    m_staking_icon->setFixedSize(18, 18);
    stakingStateRow->addWidget(m_staking_icon);
    m_staking_state = new QLabel(m_staking_card);
    m_staking_state->setObjectName("stakingState");
    stakingStateRow->addWidget(m_staking_state);
    stakingStateRow->addStretch();
    stakingLayout->addLayout(stakingStateRow);
    m_staking_detail = new QLabel(m_staking_card);
    m_staking_detail->setObjectName("stakingDetail");
    m_staking_detail->setWordWrap(true);
    stakingLayout->addWidget(m_staking_detail);
    auto* stakingButtonRow = new QHBoxLayout();
    stakingButtonRow->setContentsMargins(0, 4, 0, 0);
    m_staking_button = new QPushButton(m_staking_card);
    m_staking_button->setObjectName("stakingButton");
    m_staking_button->setCursor(Qt::PointingHandCursor);
    stakingButtonRow->addWidget(m_staking_button);
    stakingButtonRow->addStretch();
    stakingLayout->addLayout(stakingButtonRow);
    // Stretched down to the right card's bottom, the content stays at the top.
    stakingLayout->setAlignment(Qt::AlignTop);
    m_staking_card->hide();
    ui->verticalLayout_2->insertWidget(1, m_staking_card);
    connect(m_staking_button, &QPushButton::clicked, this, [this] {
        // What the button said is what is asked for: the switch may have
        // moved since the last tick, and "Start" must never pause.
        const StakingState state = stakingState();
        if (state == StakingState::Locked) {
            Q_EMIT stakingUnlockRequested();
        } else {
            Q_EMIT stakingToggleRequested(state == StakingState::Off);
        }
    });

    // Where the CoinJoin card stood while it has nothing to say: one line
    // with a link that opens it, so mixing can still be started from here.
    m_coinjoin_row = new QWidget(this);
    m_coinjoin_row->setObjectName("coinJoinRow");
    auto* coinJoinRowLayout = new QHBoxLayout(m_coinjoin_row);
    coinJoinRowLayout->setContentsMargins(ui->VerticalLayout_PS1->contentsMargins().left(), 0, 0, 0);
    coinJoinRowLayout->setSpacing(6);
    auto* coinJoinRowText = new QLabel(tr("%1: off").arg(QString::fromStdString(gCoinJoinName)), m_coinjoin_row);
    coinJoinRowText->setObjectName("coinJoinRowText");
    coinJoinRowLayout->addWidget(coinJoinRowText);
    auto* coinJoinShow = new QPushButton(tr("Show"), m_coinjoin_row);
    coinJoinShow->setObjectName("coinJoinRowShow");
    coinJoinShow->setCursor(Qt::PointingHandCursor);
    coinJoinShow->setToolTip(tr("Show the %1 card, where mixing is started").arg(QString::fromStdString(gCoinJoinName)));
    coinJoinRowLayout->addWidget(coinJoinShow);
    coinJoinRowLayout->addStretch();
    m_coinjoin_row->hide();
    // Under both columns, not in the left one: the left column's last card then
    // ends where the right card does (updateColumnStretch).
    ui->topLayout->insertWidget(ui->topLayout->indexOf(ui->horizontalLayout) + 1, m_coinjoin_row);
    connect(coinJoinShow, &QPushButton::clicked, this, [this] {
        m_coinjoin_card_pinned = true;
        updateCoinJoinCard();
    });

    m_total_size_policy = ui->labelTotal->sizePolicy();
    m_total_minimum_height = ui->labelTotal->minimumHeight();
    m_coinjoin_form_spacing = ui->coinJoinFormLayout->horizontalSpacing();
    for (QLabel* label : ui->frame->findChildren<QLabel*>()) label->installEventFilter(this);
    installEventFilter(this);
    ui->frameCoinJoin->installEventFilter(this);
    m_staking_card->installEventFilter(this);

    GUIUtil::setFont({ui->label_4,
                      ui->label_5,
                      ui->labelCoinJoinHeader
                     }, GUIUtil::FontWeight::Bold, 16);

    GUIUtil::setFont({ui->labelTotalText,
                      ui->labelWatchTotal,
                      ui->labelTotal
                     }, GUIUtil::FontWeight::Bold, 14);

    GUIUtil::setFont({ui->labelBalanceText,
                      ui->labelPendingText,
                      ui->labelImmatureText,
                      ui->labelWatchonly,
                      ui->labelSpendable
                     }, GUIUtil::FontWeight::Bold);

    // As GUIUtil::createPageHeader sets them on the other pages.
    GUIUtil::setFont({modernTitle}, GUIUtil::FontWeight::Normal, 22);
    GUIUtil::setFont({modernSubtitle}, GUIUtil::FontWeight::Normal, 13);
    GUIUtil::setFont(network_words, GUIUtil::FontWeight::Normal);
    GUIUtil::setFont(network_values, GUIUtil::FontWeight::Bold);
    // Abyss only: the card title as the other cards' in Abyss, the state a
    // step larger than the amounts, the rest in the running size.
    GUIUtil::setFont({stakingHeader}, GUIUtil::FontWeight::Bold, 12);
    GUIUtil::setFont({m_staking_state}, GUIUtil::FontWeight::Bold, 14);
    GUIUtil::setFont({m_staking_detail, bannerText, coinJoinRowText, m_next_level}, GUIUtil::FontWeight::Normal);
    updateThemePresentation();
    GUIUtil::updateFonts();

    m_balances.balance = -1;

    // Recent transactions
    ui->listTransactions->setItemDelegate(txdelegate);
    // Note: minimum height of listTransactions will be set later in updateAdvancedCJUI() to reflect actual settings
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);

    connect(ui->listTransactions, &TransactionOverviewWidget::clicked, this, &OverviewPage::handleTransactionClicked);

    // Abyss says what an empty list means, or that it waits for the sync.
    m_transactions_empty = new EmptyStateWidget("history", tr("No recent transactions"),
                                                tr("Your recent transactions will appear here."), tr("Receive"), ui->listTransactions);
    m_transactions_empty->coverViewport(ui->listTransactions);
    connect(m_transactions_empty, &EmptyStateWidget::actionClicked, this, &OverviewPage::receiveClicked);
    connect(m_transactions_empty, &EmptyStateWidget::listShown, this, [this] {
        updateSyncStatusText();
        updateEmptyState();
    });

    // init "out of sync" warning labels
    updateSyncStatusText();

    QString strCoinJoinName = QString::fromStdString(gCoinJoinName);
    ui->labelCoinJoinHeader->setText(strCoinJoinName);
    ui->labelAnonymizedText->setText(tr("%1 Balance").arg(strCoinJoinName));

    // hide PS frame (helps to preserve saved size)
    // we'll setup and make it visible in coinJoinStatus() later
    ui->frameCoinJoin->setVisible(false);

    // start with displaying the "out of sync" warnings
    showOutOfSyncWarning(true);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, [this]{ coinJoinStatus(); });
}

void OverviewPage::handleTransactionClicked(const QModelIndex &index)
{
    if(filter)
        Q_EMIT transactionClicked(filter->mapToSource(index));
}

void OverviewPage::setPrivacy(bool privacy)
{
    m_privacy = privacy;
    updateSkyBadge();
    updateNextLevel();
    if (m_balances.balance != -1) {
        setBalance(m_balances);
        coinJoinStatus(true);
    }

    ui->listTransactions->setVisible(!m_privacy);

    const QString status_tip = m_privacy ? tr("Discreet mode activated for the Overview tab. To unmask the values, uncheck Settings->Discreet mode.") : "";
    setStatusTip(status_tip);
    QStatusTipEvent event(status_tip);
    QApplication::sendEvent(this, &event);
}

void OverviewPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange) {
        updateThemePresentation();
    }
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        scheduleBalanceWidths();
    }
}

bool OverviewPage::eventFilter(QObject* watched, QEvent* event)
{
    // The scroll area's room for the page: below what the columns need side
    // by side, they stack. A hidden page catches up when it is shown.
    if (m_area != nullptr && watched == m_area->viewport()) {
        if (event->type() == QEvent::Resize && isVisible()) updateColumns();
        return QWidget::eventFilter(watched, event);
    }
    // Shown, the page stands in its window's scroll area, if it has one.
    if (watched == this && event->type() == QEvent::Show) {
        watchScrollArea();
        updateColumns();
    }
    // The bar's own layout has placed its entries by now (a layout sees its
    // widget's events before the filters do); the badge takes the room left.
    if (watched == networkCard && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize)) placeSkyBadge();
    // Which card of the left column is the last one shown decides which one stretches.
    if ((watched == ui->frameCoinJoin || watched == m_staking_card) &&
        (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent)) {
        updateColumnStretch();
    }
    switch (event->type()) {
    case QEvent::FontChange:
    case QEvent::StyleChange:
    case QEvent::Polish:
    case QEvent::Resize:
        scheduleBalanceWidths();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void OverviewPage::scheduleBalanceWidths()
{
    if (m_balance_update_pending || m_applying_balance_widths) return;
    m_balance_update_pending = true;
    QTimer::singleShot(0, this, [this] {
        m_balance_update_pending = false;
        applyBalanceWidths();
    });
}

void OverviewPage::paintEvent(QPaintEvent* event)
{
    // The central StarfieldWidget owns the only Abyss sky.
    QWidget::paintEvent(event);
}

void OverviewPage::updateThemePresentation()
{
    const bool modern = GUIUtil::isModernTheme();
    // Into a theme or out of it, the balances card's floor is read again from
    // the new stylesheet by the first width pass (the stylesheet's minimum the
    // card has then, m_inherited_minimum_widths); until then nothing sets the
    // card's minimum from the old floor, so that it cannot be what is read.
    if (modern != m_modern_presentation) m_balance_card_floor = 0;
    if (modernHeader) {
        modernHeader->setVisible(modern);
    }
    if (networkCard) {
        networkCard->setVisible(modern);
        if (modern) paintNetworkIcons();
        updateSkyBadge();
    }
    if (m_staking_card) {
        m_staking_card->setVisible(modern);
        updateStakingCard();
        updateNextLevel();
        updateCoinJoinCard();
        updateColumnStretch();
    }
    ui->topLayout->setContentsMargins(11, 11, 11, 11);
    ui->topLayout->setSpacing(modern ? 12 : 6);
    // The column holding the balances and CoinJoin cards inherits the row's
    // spacing, which the modern theme zeroes just below, so the two cards
    // touched. Give it the gap the network card leaves above them; -1 hands
    // the classic themes back the inherited spacing the form leaves them with.
    ui->verticalLayout_2->setSpacing(modern ? 12 : -1);
    ui->horizontalLayout->setSpacing(modern ? 0 : 6);
    // The same gap as the balances grid, so that the amounts line up across the cards.
    ui->coinJoinFormLayout->setHorizontalSpacing(modern ? ui->gridLayout->horizontalSpacing() : m_coinjoin_form_spacing);

    // A classic theme never stacks the columns.
    updateColumns();
    applyColumnLayout();
    // Neither the empty fourth grid column nor its spacer may take the amount's room.
    QSpacerItem* spacer = ui->horizontalSpacer_2;
    const auto& original = m_inherited_spacers.emplace(spacer, std::make_pair(spacer->sizeHint(), spacer->sizePolicy())).first->second;
    spacer->changeSize(modern ? 0 : original.first.width(), original.first.height(),
                       modern ? QSizePolicy::Fixed : original.second.horizontalPolicy(), original.second.verticalPolicy());
    for (int col = 0; col < ui->gridLayout->columnCount(); ++col) {
        const int stretch = m_inherited_grid_stretch.emplace(col, ui->gridLayout->columnStretch(col)).first->second;
        ui->gridLayout->setColumnStretch(col, modern ? (col == 1 || col == 2 ? 1 : 0) : stretch);
    }
    if (modern != m_modern_presentation) {
        m_modern_presentation = modern;
        m_inherited_minimum_widths.clear();
        if (!modern) m_classic_widths_pending = true;
        GUIUtil::setFont({ui->label_4, ui->label_5, ui->labelCoinJoinHeader}, GUIUtil::FontWeight::Bold, modern ? 12 : 16);
        // The card's title row keeps to the top. Discreet mode hides the list
        // under it, and the row was then given the whole card: the title sat in
        // its middle and the "out of sync" chip ran its full height. Abyss
        // only; the classic themes keep the form's layout.
        ui->verticalLayout->setAlignment(ui->horizontalLayout_2, modern ? Qt::AlignTop : Qt::Alignment());
        GUIUtil::setFont({ui->labelTotal}, GUIUtil::FontWeight::Bold, modern ? 26 : 14, false, modern);
        ui->labelTotal->setFont(GUIUtil::getFont(GUIUtil::FontWeight::Bold, false, modern ? 26 : 14));
        auto policy = m_total_size_policy;
        if (modern) policy.setHorizontalPolicy(QSizePolicy::Ignored);
        ui->labelTotal->setSizePolicy(policy);
        // The list's own minimum as the new theme has it, once per switch: an
        // empty list's message raises it to what it needs, and an ordinary
        // style pass must not take that back.
        if (m_transaction_list_items > 0) ui->listTransactions->setMinimumHeight(transactionListMinimum(m_transaction_list_items));
    }
    ui->horizontalLayout->invalidate();
    scheduleBalanceWidths();

    // The amounts are formatted per theme (see formatBalance), so a theme
    // change has to rewrite them -- otherwise the page keeps the previous
    // theme's markup until the balance next moves, which on a quiet wallet is
    // a long time. setBalance() re-derives the widths from the new text.
    //
    // Width work is queued until stylesheet polish has settled. In classic
    // themes it only restores once on entry; subsequent resize/font events
    // leave CSS minimums in charge rather than replaying a stale snapshot.
    if (walletModel != nullptr && m_balances.balance != -1) {
        setBalance(m_balances);
    }
    updateSyncStatusText();
    updateEmptyState();
    update();
}

void OverviewPage::updateSyncStatusText()
{
    // The classic themes print a red note; Abyss draws the same words as a
    // neutral chip, so the parentheses go.
    const QString text = GUIUtil::isModernTheme() ? tr("out of sync") : "(" + tr("out of sync") + ")";
    for (QLabel* label : {ui->labelWalletStatus, ui->labelCoinJoinSyncStatus, ui->labelTransactionsStatus}) {
        label->setText(text);
    }
}

void OverviewPage::updateEmptyState()
{
    if (m_transactions_empty == nullptr) return;
    // Rows the overview leaves out (inactive ones, past the limit) are not a
    // filter the user could clear, so an empty list is only empty or waiting
    // for the sync.
    m_transactions_empty->setState(filter == nullptr ? EmptyStateWidget::State::Hidden :
        EmptyStateWidget::stateFor(GUIUtil::isModernTheme(), filter->rowCount(), 0, m_out_of_sync));
}

void OverviewPage::updateNetworkState()
{
    if (networkCard == nullptr || clientModel == nullptr) return;
    NetworkStatus status;
    status.height = m_block_height;
    status.block_age = std::max<qint64>(0, m_block_time.secsTo(QDateTime::currentDateTime()));
    status.chain_synced = clientModel->masternodeSync().isBlockchainSynced();
    status.data_synced = status.chain_synced && clientModel->masternodeSync().isSynced();
    status.progress = status.chain_synced ? m_data_progress : m_verification_progress;
    status.chain_locked = m_chainlock_height >= 0 && m_chainlock_height >= m_block_height;
    status.peers = m_peer_count;
    if (status.chain_synced) status.masternodes = m_masternode_count;
    status.mempool = m_mempool_count;
    setNetworkStatus(status);
}

namespace {
//! A dot in the given color with a soft glow, at the label's pixel ratio
//! like the glyphs: the level's badge and the staking card draw their state
//! with it rather than ship one bitmap per color.
QPixmap GlowDot(const QLabel& label, const QColor& color)
{
    const qreal dpr = label.devicePixelRatioF();
    QPixmap dot(label.size() * dpr);
    dot.setDevicePixelRatio(dpr);
    dot.fill(Qt::transparent);
    QPainter painter(&dot);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QPointF centre(label.width() / 2.0, label.height() / 2.0);
    QColor glow = color;
    glow.setAlphaF(0.45);
    QColor edge = color;
    edge.setAlpha(0);
    QRadialGradient halo(centre, 8);
    halo.setColorAt(0, glow);
    halo.setColorAt(1, edge);
    painter.setBrush(halo);
    painter.drawEllipse(centre, 8, 8);
    painter.setBrush(color);
    painter.drawEllipse(centre, 3.5, 3.5);
    return dot;
}

//! 150628 as "150 628", grouped with the thin space the amounts use.
QString GroupDigits(qint64 number)
{
    QString digits = QString::number(number);
    for (int at = digits.size() - 3; at > (number < 0 ? 1 : 0); at -= 3) {
        digits.insert(at, QChar(THIN_SP_CP));
    }
    return digits;
}

//! The block's age in one unit and plain words: the bar reads "2 minutes ago",
//! not the shared formatter's "2 minute(s)". The plural strings carry %n, so a
//! translation chooses its own forms (Russian has three); English, which has
//! no translation file, says "1 minute" through its own string. The unit
//! changes where GUIUtil::formatNiceTimeOffset changes it.
QString AgeWords(qint64 secs)
{
    constexpr qint64 HOUR = 60 * 60, DAY = 24 * HOUR, WEEK = 7 * DAY, YEAR = 31556952;
    if (secs < 60) return OverviewPage::tr("under a minute");
    if (secs < 2 * HOUR) { const int n = secs / 60; return n == 1 ? OverviewPage::tr("1 minute") : OverviewPage::tr("%n minutes", "", n); }
    if (secs < 2 * DAY) { const int n = secs / HOUR; return n == 1 ? OverviewPage::tr("1 hour") : OverviewPage::tr("%n hours", "", n); }
    if (secs < 2 * WEEK) { const int n = secs / DAY; return n == 1 ? OverviewPage::tr("1 day") : OverviewPage::tr("%n days", "", n); }
    if (secs < YEAR) { const int n = secs / WEEK; return n == 1 ? OverviewPage::tr("1 week") : OverviewPage::tr("%n weeks", "", n); }
    const int n = secs / YEAR;
    return n == 1 ? OverviewPage::tr("1 year") : OverviewPage::tr("%n years", "", n);
}
} // namespace

void OverviewPage::setNetworkStatus(const NetworkStatus& status)
{
    if (networkCard == nullptr) return;
    // Until the masternode data is in as well, the node is still catching up.
    const bool syncing = !status.data_synced;
    const QString age = AgeWords(status.block_age);
    const int percent = std::clamp(static_cast<int>(std::floor(status.progress * 100)), 0, 100);

    m_net_block.value->setText(GroupDigits(status.height));
    m_net_age.text->setText(status.chain_synced ? tr("%1 ago").arg(age) : tr("%1 behind").arg(age));
    m_net_synced.widget->setVisible(!syncing);
    m_net_chainlock.widget->setVisible(!syncing && status.chain_locked);
    m_net_syncing.widget->setVisible(syncing);
    // With the chain in, the remaining masternode data has no honest
    // percentage to show next to a current tip: "Syncing 100%" read as stuck.
    m_net_syncing.text->setText(status.chain_synced ? tr("Syncing data…") : tr("Syncing %1%").arg(percent));
    m_net_syncing.widget->setToolTip(status.chain_synced ? tr("Synchronizing additional data") : tr("Catching up with the blockchain"));
    m_net_progress->setVisible(!status.chain_synced);
    m_net_progress->setValue(qRound(std::clamp(status.progress, 0.0, 1.0) * 1000));
    m_net_peers.value->setText(QString::number(status.peers));
    m_net_peers.text->setText(status.peers == 1 ? tr("peer") : tr("peers"));
    m_net_masternodes.value->setText(status.masternodes ? GroupDigits(*status.masternodes) : QString(QChar(0x2013)));
    m_net_mempool.widget->setVisible(!syncing && status.mempool.has_value());
    m_net_mempool.value->setText(GroupDigits(status.mempool.value_or(0)));
    m_shown_masternodes = status.masternodes;
    // Longer words or numbers may no longer fit on one line, shorter ones may again.
    updateNetworkWrap();
    fitNetworkWords();

    // The card's own border turns amber while syncing. Only the card is
    // repolished; it carries no font of its own.
    if (networkCard->property("syncing").toBool() != syncing) {
        networkCard->setProperty("syncing", syncing);
        networkCard->style()->unpolish(networkCard);
        networkCard->style()->polish(networkCard);
        networkCard->update();
    }
}

void OverviewPage::fitNetworkWords()
{
    const auto advance = [](const NetworkItem& item, const QString& text) {
        return item.widget->isHidden() ? 0 : QFontMetrics(item.text->font()).horizontalAdvance(text);
    };
    const int count = m_shown_masternodes.value_or(2);
    const QString masternodes = count == 1 ? tr("masternode") : tr("masternodes", "", count);
    const QString mempool = tr("in mempool");
    const QString short_masternodes = tr("MN");
    const QString short_mempool = tr("tx");
    // What the bar asks for without these two words, whichever are up now. An
    // explicit minimum outranks the label's own size hint: without it the longer
    // words would hold the window at their width, and it could never get narrow
    // enough to fall back on the short ones. Broken into lines, the words'
    // line is what counts, within the bar's padding.
    const int line = m_net_peers_line ? m_net_peers_line->sizeHint().width() + networkCard->contentsMargins().left() +
                                            networkCard->contentsMargins().right() + NETWORK_MARGINS.left() + NETWORK_MARGINS.right()
                                      : networkCard->layout()->sizeHint().width();
    const int others = line - advance(m_net_masternodes, m_net_masternodes.text->text()) -
                       advance(m_net_mempool, m_net_mempool.text->text());
    constexpr int ROUNDING{4};
    // The level's badge comes first: it takes the bar's free middle, so the
    // words are long only where both fit. While syncing, the expanding
    // progress track takes some of that middle as well, up to its maximum.
    // Broken into lines, the bar shows no badge and the track has its own line.
    const int track_growth = m_net_progress->isHidden() ? 0 : std::max(0, m_net_progress->maximumWidth() - m_net_progress->sizeHint().width());
    const int badge = m_sky_badge_wanted && !m_net_wrapped ? m_net_sky.widget->sizeHint().width() + track_growth : 0;
    const bool room = networkCard->width() >= others + advance(m_net_masternodes, masternodes) + advance(m_net_mempool, mempool) + badge + ROUNDING;
    m_net_masternodes.text->setMinimumWidth(QFontMetrics(m_net_masternodes.text->font()).horizontalAdvance(short_masternodes));
    m_net_mempool.text->setMinimumWidth(QFontMetrics(m_net_mempool.text->font()).horizontalAdvance(short_mempool));
    m_net_masternodes.text->setText(room ? masternodes : short_masternodes);
    m_net_mempool.text->setText(room ? mempool : short_mempool);
    // A resize placed the badge before the words were chosen; place it again
    // now. Changed words re-run the layout, and the badge is placed once more.
    placeSkyBadge();
}

void OverviewPage::paintNetworkIcons()
{
    using GUIUtil::ThemedColor;
    const auto paint = [](const NetworkItem& item, const char* glyph, ThemedColor color) {
        GUIUtil::setTintedSvgPixmap(item.icon, QString(":/icons/tabler/%1.svg").arg(glyph),
                                   item.icon->size(), GUIUtil::getThemedQColor(color));
    };
    paint(m_net_block, "cube", ThemedColor::BLUE);
    paint(m_net_age, "clock", ThemedColor::UNCONFIRMED);
    paint(m_net_synced, "circle-check", ThemedColor::GREEN);
    paint(m_net_chainlock, "shield-check", ThemedColor::GREEN);
    paint(m_net_syncing, "refresh", ThemedColor::ORANGE);
    paint(m_net_peers, "plug-connected", ThemedColor::UNCONFIRMED);
    paint(m_net_masternodes, "server", ThemedColor::UNCONFIRMED);
    paint(m_net_mempool, "stack-2", ThemedColor::UNCONFIRMED);
    // The level's glyph is drawn, not shipped: a dot in the theme's blue.
    m_net_sky.icon->setPixmap(GlowDot(*m_net_sky.icon, GUIUtil::getThemedQColor(ThemedColor::BLUE)));
}

int OverviewPage::skyTier() const
{
    // A resting sky has no level to show: no badge, and the window keeps the plain backdrop.
    return (SkyTiers::dormant() || m_privacy || walletModel == nullptr) ? 0 : m_sky_tier;
}

void OverviewPage::placeSkyBadge()
{
    if (networkCard == nullptr || m_net_sky.widget == nullptr) return;
    QWidget* badge = m_net_sky.widget;
    int left = 0;
    for (const QWidget* entry : {m_net_block.widget, m_net_age.widget, m_net_synced.widget, m_net_chainlock.widget,
                                 m_net_syncing.widget, static_cast<QWidget*>(m_net_progress)}) {
        if (!entry->isHidden()) left = std::max(left, entry->geometry().right() + 1);
    }
    const int right = m_net_peers.widget->geometry().left();
    const QSize size = badge->sizeHint();
    // The bar's own spacing on both sides where there is room, a little less at a pinch.
    const int margin = std::min(NETWORK_SPACING, (right - left - size.width()) / 2);
    // A bar broken into lines has no free middle: the badge stays out, as on one line with no room.
    const bool fits = m_sky_badge_wanted && !m_net_wrapped && networkCard->isVisible() && margin >= 6;
    if (fits) badge->setGeometry(right - margin - size.width(), (networkCard->height() - size.height()) / 2, size.width(), size.height());
    if (badge->isHidden() == fits) badge->setVisible(fits);
}

void OverviewPage::buildNetworkLayout()
{
    // Built anew, never rearranged: on one line the bar has the very layout
    // it always had, so nothing on it moves by a pixel.
    delete networkCard->layout();
    m_net_peers_line = nullptr;
    const auto add = [](QBoxLayout* line, std::initializer_list<QWidget*> entries) {
        for (QWidget* entry : entries) line->addWidget(entry);
    };
    if (!m_net_wrapped) {
        auto* layout = new QHBoxLayout(networkCard);
        layout->setContentsMargins(NETWORK_MARGINS);
        layout->setSpacing(NETWORK_SPACING);
        add(layout, {m_net_block.widget, m_net_age.widget, m_net_separator, m_net_synced.widget, m_net_chainlock.widget,
                     m_net_syncing.widget, m_net_progress});
        layout->addStretch();
        add(layout, {m_net_peers.widget, m_net_masternodes.widget, m_net_mempool.widget});
        // Hidden only while the bar was broken into lines.
        if (m_net_separator->isHidden() && m_net_separator->testAttribute(Qt::WA_WState_ExplicitShowHide)) m_net_separator->show();
        return;
    }
    // Three lines: the tip and its age, the node's state, its company. The
    // separator stands between the first two on one line only.
    auto* layout = new QVBoxLayout(networkCard);
    layout->setContentsMargins(NETWORK_MARGINS);
    layout->setSpacing(NETWORK_LINE_SPACING);
    const auto line = [&](std::initializer_list<QWidget*> entries) {
        auto* row = new QHBoxLayout();
        row->setSpacing(NETWORK_SPACING);
        layout->addLayout(row);
        add(row, entries);
        row->addStretch();
        return row;
    };
    line({m_net_block.widget, m_net_age.widget});
    line({m_net_synced.widget, m_net_chainlock.widget, m_net_syncing.widget, m_net_progress});
    m_net_peers_line = line({m_net_peers.widget, m_net_masternodes.widget, m_net_mempool.widget});
    m_net_separator->hide();
}

int OverviewPage::networkOneLineWidth() const
{
    // What the bar asks for on one line, the words short, as its layout counts
    // it: the entries on show, the bar's spacing between them (none beside the
    // stretch, which is empty), its padding and the card's border. The
    // separator counts even while the bar is broken into lines and hides it.
    const QMargins card = networkCard->contentsMargins();
    int width = card.left() + card.right() + NETWORK_MARGINS.left() + NETWORK_MARGINS.right();
    int shown = 0;
    for (QWidget* entry : {m_net_block.widget, m_net_age.widget, static_cast<QWidget*>(m_net_separator), m_net_synced.widget,
                           m_net_chainlock.widget, m_net_syncing.widget, static_cast<QWidget*>(m_net_progress), m_net_peers.widget,
                           m_net_masternodes.widget, m_net_mempool.widget}) {
        if (entry == m_net_separator) {
            width += m_net_separator->minimumWidth();
        } else {
            QWidgetItem item(entry);
            if (item.isEmpty()) continue;
            width += item.minimumSize().width();
        }
        ++shown;
    }
    return width + std::max(0, shown - 1) * NETWORK_SPACING;
}

void OverviewPage::updateNetworkWrap()
{
    // Stacked, where the page's room in the scroll area is less than the bar
    // needs on one line, the bar breaks into lines. The room is the viewport
    // as it stands, as for the columns: the lines only add height, so a
    // vertical bar they bring on keeps them.
    if (networkCard == nullptr) return;
    const int one_line = networkOneLineWidth();
    // What the bar needs on one line counts for the breakpoint whichever way
    // it stands (twoColumnWidth): when it changes, the width pass measures again.
    const bool one_line_changed = one_line != m_net_one_line;
    m_net_one_line = one_line;
    bool wrapped = false;
    if (m_stacked && m_area != nullptr) {
        const QMargins page = contentsMargins();
        const QMargins rows = ui->topLayout->contentsMargins();
        const int room = m_area->viewport()->width() - page.left() - page.right() - rows.left() - rows.right();
        wrapped = room < one_line;
    }
    if (wrapped != m_net_wrapped) {
        m_net_wrapped = wrapped;
        buildNetworkLayout();
        // The card's minimum is the new layout's from now, not from its first
        // layout request: the page's minimum is read from it before that.
        networkCard->layout()->activate();
        fitNetworkWords();
    } else if (!one_line_changed) {
        return;
    }
    scheduleBalanceWidths();
}

void OverviewPage::updateSkyBadge()
{
    const int tier = skyTier();
    if (m_net_sky.widget != nullptr) {
        m_sky_badge_wanted = tier > 0 && GUIUtil::isModernTheme();
        m_net_sky.text->setText(SkyTiers::name(tier));
        QString tip = tr("Night sky level: %1").arg(SkyTiers::name(tier));
        if (tier == SkyTiers::COUNT - 1) {
            tip += QLatin1Char('\n') + tr("The highest level.");
        } else if (tier > 0 && walletModel != nullptr && walletModel->getOptionsModel() != nullptr) {
            const int unit = walletModel->getOptionsModel()->getDisplayUnit();
            const CAmount next = SkyTiers::threshold(tier + 1);
            tip += QLatin1Char('\n') + tr("Next: %1 from %2").arg(SkyTiers::name(tier + 1),
                GUIUtil::isModernTheme() ? AbyssUnits::format(unit, next)
                                         : BitcoinUnits::formatWithUnit(unit, next, false, BitcoinUnits::SeparatorStyle::ALWAYS));
        }
        m_net_sky.widget->setToolTip(tip);
        fitNetworkWords();
        placeSkyBadge();
    }
    if (tier != m_sky_tier_shown) {
        m_sky_tier_shown = tier;
        Q_EMIT skyTierChanged(tier);
    }
}

void OverviewPage::updateNextLevel()
{
    if (m_next_level == nullptr) return;
    // The level is the balance's own (not skyTier(), which is 0 in discreet
    // mode): the line goes with the amounts, which discreet mode masks, and
    // with the level, which a wallet without keys does not have.
    const bool shown = GUIUtil::isModernTheme() && !m_privacy && walletModel != nullptr &&
                       walletModel->getOptionsModel() != nullptr && !walletModel->wallet().privateKeysDisabled();
    if (shown) {
        if (SkyTiers::dormant()) {
            // The sky rests and has no level to name. The line stays, blank,
            // with its height: the pages were fitted to the Overview's height
            // with it (the Receive page's first view, U2), and are not refitted.
            m_next_level->setText(QStringLiteral(" "));
            m_next_level->setToolTip(QString());
        } else if (m_sky_tier >= SkyTiers::COUNT - 1) {
            m_next_level->setText(tr("Top level"));
        } else {
            const int unit = walletModel->getOptionsModel()->getDisplayUnit();
            // The thresholds are whole coins: Abyss's amounts show no decimals for them.
            m_next_level->setText(tr("Next level at %1").arg(AbyssUnits::format(unit, SkyTiers::threshold(m_sky_tier + 1))));
        }
        if (!SkyTiers::dormant()) m_next_level->setToolTip(tr("The night sky behind the wallet grows with the balance: %1 now.").arg(SkyTiers::name(m_sky_tier)));
    }
    if (m_next_level->isHidden() == shown) m_next_level->setVisible(shown);
}

OverviewPage::StakingState OverviewPage::stakingState() const
{
    // In the order the thread itself gives up: a wallet that cannot sign
    // (no keys, or locked) before the switch, the chain before the coins.
    if (m_staking.keyless) return StakingState::Unavailable;
    if (!m_staking.minter) return StakingState::Disabled;
    if (m_staking.locked) return StakingState::Locked;
    if (!m_staking.enabled) return StakingState::Off;
    if (m_staking.syncing) return StakingState::Syncing;
    // The wallet's word stays "staking" across passes that never search; the
    // thread's own flag drops when a pass fails before the search.
    if (m_staking.wallet_state == IS_STAKING && m_staking.searching) return StakingState::Staking;
    return StakingState::Ready;
}

bool OverviewPage::stakingWaitsForSync(const StakingGates& gates)
{
    // As the thread checks them: the peers and the initial download on the
    // main network only; the proof-of-work end, the masternode data and the
    // clock on every network. A tip stamped ahead of the clock (up to the
    // three minutes validation allows) keeps the thread from searching, and
    // its last word on the wallet would still say "staking".
    if (gates.mainnet && (gates.peers < size_t(STAKING_MIN_PEERS_MAINNET) || gates.initial_download)) return true;
    if (gates.height < gates.last_pow_block) return true;
    if (gates.adjusted_time < gates.tip_time) return true;
    return !gates.masternodes_synced;
}

void OverviewPage::setStakingStatus(const StakingStatus& status)
{
    m_staking = status;
    updateStakingCard();
}

void OverviewPage::updateStakingCard()
{
    if (m_staking_card == nullptr) return;
    const bool modern = GUIUtil::isModernTheme();
    const StakingState state = stakingState();
    // The line says what is the matter and what to do about it; what only a
    // log or a command line can use (the thread, the option's name) is the
    // line's tooltip.
    QString word, detail, tip, button;
    GUIUtil::ThemedColor color = GUIUtil::ThemedColor::UNCONFIRMED;
    switch (state) {
    case StakingState::Unavailable:
        word = tr("Not available");
        detail = tr("This wallet holds no private keys, so it cannot stake.");
        break;
    case StakingState::Disabled:
        word = tr("Off");
        if (m_staking.configured) {
            // Not running while configured: the thread stopped on an error and
            // is between retries; the log has the reason, the card must not
            // guess one.
            detail = tr("Staking stopped and will retry soon. If it stays off, restart the wallet.");
            tip = tr("The staking thread is not running. The debug log file has the reason.");
        } else {
            detail = tr("Staking is turned off in the startup settings. To stake, turn it on and restart.");
            tip = tr("The wallet was started with -staking=0, in its configuration file or on its command line.");
        }
        break;
    case StakingState::Locked:
        // The banner over the network bar says why and holds the unlock, so
        // the card keeps to the state: the banner's room comes out of the
        // card's line and button, and the page needs no more height locked
        // than unlocked. At 175% on a 1600-pixel display there was none to
        // spare, and the window, held under the screen, squeezed the balances
        // card instead.
        word = tr("Paused: wallet locked");
        color = GUIUtil::ThemedColor::ORANGE;
        break;
    case StakingState::Off:
        word = tr("Off");
        detail = tr("Start staking to earn rewards with your balance.");
        button = tr("Start staking");
        break;
    case StakingState::Syncing:
        word = tr("Waiting for sync");
        detail = tr("Staking starts once the wallet is up to date with the network.");
        button = tr("Pause staking");
        color = GUIUtil::ThemedColor::ORANGE;
        break;
    case StakingState::Ready:
        word = tr("Ready");
        // The thread's reasons are coarse: no spendable balance over the
        // reserve, or nothing it could select -- immature or too young, too
        // small or too large, or exactly a collateral. None of these is a
        // wait that ends by itself, so the card does not promise one.
        detail = m_staking.wallet_state == NOT_STAKING_BALANCE ? tr("No spendable balance to stake.") :
                 m_staking.wallet_state == NOT_STAKING_DEPTH ? tr("None of the coins can stake right now.") :
                 tr("Looking for coins to stake.");
        if (m_staking.wallet_state == NOT_STAKING_DEPTH) {
            tip = tr("Coins stay out while immature or too young, under the smallest or over the largest stake, or exactly a masternode collateral.");
        }
        button = tr("Pause staking");
        color = GUIUtil::ThemedColor::BLUE;
        break;
    case StakingState::Staking:
        word = tr("Staking");
        // The thread is searching; a reward comes only with a block it finds.
        detail = tr("This wallet is staking. A reward comes with each block it stakes.");
        button = tr("Pause staking");
        color = GUIUtil::ThemedColor::GREEN;
        break;
    }
    m_staking_state->setText(word);
    m_staking_detail->setText(detail);
    // An empty line would still take a row and its spacing.
    if (m_staking_detail->isHidden() == !detail.isEmpty()) m_staking_detail->setVisible(!detail.isEmpty());
    m_staking_detail->setToolTip(tip);
    m_staking_button->setText(button);
    // Each tick lands here: a name that did not change is not announced again.
    if (m_staking_button->accessibleName() != button) m_staking_button->setAccessibleName(button);
    if (m_staking_button->isHidden() == !button.isEmpty()) m_staking_button->setVisible(!button.isEmpty());
    if (modern) {
        // Nor is a dot of the same color at the same pixel ratio painted again.
        const QColor dot = GUIUtil::getThemedQColor(color);
        const qreal dpr = m_staking_icon->devicePixelRatioF();
        if (dot.rgba() != m_staking_dot_rgba || dpr != m_staking_dot_dpr) {
            m_staking_dot_rgba = dot.rgba();
            m_staking_dot_dpr = dpr;
            m_staking_icon->setPixmap(GlowDot(*m_staking_icon, dot));
        }
    }
    const bool banner = modern && state == StakingState::Locked;
    if (m_staking_banner->isHidden() == banner) m_staking_banner->setVisible(banner);
}

void OverviewPage::updateColumnStretch()
{
    if (m_staking_card == nullptr) return;
    const bool modern = GUIUtil::isModernTheme();
    // The left column's cards, top down. In Abyss none goes below its natural
    // height: where the page is short of room, its minimum grows, and so does
    // the window's (BitcoinGUI::applyModernWindowMinimum, up to the screen).
    // A locked wallet's banner used to squeeze the balances card instead. The
    // last card shown takes the room down to the right card's bottom.
    const std::array<QWidget*, 3> cards{ui->frame, m_staking_card, ui->frameCoinJoin};
    QWidget* last = nullptr;
    for (QWidget* card : cards) {
        if (!card->isHidden()) last = card;
    }
    for (QWidget* card : cards) {
        const QSizePolicy original = m_inherited_card_policies.emplace(card, card->sizePolicy()).first->second;
        QSizePolicy policy = original;
        if (modern) policy.setVerticalPolicy(card == last ? QSizePolicy::MinimumExpanding : QSizePolicy::Minimum);
        if (card->sizePolicy() != policy) card->setSizePolicy(policy);
    }
    // The column's closing spacer would take that room instead.
    QSpacerItem* spacer = ui->verticalSpacer;
    const auto& original = m_inherited_spacers.emplace(spacer, std::make_pair(spacer->sizeHint(), spacer->sizePolicy())).first->second;
    spacer->changeSize(modern ? 0 : original.first.width(), modern ? 0 : original.first.height(),
                       modern ? QSizePolicy::Fixed : original.second.horizontalPolicy(),
                       modern ? QSizePolicy::Fixed : original.second.verticalPolicy());
    ui->verticalLayout_2->invalidate();
}

void OverviewPage::applyColumnLayout()
{
    const bool modern = GUIUtil::isModernTheme();
    // Stacked, the Recent transactions card stands under the cards.
    ui->horizontalLayout->setDirection(m_stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    for (int item = 0; item < ui->horizontalLayout->count(); ++item) {
        const int inherited = m_inherited_stretch.emplace(item, ui->horizontalLayout->stretch(item)).first->second;
        QLayoutItem* entry = ui->horizontalLayout->itemAt(item);
        QSpacerItem* spacer = entry->spacerItem();
        // Stacked, the height to spare goes to the Recent transactions, not to the cards.
        const int stretch = m_stacked ? (entry->layout() == ui->verticalLayout_3 ? 1 : 0) : modern ? (spacer ? 0 : 1) : inherited;
        ui->horizontalLayout->setStretch(item, stretch);
        if (spacer) {
            const auto& original = m_inherited_spacers.emplace(spacer, std::make_pair(spacer->sizeHint(), spacer->sizePolicy())).first->second;
            if (m_stacked) {
                // The gap between the columns, now downwards; the side spacers take nothing.
                spacer->changeSize(0, item == 2 ? ABYSS_COLUMN_GAP : 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
            } else {
                spacer->changeSize(modern ? (item == 2 ? ABYSS_COLUMN_GAP : 0) : original.first.width(), original.first.height(),
                                   modern ? QSizePolicy::Fixed : original.second.horizontalPolicy(), original.second.verticalPolicy());
            }
        }
    }
}

int OverviewPage::twoColumnWidth() const
{
    // Side by side, the row of cards is the left column, the gap and the right
    // column (Abyss gives the row no spacing and its side spacers no width);
    // the page is as wide as its widest row, within its margins. Stacked, the
    // balances card has no floor of its own and the network bar may stand in
    // lines (applyBalanceCardFloor, updateNetworkWrap); side by side both are
    // as they were, and this is what the columns need then.
    // The left column as the side-by-side layout measures it: the widest of
    // its cards, the balances card at its floor (not its own minimum, which
    // stacked counts its border too), within the column's margins.
    const QMargins left = ui->verticalLayout_2->contentsMargins();
    int left_column = m_balance_card_floor;
    for (int item = 0; item < ui->verticalLayout_2->count(); ++item) {
        QLayoutItem* card = ui->verticalLayout_2->itemAt(item);
        if (card->widget() != ui->frame && !card->isEmpty()) left_column = std::max(left_column, card->minimumSize().width());
    }
    left_column += left.left() + left.right();
    int widest = left_column + ABYSS_COLUMN_GAP + ui->verticalLayout_3->minimumSize().width();
    for (int item = 0; item < ui->topLayout->count(); ++item) {
        QLayoutItem* row = ui->topLayout->itemAt(item);
        if (row->layout() == ui->horizontalLayout) continue;
        const bool bar_in_lines = m_net_wrapped && row->widget() == networkCard && !row->isEmpty();
        widest = std::max(widest, bar_in_lines ? networkOneLineWidth() : row->minimumSize().width());
    }
    const QMargins margins = ui->topLayout->contentsMargins();
    return margins.left() + margins.right() + widest;
}

void OverviewPage::watchScrollArea()
{
    QScrollArea* area = nullptr;
    for (QWidget* parent = parentWidget(); parent != nullptr && area == nullptr; parent = parent->parentWidget()) {
        area = qobject_cast<QScrollArea*>(parent);
    }
    if (area == m_area) return;
    if (m_area) m_area->viewport()->removeEventFilter(this);
    m_area = area;
    if (m_area) m_area->viewport()->installEventFilter(this);
}

void OverviewPage::updateColumns()
{
    // Stacked where the scroll area's room with no scroll bar is less than
    // the columns need side by side. Where it has that width but a vertical
    // bar on show takes the columns' last pixels, they stand side by side
    // only if, side by side, the page needs no bar there: measured, not read
    // off the bar on show, which may still be the other state's, so the
    // state the page turns to keeps. Measured at the columns' own width:
    // with its bar on show the scroll area lays the page out there (the page
    // is no narrower), and takes the bar away only if the page's height for
    // that width fits (QScrollArea's updateScrollBars); a wider room's
    // height would turn the columns behind a bar that stays.
    bool stacked = false;
    if (GUIUtil::isModernTheme() && m_area != nullptr && m_two_column_width > 0) {
        const QSize room = m_area->maximumViewportSize();
        if (room.width() < m_two_column_width) {
            stacked = true;
        } else if (m_area->viewport()->width() < m_two_column_width) {
            stacked = sideBySideNeed(m_two_column_width) > room.height();
        }
    }
    if (stacked != m_stacked) {
        m_stacked = stacked;
        applyBalanceCardFloor();
        applyColumnLayout();
        ui->horizontalLayout->invalidate();
        ui->topLayout->invalidate();
        // The page's minimum follows: the two columns' side by side, the widest row's stacked.
        scheduleBalanceWidths();
    }
    updateNetworkWrap();
}

void OverviewPage::applyBalanceCardFloor()
{
    // Side by side, the balances card keeps its floor: general.css's 490 px,
    // or what its amounts need beyond that. Stacked it has none of its own,
    // and what its labels and amounts need is its minimum, so a narrow page
    // is not held wider than they are; an amount is never cut either way.
    if (!GUIUtil::isModernTheme() || m_balance_card_floor <= 0) return;
    const int wanted = m_stacked ? 0 : m_balance_card_floor;
    if (ui->frame->minimumWidth() != wanted) ui->frame->setMinimumWidth(wanted);
}

int OverviewPage::sideBySideNeed(int width)
{
    // What moves here sends resize and font events; no width pass may start
    // from them.
    const bool applying = m_applying_balance_widths;
    m_applying_balance_widths = true;
    const auto invalidate = [this] {
        for (QLayout* nested : findChildren<QLayout*>()) nested->invalidate();
        ui->topLayout->invalidate();
    };
    // As WalletFrame::pageWhole measures a page: its minimum height, and
    // more where wrapped text asks for it at that width.
    const auto need = [this, width] {
        int height = ui->topLayout->totalMinimumSize().height();
        if (ui->topLayout->hasHeightForWidth()) height = std::max(height, ui->topLayout->totalHeightForWidth(width));
        return height;
    };
    const bool was_stacked = m_stacked;
    // At the width an empty list's message may wrap more, and the list's
    // resize raises its minimum to it (EmptyStateWidget::fitList): that
    // counts side by side, and is put back before the page is laid out as it is.
    const EmptyStateWidget::Fit fit = m_transactions_empty->fit();
    m_stacked = false;
    applyColumnLayout();
    // Side by side, the balances card has its floor and the network bar its one line.
    const bool was_wrapped = m_net_wrapped;
    applyBalanceCardFloor();
    if (was_wrapped) {
        m_net_wrapped = false;
        buildNetworkLayout();
        networkCard->layout()->activate();
    }
    invalidate();
    // Laid out at the width, as tall as it needs: the total's card has its
    // side-by-side width, and the font is fitted to it.
    ui->topLayout->setGeometry(QRect(0, 0, width, std::max(need(), height())).marginsRemoved(contentsMargins()));
    fitTotalFont();
    invalidate();
    // The balances card's minimum height is what its layout set when it last
    // ran; the total's fitted height reaches it only when that layout runs
    // again, so it runs now, not at the next event.
    ui->frame->layout()->activate();
    const int side_by_side = need();
    m_stacked = was_stacked;
    applyColumnLayout();
    applyBalanceCardFloor();
    if (was_wrapped) {
        m_net_wrapped = true;
        buildNetworkLayout();
        networkCard->layout()->activate();
    }
    m_transactions_empty->restoreFit(fit);
    invalidate();
    ui->topLayout->setGeometry(contentsRect());
    fitTotalFont();
    invalidate();
    m_applying_balance_widths = applying;
    return side_by_side;
}

QSize OverviewPage::sideBySideWhole()
{
    if (!m_stacked) return {};
    return QSize(m_two_column_width, sideBySideNeed(m_two_column_width));
}

void OverviewPage::updateCoinJoinCard()
{
    if (m_coinjoin_row == nullptr || !m_coinjoin_known) return;
    const bool modern = GUIUtil::isModernTheme();
    if (!modern) {
        // As before: the card whenever CoinJoin is on; disabled, it stays as
        // the classic status last left it (not as Abyss did) and says so itself.
        if (ui->frameCoinJoin->isHidden() == m_coinjoin_classic_card) ui->frameCoinJoin->setVisible(m_coinjoin_classic_card);
        if (!m_coinjoin_row->isHidden()) m_coinjoin_row->hide();
        return;
    }
    // Mixed coins are a reason to show the card; a balance never received is not.
    const bool mixed = m_balances.balance != -1 && m_balances.anonymized_balance + m_balances.dfcn_scale_anonymized_balance > 0;
    const bool card = m_coinjoin_disabled ? m_coinjoin_card_pinned :
                      m_coinjoin_enabled && (m_coinjoin_mixing || mixed || m_coinjoin_card_pinned);
    if (ui->frameCoinJoin->isHidden() == card) ui->frameCoinJoin->setVisible(card);
    const bool row = (m_coinjoin_disabled || m_coinjoin_enabled) && !card;
    if (m_coinjoin_row->isHidden() == row) m_coinjoin_row->setVisible(row);
}

OverviewPage::~OverviewPage()
{
    delete ui;
}

std::vector<QLabel*> OverviewPage::balanceLabels() const
{
    return {ui->labelBalance, ui->labelUnconfirmed, ui->labelImmature, ui->labelTotal,
            ui->labelWatchAvailable, ui->labelWatchPending, ui->labelWatchImmature, ui->labelWatchTotal};
}

//! Everywhere else these labels are given HTML, in which the thousands
//! separator is a space fixed at 6 pt (HTML_HACK_SP). Beside an ordinary
//! amount that reads; beside the total Abyss draws at 26 pt it measures 4 px
//! against digits 20 px wide, so "50 820 975.99" arrives as "50820975.99"
//! while every smaller amount on the same card is grouped correctly -- which
//! is exactly what the page looked like. Plain text keeps the real U+2009
//! THIN SPACE: it belongs to the text run, so it scales with the font.
//!
//! It buys a second thing the layout needs. A rich-text label's width is an
//! estimate taken from a QTextDocument; a plain one's comes from the font's own
//! metrics, and that is the number the caps below are raised to.
//!
//! Abyss leaves out the trailing zeros (U3-C: "0.5 DFCN", "935 000 000 DFCN");
//! discreet mode keeps the usual mask, which does not tell how many digits an
//! amount has.
QString OverviewPage::formatBalance(int unit, const CAmount& amount) const
{
    if (GUIUtil::isModernTheme()) {
        if (m_privacy) return BitcoinUnits::floorWithPrivacy(unit, amount, BitcoinUnits::SeparatorStyle::ALWAYS, true);
        return AbyssUnits::format(unit, amount, false, true);
    }
    return BitcoinUnits::floorHtmlWithPrivacy(unit, amount, BitcoinUnits::SeparatorStyle::ALWAYS, m_privacy);
}

//! The amount, and the tooltip that goes with it: in Abyss the exact amount
//! above the form's explanation (only the amount where the form has none);
//! in discreet mode the explanation alone; in a classic theme exactly the
//! form's tooltip, so that a switch back restores it.
void OverviewPage::setBalanceText(QLabel* label, int unit, const CAmount& amount)
{
    label->setText(formatBalance(unit, amount));
    const auto form = m_form_tooltips.find(label);
    const QString explanation = form == m_form_tooltips.end() ? QString() : form->second;
    if (!GUIUtil::isModernTheme() || m_privacy) {
        label->setToolTip(explanation);
        return;
    }
    const QString exact = AbyssUnits::exact(unit, amount);
    label->setToolTip(explanation.isEmpty() ? exact : exact + QLatin1Char('\n') + explanation);
}

// Coalesced after style/font/layout changes. Qt's qSmartMinSize uses explicit
// minimums ahead of size hints, so Abyss must raise the labels before asking
// their card and the page how much room is needed. Classic themes instead let
// the current stylesheet restore its minimums once on entry: a snapshot taken
// before polish is not a valid replacement for general.css's 60px/490px floors.
void OverviewPage::applyBalanceWidths()
{
    m_applying_balance_widths = true;
    const bool modern = GUIUtil::isModernTheme();
    for (QLabel* label : balanceLabels()) label->setTextFormat(modern ? Qt::PlainText : Qt::AutoText);
    if (!modern) {
        if (m_classic_widths_pending) {
            std::map<QWidget*, QFont> presentation_fonts;
            for (const auto& entry : m_form_minimum_widths) {
                presentation_fonts.emplace(entry.first, entry.first->font());
            }
            const auto restore = [&](QWidget* widget) {
                widget->setMinimumWidth(m_form_minimum_widths.at(widget));
                if (widget == ui->labelTotal) widget->setMinimumHeight(m_total_minimum_height);
                widget->style()->polish(widget);
                // Follow Qt's repolish protocol: QFrame/QLabel need StyleChange
                // to refresh their CSS border/padding contents rectangles too.
                QEvent style_change(QEvent::StyleChange);
                QApplication::sendEvent(widget, &style_change);
                // The form uses the whole widget as each frame's rectangle.
                // Recompute contents margins from the current style's widths,
                // rather than preserving padding left over from Abyss.
                if (auto* frame = qobject_cast<QFrame*>(widget)) frame->setFrameRect({});
            };
            for (QLabel* label : ui->frame->findChildren<QLabel*>()) restore(label);
            restore(ui->frame);
            restore(this);
            // Polishing ancestors can propagate their saved style font to
            // children. Restore presentation fonts only after all polish calls,
            // then refresh QLabel's cached rich-text font even if QWidget's
            // public font compares equal and setFont would omit FontChange.
            setFont(presentation_fonts.at(this));
            ui->frame->setFont(presentation_fonts.at(ui->frame));
            for (QLabel* label : ui->frame->findChildren<QLabel*>()) {
                label->setFont(presentation_fonts.at(label));
                QEvent font_change(QEvent::FontChange);
                QApplication::sendEvent(label, &font_change);
            }
            // Preserve the original page's screen bound in classic themes.
            // Resizes must not reapply these minimums after CSS has settled.
            if (const QScreen* display = screen()) {
                setMinimumWidth(std::min(minimumWidth(), display->availableGeometry().width()));
            }
            m_classic_widths_pending = false;
            // The restored fonts and CSS box metrics also change cached heights.
            for (QLayout* nested : findChildren<QLayout*>()) nested->invalidate();
            ui->topLayout->invalidate();
            ui->topLayout->activate();
        }
        m_applying_balance_widths = false;
        return;
    }

    ensurePolished();
    ui->frame->ensurePolished();
    auto inherited = [&](QWidget* widget) {
        return m_inherited_minimum_widths.emplace(widget, widget->minimumWidth()).first->second;
    };
    // The labels of both cards share one column, the stylesheet's width or what
    // a larger font needs, so that the amounts stand under each other.
    const std::array<QLabel*, 4> balance_column{ui->labelBalanceText, ui->labelPendingText, ui->labelImmatureText, ui->labelTotalText};
    const std::array<QLabel*, 5> coinjoin_column{ui->labelCoinJoinEnabledText, ui->labelCompletitionText, ui->labelAnonymizedText,
                                                 ui->labelAmountAndRoundsText, ui->labelSubmittedDenomText};
    int column = 0;
    const auto measure = [&](QLabel* label) {
        label->ensurePolished();
        // Not the size hint: it includes the label's minimum, so a column raised
        // for a larger font would never come back.
        column = std::max({column, inherited(label), QFontMetrics(label->font()).horizontalAdvance(label->text()) + 2});
    };
    std::for_each(balance_column.begin(), balance_column.end(), measure);
    std::for_each(coinjoin_column.begin(), coinjoin_column.end(), measure);
    // A label caches its size hint with the minimum it had then, and lowering the
    // minimum does not clear that: the layouts would keep the old, wider column.
    const auto set_column = [column](QLabel* label) {
        if (label->minimumWidth() == column) return;
        label->setMinimumWidth(column);
        QEvent font_change(QEvent::FontChange);
        QApplication::sendEvent(label, &font_change);
    };
    std::for_each(balance_column.begin(), balance_column.end(), set_column);
    std::for_each(coinjoin_column.begin(), coinjoin_column.end(), set_column);
    for (QLabel* label : ui->frame->findChildren<QLabel*>()) {
        if (label == m_next_level) continue; // Takes the column's width, never sets it.
        label->ensurePolished();
        const int original = inherited(label);
        int wanted;
        if (label == ui->labelTotal) {
            QFont font = GUIUtil::getFont(GUIUtil::FontWeight::Bold, false, 26);
            const qreal minimum = std::max(1.0, GUIUtil::internal::EffectivePointSize(ui->labelBalance->font(), label->logicalDpiY()).value_or(1.0));
            font.setPointSizeF(minimum);
            const int padding = label->contentsMargins().left() + label->contentsMargins().right() +
                                2 * label->margin() + std::max(0, label->indent());
            wanted = std::max(original, int(std::ceil(QFontMetricsF(font).horizontalAdvance(label->text()))) + padding + 2);
        } else if (std::find(balance_column.begin(), balance_column.end(), label) != balance_column.end()) {
            wanted = column;
        } else {
            wanted = std::max(original, label->sizeHint().width());
        }
        if (label->minimumWidth() != wanted) label->setMinimumWidth(wanted);
    }
    fitNetworkWords();
    // Invalidate bottom up before reading cached layout minimums. Otherwise a
    // card can repeat its width from before the amount labels were remeasured.
    for (QWidget* widget : {static_cast<QWidget*>(ui->frame), static_cast<QWidget*>(this)}) {
        int wanted = inherited(widget);
        if (widget->layout()) {
            for (QLayout* nested : widget->findChildren<QLayout*>()) nested->invalidate();
            widget->layout()->invalidate();
            if (widget == ui->frame) {
                // Its floor side by side; stacked it has none (applyBalanceCardFloor).
                m_balance_card_floor = std::max(wanted, widget->layout()->minimumSize().width());
                applyBalanceCardFloor();
                continue;
            }
            if (widget == this) {
                // Where the scroll area gives the page less than its columns
                // need side by side, they stack; the form's minimum is a
                // two-column width and holds only while they do not.
                m_two_column_width = std::max(wanted, twoColumnWidth());
                updateColumns();
                if (m_stacked) wanted = 0;
            }
            wanted = std::max(wanted, widget->layout()->minimumSize().width());
        }
        // Abyss keeps the measured text floor even on a smaller screen so the
        // requested minimum-size view does not clip the amount or its unit.
        // Classic themes retain the original screen bound in the branch above.
        if (widget->minimumWidth() != wanted) widget->setMinimumWidth(wanted);
    }
    ui->topLayout->activate();
    fitTotalFont();
    m_applying_balance_widths = false;
}

void OverviewPage::fitTotalFont()
{
    QLabel* label = ui->labelTotal;
    QFont font = GUIUtil::getFont(GUIUtil::FontWeight::Bold, false, 26);
    // Pixel-sized fonts report pointSizeF() == -1. Convert the floor through
    // the existing shared helper and keep it positive: Qt ignores nonpositive
    // point sizes, which would otherwise leave the loop stuck at the same size.
    const qreal minimum = std::max(1.0, GUIUtil::internal::EffectivePointSize(ui->labelBalance->font(), label->logicalDpiY()).value_or(1.0));
    if (font.pointSizeF() <= 0) font.setPointSizeF(std::max(minimum, qreal(GUIUtil::getScaledFontSize(26))));
    const int available = label->contentsRect().width() - 2 * label->margin() - std::max(0, label->indent()) - 2;
    // Quarter-point steps keep the whole formatted amount visible, including
    // its unit. The minimum width is measured at the floor, not at the hero size.
    while (font.pointSizeF() > minimum && QFontMetricsF(font).horizontalAdvance(label->text()) > available) {
        const qreal previous = font.pointSizeF();
        font.setPointSizeF(std::max(minimum, previous - 0.25));
        if (font.pointSizeF() >= previous) break;
    }
    if (label->font() != font) label->setFont(font);
    // CSS's small-label floor otherwise overrides the larger Abyss font's
    // natural height in the grid. Keep the complete rendered amount visible;
    // classic entry restores the original height before its stylesheet polish.
    // The height is the text's own at this font, measured as QLabel measures
    // plain text, and not minimumSizeHint(): a label's hints include its
    // minimum, so once the total had been shown at the hero size its row kept
    // that height under any smaller font. The "Total:" label beside it keeps
    // the stylesheet's floor for the row.
    const QMargins margins = label->contentsMargins();
    const int flags = int(label->alignment()) & ~(Qt::AlignVCenter | Qt::AlignHCenter);
    const int height = label->fontMetrics().boundingRect(QRect(0, 0, 2000, 2000), flags, label->text()).height() +
                       margins.top() + margins.bottom() + 2 * label->margin();
    if (label->minimumHeight() != height) label->setMinimumHeight(height);
}

void OverviewPage::setBalance(const interfaces::WalletBalances& balances)
{
    int unit = walletModel->getOptionsModel()->getDisplayUnit();
    m_balances = balances;
    // The sky's level follows the spendable total; watch-only amounts do not
    // count, and neither does a wallet without private keys: its descriptors
    // report what they only watch as the wallet's own balance.
    const bool keyless = walletModel->wallet().privateKeysDisabled();
    m_sky_tier = SkyTiers::tierForBalance(keyless ? 0 : balances.balance + balances.unconfirmed_balance + balances.immature_balance, m_sky_tier);
    updateSkyBadge();
    updateNextLevel();
    updateCoinJoinCard();
    if (walletModel->wallet().isLegacy()) {
        if (walletModel->wallet().privateKeysDisabled()) {
            setBalanceText(ui->labelBalance, unit, balances.watch_only_balance);
            setBalanceText(ui->labelUnconfirmed, unit, balances.unconfirmed_watch_only_balance);
            setBalanceText(ui->labelImmature, unit, balances.immature_watch_only_balance);
            setBalanceText(ui->labelTotal, unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance);
        } else {
            setBalanceText(ui->labelBalance, unit, balances.balance);
            setBalanceText(ui->labelUnconfirmed, unit, balances.unconfirmed_balance);
            setBalanceText(ui->labelImmature, unit, balances.immature_balance);
            setBalanceText(ui->labelAnonymized, unit, balances.anonymized_balance + balances.dfcn_scale_anonymized_balance);
            setBalanceText(ui->labelTotal, unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance);
            setBalanceText(ui->labelWatchAvailable, unit, balances.watch_only_balance);
            setBalanceText(ui->labelWatchPending, unit, balances.unconfirmed_watch_only_balance);
            setBalanceText(ui->labelWatchImmature, unit, balances.immature_watch_only_balance);
            setBalanceText(ui->labelWatchTotal, unit, balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance);
        }
    } else {
            setBalanceText(ui->labelBalance, unit, balances.balance);
            setBalanceText(ui->labelUnconfirmed, unit, balances.unconfirmed_balance);
            setBalanceText(ui->labelImmature, unit, balances.immature_balance);
            setBalanceText(ui->labelAnonymized, unit, balances.anonymized_balance + balances.dfcn_scale_anonymized_balance);
            setBalanceText(ui->labelTotal, unit, balances.balance + balances.unconfirmed_balance + balances.immature_balance);
    }
    // only show immature (newly mined) balance if it's non-zero, so as not to complicate things
    // for the non-mining users
    bool fDebugUI = gArgs.GetBoolArg("-debug-ui", false);
    bool showImmature = fDebugUI || balances.immature_balance != 0;
    bool showWatchOnlyImmature = fDebugUI || balances.immature_watch_only_balance != 0;

    // for symmetry reasons also show immature label when the watch-only one is shown
    ui->labelImmature->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelImmatureText->setVisible(showImmature || showWatchOnlyImmature);
    ui->labelWatchImmature->setVisible(!walletModel->wallet().privateKeysDisabled() && showWatchOnlyImmature); // show watch-only immature balance

    updateCoinJoinProgress();

    // The amounts have just changed length, and in the modern theme their
    // length is what the card and the page are allowed to shrink to.
    scheduleBalanceWidths();

    int numISLocks = walletModel->getNumISLocks();
    if(cachedNumISLocks != numISLocks) {
        cachedNumISLocks = numISLocks;
        ui->listTransactions->update();
    }
}

// show/hide watch-only labels
void OverviewPage::updateWatchOnlyLabels(bool showWatchOnly)
{
    ui->labelSpendable->setVisible(showWatchOnly);      // show spendable label (only when watch-only is active)
    ui->labelWatchonly->setVisible(showWatchOnly);      // show watch-only label
    ui->lineWatchBalance->setVisible(showWatchOnly);    // show watch-only balance separator line
    ui->labelWatchAvailable->setVisible(showWatchOnly); // show watch-only available balance
    ui->labelWatchPending->setVisible(showWatchOnly);   // show watch-only pending balance
    ui->labelWatchTotal->setVisible(showWatchOnly);     // show watch-only total balance

    if (!showWatchOnly){
        ui->labelWatchImmature->hide();
    }
    else{
        ui->labelBalance->setIndent(20);
        ui->labelUnconfirmed->setIndent(20);
        ui->labelImmature->setIndent(20);
        ui->labelTotal->setIndent(20);
    }
    scheduleBalanceWidths();
}

void OverviewPage::setClientModel(ClientModel *model)
{
    m_network_model_context.reset();
    this->clientModel = model;
    if (model) {
        // Show warning, for example if this is a prerelease version
        connect(model, &ClientModel::alertsChanged, this, &OverviewPage::updateAlerts);
        updateAlerts(model->getStatusBarWarnings());

        // What the network bar shows, kept from the model's signals. The
        // mempool is polled every MODEL_UPDATE_DELAY, which also keeps the
        // block's age current between blocks.
        m_network_model_context = std::make_unique<QObject>();
        QObject* context = m_network_model_context.get();
        m_data_progress = 0;
        m_chainlock_height = -1;
        m_masternode_count.reset();
        m_mempool_count.reset();
        m_block_height = model->getNumBlocks();
        m_block_time = QDateTime::fromSecsSinceEpoch(model->node().getLastBlockTime());
        m_verification_progress = model->node().getVerificationProgress();
        m_peer_count = model->getNumConnections();
        const auto count_masternodes = [this, model] {
            const CDeterministicMNList list = model->getMasternodeList().first;
            // An empty list that was never set says nothing about the network.
            if (!list.GetBlockHash().IsNull()) m_masternode_count = list.GetValidMNsCount();
        };
        count_masternodes();
        connect(model, &ClientModel::numBlocksChanged, context, [this](int count, const QDateTime& date, const QString&, double progress, SyncType type, SynchronizationState) {
            if (type == SyncType::BLOCK_SYNC) {
                m_block_height = count;
                m_block_time = date;
                m_verification_progress = progress;
            }
            updateNetworkState();
        });
        connect(model, &ClientModel::additionalDataSyncProgressChanged, context, [this](double progress) {
            m_data_progress = progress;
            updateNetworkState();
        });
        connect(model, &ClientModel::chainLockChanged, context, [this](const QString&, int height) {
            m_chainlock_height = height;
            updateNetworkState();
        });
        connect(model, &ClientModel::numConnectionsChanged, context, [this](int count) {
            m_peer_count = count;
            updateNetworkState();
        });
        connect(model, &ClientModel::masternodeListChanged, context, [this, count_masternodes] {
            count_masternodes();
            updateNetworkState();
        });
        connect(model, &ClientModel::mempoolSizeChanged, context, [this](long count, size_t) {
            m_mempool_count = count;
            updateNetworkState();
        });
        updateNetworkState();
    }
}

void OverviewPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
    if (model == nullptr) {
        m_sky_tier = 0;
        updateSkyBadge();
        updateNextLevel();
    }
    if(model && model->getOptionsModel())
    {
        // update the display unit, to not use the default ("DASH")
        updateDisplayUnit();
        // Keep up to date with wallet
        interfaces::Wallet& wallet = model->wallet();
        interfaces::WalletBalances balances = wallet.getBalances();
        setBalance(balances);
        connect(model, &WalletModel::balanceChanged, this, &OverviewPage::setBalance);

        connect(model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &OverviewPage::updateDisplayUnit);

        updateWatchOnlyLabels((wallet.haveWatchOnly() && !model->wallet().privateKeysDisabled()) || gArgs.GetBoolArg("-debug-ui", false));
        // The page is the context of this lambda and of the CoinJoin one
        // below, so the connections go with the page. The options outlive a
        // closed wallet's page: without a context, switching CoinJoin after
        // the wallet was closed ran on the page that was gone.
        connect(model, &WalletModel::notifyWatchonlyChanged, this, [this](bool showWatchOnly) {
            updateWatchOnlyLabels(showWatchOnly && !walletModel->wallet().privateKeysDisabled());
        });

        // explicitly update PS frame and transaction list to reflect actual settings
        updateAdvancedCJUI(model->getOptionsModel()->getShowAdvancedCJUI());

        connect(model->getOptionsModel(), &OptionsModel::coinJoinRoundsChanged, this, &OverviewPage::updateCoinJoinProgress);
        connect(model->getOptionsModel(), &OptionsModel::coinJoinAmountChanged, this, &OverviewPage::updateCoinJoinProgress);
        connect(model->getOptionsModel(), &OptionsModel::AdvancedCJUIChanged, this, &OverviewPage::updateAdvancedCJUI);
        connect(model->getOptionsModel(), &OptionsModel::coinJoinEnabledChanged, this, [=]() {
            coinJoinStatus(true);
        });

        // Disable coinJoinClient builtin support for automatic backups while we are in GUI,
        // we'll handle automatic backups and user warnings in coinJoinStatus()
        walletModel->withCoinJoin([](auto& client) { client.disableAutobackups(); });

        connect(ui->toggleCoinJoin, &QPushButton::clicked, this, &OverviewPage::toggleCoinJoin);

        // coinjoin buttons will not react to spacebar must be clicked on
        ui->toggleCoinJoin->setFocusPolicy(Qt::NoFocus);
    }
}

void OverviewPage::updateDisplayUnit()
{
    if(walletModel && walletModel->getOptionsModel())
    {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        if (m_balances.balance != -1) {
            setBalance(m_balances);
        }

        // Update txdelegate->unit with the current unit
        txdelegate->unit = nDisplayUnit;

        ui->listTransactions->update();
    }
}

void OverviewPage::updateAlerts(const QString &warnings)
{
    this->ui->labelAlerts->setVisible(!warnings.isEmpty());
    this->ui->labelAlerts->setText(warnings);
}

void OverviewPage::showOutOfSyncWarning(bool fShow)
{
    ui->labelWalletStatus->setVisible(fShow);
    ui->labelCoinJoinSyncStatus->setVisible(fShow);
    ui->labelTransactionsStatus->setVisible(fShow);
    m_out_of_sync = fShow;
    updateEmptyState();
}

void OverviewPage::updateCoinJoinProgress()
{
    if (!walletModel || !clientModel || clientModel->node().shutdownRequested() || !clientModel->masternodeSync().isBlockchainSynced()) return;

    QString strAmountAndRounds;
    QString strCoinJoinAmount = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, clientModel->coinJoinOptions().getAmount() * COIN, false, BitcoinUnits::SeparatorStyle::ALWAYS);

    if(m_balances.balance == 0)
    {
        ui->coinJoinProgress->setValue(0);
        ui->coinJoinProgress->setToolTip(tr("No inputs detected"));

        // when balance is zero just show info from settings
        strCoinJoinAmount = strCoinJoinAmount.remove(strCoinJoinAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strCoinJoinAmount + " / " + tr("%n Rounds", "", clientModel->coinJoinOptions().getRounds());

        ui->labelAmountRounds->setToolTip(tr("No inputs detected"));
        ui->labelAmountRounds->setText(strAmountAndRounds);
        return;
    }

    CAmount nAnonymizableBalance = walletModel->wallet().getAnonymizableBalance(false, false);

    CAmount nMaxToAnonymize = nAnonymizableBalance + m_balances.anonymized_balance;

    // If it's more than the anon threshold, limit to that.
    if (nMaxToAnonymize > clientModel->coinJoinOptions().getAmount() * COIN) nMaxToAnonymize = clientModel->coinJoinOptions().getAmount() * COIN;

    if(nMaxToAnonymize == 0) return;

    if (m_privacy) {
        strAmountAndRounds = "#### " + BitcoinUnits::name(nDisplayUnit) + " / " + tr("%n Rounds", "", 0);
        ui->labelAmountRounds->setToolTip("");
    } else if (nMaxToAnonymize >= clientModel->coinJoinOptions().getAmount() * COIN) {
        ui->labelAmountRounds->setToolTip(tr("Found enough compatible inputs to mix %1")
                                          .arg(strCoinJoinAmount));
        strCoinJoinAmount = strCoinJoinAmount.remove(strCoinJoinAmount.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = strCoinJoinAmount + " / " + tr("%n Rounds", "", clientModel->coinJoinOptions().getRounds());
    } else {
        QString strMaxToAnonymize = BitcoinUnits::formatHtmlWithUnit(nDisplayUnit, nMaxToAnonymize, false, BitcoinUnits::SeparatorStyle::ALWAYS);
        ui->labelAmountRounds->setToolTip(tr("Not enough compatible inputs to mix <span style='%1'>%2</span>,<br>"
                                             "will mix <span style='%1'>%3</span> instead")
                                          .arg(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR))
                                          .arg(strCoinJoinAmount)
                                          .arg(strMaxToAnonymize));
        strMaxToAnonymize = strMaxToAnonymize.remove(strMaxToAnonymize.indexOf("."), BitcoinUnits::decimals(nDisplayUnit) + 1);
        strAmountAndRounds = "<span style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR) + "'>" +
                QString(BitcoinUnits::factor(nDisplayUnit) == 1 ? "" : "~") + strMaxToAnonymize +
                " / " + tr("%n Rounds", "", clientModel->coinJoinOptions().getRounds()) + "</span>";
    }
    ui->labelAmountRounds->setText(strAmountAndRounds);

    if (!fShowAdvancedCJUI) return;

    CAmount nDenominatedConfirmedBalance;
    CAmount nDenominatedUnconfirmedBalance;
    CAmount nNormalizedAnonymizedBalance;
    float nAverageAnonymizedRounds;

    nDenominatedConfirmedBalance = walletModel->wallet().getDenominatedBalance(false);
    nDenominatedUnconfirmedBalance = walletModel->wallet().getDenominatedBalance(true);
    nNormalizedAnonymizedBalance = walletModel->wallet().getNormalizedAnonymizedBalance();
    nAverageAnonymizedRounds = walletModel->wallet().getAverageAnonymizedRounds();

    // calculate parts of the progress, each of them shouldn't be higher than 1
    // progress of denominating
    float denomPart = 0;
    // mixing progress of denominated balance
    float anonNormPart = 0;
    // completeness of full amount anonymization
    float anonFullPart = 0;

    CAmount denominatedBalance = nDenominatedConfirmedBalance + nDenominatedUnconfirmedBalance;
    denomPart = (float)denominatedBalance / nMaxToAnonymize;
    denomPart = denomPart > 1 ? 1 : denomPart;
    denomPart *= 100;

    anonNormPart = (float)nNormalizedAnonymizedBalance / nMaxToAnonymize;
    anonNormPart = anonNormPart > 1 ? 1 : anonNormPart;
    anonNormPart *= 100;

    anonFullPart = (float)m_balances.anonymized_balance / nMaxToAnonymize;
    anonFullPart = anonFullPart > 1 ? 1 : anonFullPart;
    anonFullPart *= 100;

    // apply some weights to them ...
    float denomWeight = 1;
    float anonNormWeight = clientModel->coinJoinOptions().getRounds();
    float anonFullWeight = 2;
    float fullWeight = denomWeight + anonNormWeight + anonFullWeight;
    // ... and calculate the whole progress
    float denomPartCalc = ceilf((denomPart * denomWeight / fullWeight) * 100) / 100;
    float anonNormPartCalc = ceilf((anonNormPart * anonNormWeight / fullWeight) * 100) / 100;
    float anonFullPartCalc = ceilf((anonFullPart * anonFullWeight / fullWeight) * 100) / 100;
    float progress = denomPartCalc + anonNormPartCalc + anonFullPartCalc;
    if(progress >= 100) progress = 100;

    ui->coinJoinProgress->setValue(progress);

    QString strToolPip = QString("<b>" + tr("Overall progress") + ": %1%</b><br/>" +
                          tr("Denominated") + ": %2%<br/>" +
                          tr("Partially mixed") + ": %3%<br/>" +
                          tr("Mixed") + ": %4%<br/>" +
                          tr("Denominated inputs have %5 of %n rounds on average", "", clientModel->coinJoinOptions().getRounds()))
            .arg(progress).arg(denomPart).arg(anonNormPart).arg(anonFullPart)
            .arg(nAverageAnonymizedRounds);
    ui->coinJoinProgress->setToolTip(strToolPip);
}

void OverviewPage::updateAdvancedCJUI(bool fShowAdvancedCJUI)
{
    if (!walletModel || !clientModel) return;

    this->fShowAdvancedCJUI = fShowAdvancedCJUI;
    coinJoinStatus(true);
}

void OverviewPage::coinJoinStatus(bool fForce)
{
    if (!walletModel || !clientModel) return;

    if (!fForce && (clientModel->node().shutdownRequested() || !clientModel->masternodeSync().isBlockchainSynced())) return;

    // Disable any PS UI for masternode or when autobackup is disabled or failed for whatever reason
    if (clientModel->node().isMasternode() || nWalletBackups <= 0) {
        DisableCoinJoinCompletely();
        if (nWalletBackups <= 0) {
            ui->labelCoinJoinEnabled->setToolTip(tr("Automatic backups are disabled, no mixing available!"));
        }
        // Disabled is a state of its own for the card: Abyss keeps it behind
        // the row's link, the classic themes leave it as it stands.
        m_coinjoin_known = true;
        m_coinjoin_disabled = true;
        m_coinjoin_mixing = false;
        updateCoinJoinCard();
        return;
    }

    bool fIsEnabled = clientModel->coinJoinOptions().isEnabled();
    // Abyss keeps the card out of the way until it has something to say; the
    // classic themes show it whenever CoinJoin is on, as before.
    m_coinjoin_known = true;
    m_coinjoin_disabled = false;
    m_coinjoin_enabled = fIsEnabled;
    m_coinjoin_classic_card = fIsEnabled;
    m_coinjoin_mixing = false;
    if (fIsEnabled) walletModel->withCoinJoin([&](auto& client) { m_coinjoin_mixing = client.isMixing(); });
    updateCoinJoinCard();
    if (!fIsEnabled) {
        SetupTransactionList(NUM_ITEMS_DISABLED);
        if (timer != nullptr) {
            timer->stop();
        }
        return;
    }

    if (timer != nullptr && !timer->isActive()) {
        timer->start(1000);
    }

    // Wrap all coinjoin related widgets we want to show/hide state based.
    // Value of the map contains a flag if this widget belongs to the advanced
    // CoinJoin UI option or not. True if it does, false if not.
    std::map<QWidget*, bool> coinJoinWidgets = {
        {ui->labelCompletitionText, true},
        {ui->coinJoinProgress, true},
        {ui->labelSubmittedDenomText, true},
        {ui->labelSubmittedDenom, true},
        {ui->labelAmountAndRoundsText, false},
        {ui->labelAmountRounds, false}
    };

    auto setWidgetsVisible = [&](bool fVisible) {
        // Only update the widget's visibility if something changed since the last call of setWidgetsVisible
        if (m_cj_widgets_last_show_advanced == fShowAdvancedCJUI && m_cj_widgets_last_visible == fVisible) {
            if (m_cj_widgets_initial) {
                m_cj_widgets_initial = false;
            } else {
                return;
            }
        }
        // Set visible if: fVisible and not advanced UI element or advanced ui element and advanced ui active
        for (const auto& it : coinJoinWidgets) {
            it.first->setVisible(fVisible && (!it.second || it.second == fShowAdvancedCJUI));
        }
        m_cj_widgets_last_visible = fVisible;
        m_cj_widgets_last_show_advanced = fShowAdvancedCJUI;
    };

    int nBestHeight = clientModel->node().getNumBlocks();

    // We are processing more than 1 block per second, we'll just leave
    bool tooFast{false};
    walletModel->withCoinJoin([&](auto& client) {
        tooFast = nBestHeight > client.getCachedBlocks() && GetTime() - m_cj_last_progress_time <= 1;
    });
    if (tooFast) return;
    m_cj_last_progress_time = GetTime();

    QString strKeysLeftText(tr("keys left: %1").arg(walletModel->getKeysLeftSinceAutoBackup()));
    if(walletModel->getKeysLeftSinceAutoBackup() < COINJOIN_KEYS_THRESHOLD_WARNING) {
        strKeysLeftText = "<span style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR) + "'>" + strKeysLeftText + "</span>";
    }
    if (!walletModel->wallet().isLegacy()) {
        // we don't need in auto-back for case of Descriptor wallets
        strKeysLeftText = "";
    }
    ui->labelCoinJoinEnabled->setToolTip(strKeysLeftText);

    QString strCoinJoinName = QString::fromStdString(gCoinJoinName);
    bool notMixing{false};
    bool refreshProgress{false};
    walletModel->withCoinJoin([&](auto& client) {
        notMixing = !client.isMixing();
        if (notMixing && nBestHeight != client.getCachedBlocks()) {
            client.setCachedBlocks(nBestHeight);
            refreshProgress = true;
        }
    });
    if (refreshProgress) updateCoinJoinProgress();
    if (notMixing) {
        setWidgetsVisible(false);
        ui->toggleCoinJoin->setText(tr("Start %1").arg(strCoinJoinName));

        QString strEnabled = tr("Disabled");
        // Show how many keys left in advanced PS UI mode only
        if (fShowAdvancedCJUI && !strKeysLeftText.isEmpty()) strEnabled += ", " + strKeysLeftText;
        ui->labelCoinJoinEnabled->setText(strEnabled);

        // If mixing isn't active always show the lower number of txes because there are
        // anyway the most PS widgets hidden.
        SetupTransactionList(NUM_ITEMS_ENABLED_NORMAL);

        return;
    } else {
        SetupTransactionList(fShowAdvancedCJUI ? NUM_ITEMS_ENABLED_ADVANCED : NUM_ITEMS_ENABLED_NORMAL);
    }

    // Warn user that wallet is running out of keys
    // NOTE: we do NOT warn user and do NOT create autobackups if mixing is not running
    if (walletModel->wallet().isLegacy() && nWalletBackups > 0 && walletModel->getKeysLeftSinceAutoBackup() < COINJOIN_KEYS_THRESHOLD_WARNING) {
        QSettings settings;
        if(settings.value("fLowKeysWarning").toBool()) {
            QString strWarn =   tr("Very low number of keys left since last automatic backup!") + "<br><br>" +
                                tr("We are about to create a new automatic backup for you, however "
                                   "<span style='%1'> you should always make sure you have backups "
                                   "saved in some safe place</span>!").arg(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_COMMAND)) + "<br><br>" +
                                tr("Note: You can turn this message off in options.");
            ui->labelCoinJoinEnabled->setToolTip(strWarn);
            LogPrint(BCLog::COINJOIN, "OverviewPage::coinJoinStatus -- Very low number of keys left since last automatic backup, warning user and trying to create new backup...\n");
            QMessageBox::warning(this, strCoinJoinName, strWarn, QMessageBox::Ok, QMessageBox::Ok);
        } else {
            LogPrint(BCLog::COINJOIN, "OverviewPage::coinJoinStatus -- Very low number of keys left since last automatic backup, skipping warning and trying to create new backup...\n");
        }

        QString strBackupWarning;
        QString strBackupError;
        if(!walletModel->autoBackupWallet(strBackupWarning, strBackupError)) {
            if (!strBackupWarning.isEmpty()) {
                // It's still more or less safe to continue but warn user anyway
                LogPrint(BCLog::COINJOIN, "OverviewPage::coinJoinStatus -- WARNING! Something went wrong on automatic backup: %s\n", strBackupWarning.toStdString());

                QMessageBox::warning(this, strCoinJoinName,
                    tr("WARNING! Something went wrong on automatic backup") + ":<br><br>" + strBackupWarning,
                    QMessageBox::Ok, QMessageBox::Ok);
            }
            if (!strBackupError.isEmpty()) {
                // Things are really broken, warn user and stop mixing immediately
                LogPrint(BCLog::COINJOIN, "OverviewPage::coinJoinStatus -- ERROR! Failed to create automatic backup: %s\n", strBackupError.toStdString());

                QMessageBox::warning(this, strCoinJoinName,
                    tr("ERROR! Failed to create automatic backup") + ":<br><br>" + strBackupError + "<br>" +
                    tr("Mixing is disabled, please close your wallet and fix the issue!"),
                    QMessageBox::Ok, QMessageBox::Ok);
            }
        }
    }

    QString strEnabled;
    walletModel->withCoinJoin([&](auto& client) { strEnabled = client.isMixing() ? tr("Enabled") : tr("Disabled"); });
    // Show how many keys left in advanced PS UI mode only
    if(fShowAdvancedCJUI && !strKeysLeftText.isEmpty()) strEnabled += ", " + strKeysLeftText;
    ui->labelCoinJoinEnabled->setText(strEnabled);

    if (walletModel->wallet().isLegacy()) {
        if(nWalletBackups == -1) {
            // Automatic backup failed, nothing else we can do until user fixes the issue manually
            DisableCoinJoinCompletely();

            QString strError =  tr("ERROR! Failed to create automatic backup") + ", " +
                                tr("see debug.log for details.") + "<br><br>" +
                                tr("Mixing is disabled, please close your wallet and fix the issue!");
            ui->labelCoinJoinEnabled->setToolTip(strError);

            return;
        } else if(nWalletBackups == -2) {
            // We were able to create automatic backup but keypool was not replenished because wallet is locked.
            QString strWarning = tr("WARNING! Failed to replenish keypool, please unlock your wallet to do so.");
            ui->labelCoinJoinEnabled->setToolTip(strWarning);
        }
    }

    // check coinjoin status and unlock if needed
    bool refreshProgressTail{false};
    walletModel->withCoinJoin([&](auto& client) {
        if (nBestHeight != client.getCachedBlocks()) {
            // Balance and number of transactions might have changed
            client.setCachedBlocks(nBestHeight);
            refreshProgressTail = true;
        }
        ui->labelSubmittedDenom->setText(m_privacy ? "####" : QString(client.getSessionDenoms().c_str()));
    });
    if (refreshProgressTail) updateCoinJoinProgress();

    setWidgetsVisible(true);
}

void OverviewPage::toggleCoinJoin(){
    QSettings settings;
    // Popup some information on first mixing
    QString hasMixed = settings.value("hasMixed").toString();
    QString strCoinJoinName = QString::fromStdString(gCoinJoinName);
    if(hasMixed.isEmpty()){
        QMessageBox::information(this, strCoinJoinName,
                tr("If you don't want to see internal %1 fees/transactions select \"Most Common\" as Type on the \"Transactions\" tab.").arg(strCoinJoinName),
                QMessageBox::Ok, QMessageBox::Ok);
        settings.setValue("hasMixed", "hasMixed");
    }

    bool mixing{false};
    walletModel->withCoinJoin([&](auto& client) { mixing = client.isMixing(); });
    if (!mixing) {
        auto& options = walletModel->node().coinJoinOptions();
        const CAmount nMinAmount = options.getSmallestDenomination() + options.getMaxCollateralAmount();
        if(m_balances.balance < nMinAmount) {
            QString strMinAmount(BitcoinUnits::formatWithUnit(nDisplayUnit, nMinAmount));
            QMessageBox::warning(this, strCoinJoinName,
                tr("%1 requires at least %2 to use.").arg(strCoinJoinName).arg(strMinAmount),
                QMessageBox::Ok, QMessageBox::Ok);
            return;
        }

        // if wallet is locked, ask for a passphrase
        if (walletModel->getEncryptionStatus() == WalletModel::Locked)
        {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock(true));
            if(!ctx.isValid())
            {
                //unlock was cancelled
                walletModel->withCoinJoin([](auto& client) { client.resetCachedBlocks(); });
                QMessageBox::warning(this, strCoinJoinName,
                    tr("Wallet is locked and user declined to unlock. Disabling %1.").arg(strCoinJoinName),
                    QMessageBox::Ok, QMessageBox::Ok);
                LogPrint(BCLog::COINJOIN, "OverviewPage::toggleCoinJoin -- Wallet is locked and user declined to unlock. Disabling CoinJoin.\n");
                return;
            }
        }

    }

    walletModel->withCoinJoin([&](auto& client) {
        client.resetCachedBlocks();
        if (client.isMixing()) {
            ui->toggleCoinJoin->setText(tr("Start %1").arg(strCoinJoinName));
            client.resetPool();
            client.stopMixing();
        } else {
            ui->toggleCoinJoin->setText(tr("Stop %1").arg(strCoinJoinName));
            client.startMixing();
        }
    });
}

void OverviewPage::SetupTransactionList(int nNumItems)
{
    if (walletModel == nullptr || walletModel->getTransactionTableModel() == nullptr) {
        return;
    }

    // Set up transaction list
    if (filter == nullptr) {
        filter.reset(new TransactionFilterProxy());
        filter->setSourceModel(walletModel->getTransactionTableModel());
        filter->setDynamicSortFilter(true);
        filter->setSortRole(Qt::EditRole);
        filter->setShowInactive(false);
        filter->sort(TransactionTableModel::Date, Qt::DescendingOrder);
        ui->listTransactions->setModel(filter.get());
        connect(filter.get(), &QAbstractItemModel::rowsInserted, this, &OverviewPage::updateEmptyState);
        connect(filter.get(), &QAbstractItemModel::rowsRemoved, this, &OverviewPage::updateEmptyState);
        connect(filter.get(), &QAbstractItemModel::modelReset, this, &OverviewPage::updateEmptyState);
        connect(filter.get(), &QAbstractItemModel::layoutChanged, this, &OverviewPage::updateEmptyState);
        updateEmptyState();
    }

    if (filter->rowCount() == nNumItems) {
        return;
    }

    filter->setLimit(nNumItems);
    m_transaction_list_items = nNumItems;
    ui->listTransactions->setMinimumHeight(transactionListMinimum(nNumItems));
    updateEmptyState();
}

//! In Abyss the recent transactions card takes the row the left column sets
//! (updateColumnStretch), and its list shows as many of the transactions as
//! fit there; it does not hold the row open for all of them, so that a short
//! left column (a locked wallet's staking card, under its banner) makes the
//! page shorter. Four rows stay its floor, which an empty list's message
//! raises to what it needs (EmptyStateWidget::fitList). The classic themes
//! keep the room for every row.
int OverviewPage::transactionListMinimum(int items) const
{
    constexpr int MODERN_LIST_MINIMUM_ROWS{4};
    return (GUIUtil::isModernTheme() ? std::min(items, MODERN_LIST_MINIMUM_ROWS) : items) * ITEM_HEIGHT;
}

void OverviewPage::DisableCoinJoinCompletely()
{
    if (walletModel == nullptr) {
        return;
    }

    ui->toggleCoinJoin->setText("(" + tr("Disabled") + ")");
    ui->frameCoinJoin->setEnabled(false);
    if (nWalletBackups <= 0) {
        ui->labelCoinJoinEnabled->setText("<span style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR) + "'>(" + tr("Disabled") + ")</span>");
    }
    walletModel->withCoinJoin([](auto& client) { client.stopMixing(); });
}
