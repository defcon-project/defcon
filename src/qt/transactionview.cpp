// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/transactionview.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/csvmodelwriter.h>
#include <qt/editaddressdialog.h>
#include <qt/emptystatewidget.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/qrdialog.h>
#include <qt/transactiondescdialog.h>
#include <qt/transactionfilterproxy.h>
#include <qt/transactionrecord.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <interfaces/node.h>
#include <node/interface_ui.h>

#include <optional>

#include <QApplication>
#include <QCalendarWidget>
#include <chrono>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDoubleValidator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPoint>
#include <QScrollBar>
#include <QSettings>
#include <QStyleOptionComboBox>
#include <QStyleOptionViewItem>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTextCharFormat>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

/** Date format for persistence */
static const char* PERSISTENCE_DATE_FORMAT = "yyyy-MM-dd";

namespace {
/** A list of the Transactions filter row. The classic themes show it as any
 *  combo box (the watch-only filter as a narrow list of icons): Rule::Plain.
 *  In Abyss a list either gives way where the row is short of room (a narrow
 *  window), down to its least text whole beside its icon and arrow (an empty
 *  least text: the icon alone), its current text shortened to what is left
 *  (Rule::GivesWay); or it is always as wide as its widest choice and never
 *  narrower than a floor (Rule::Whole). An open list keeps its texts whole. */
class FilterBox : public QComboBox
{
public:
    enum class Rule { Plain, GivesWay, Whole };
    using QComboBox::QComboBox;

    void setRule(Rule rule, const QString& least_text = QString(), int floor = 0)
    {
        if (m_rule == rule && m_least_text == least_text && m_floor == floor) return;
        m_rule = rule;
        m_least_text = least_text;
        m_floor = floor;
        updateGeometry();
        update();
    }
    //! With the text of the widest choice. QComboBox counts four pixels
    //! between an icon and the text and none after the text; a style sheet's
    //! label leaves LABEL_GAP after the icon, the text ends END_CLEAR before
    //! the field does, and two more allow for a text's advance past its
    //! measured width.
    QSize sizeHint() const override
    {
        QSize size = QComboBox::sizeHint();
        if (m_rule == Rule::Plain) return size;
        size.rwidth() += slack();
        if (m_rule == Rule::Whole) size.setWidth(std::max(size.width(), m_floor));
        return size;
    }
    //! GivesWay: QComboBox's own sum (an icon and four pixels, the text) for
    //! the least text. Whole: the full width.
    QSize minimumSizeHint() const override
    {
        if (m_rule == Rule::Plain) return QComboBox::minimumSizeHint();
        if (m_rule == Rule::Whole) return sizeHint();
        QSize size = QComboBox::minimumSizeHint();
        int contents = m_least_text.isEmpty() ? 0 : fontMetrics().horizontalAdvance(m_least_text);
        if (hasIcons()) contents += iconSize().width() + 4;
        QStyleOptionComboBox option;
        initStyleOption(&option);
        int width = style()->sizeFromContents(QStyle::CT_ComboBox, &option, QSize(contents, size.height()), this).width();
        if (!m_least_text.isEmpty()) width += slack();
        size.setWidth(std::min(width, sizeHint().width()));
        return size;
    }
    void showPopup() override
    {
        // A shortened list's popup is as wide as the box would be with room.
        const int full = std::min(sizeHint().width(), maximumWidth());
        view()->setMinimumWidth(m_rule == Rule::GivesWay && width() < full ? full : 0);
        QComboBox::showPopup();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        // QComboBox::paintEvent(), with the text shortened to its room.
        QStylePainter painter(this);
        painter.setPen(palette().color(QPalette::Text));
        QStyleOptionComboBox option;
        initStyleOption(&option);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        if (m_rule == Rule::GivesWay && !option.currentText.isEmpty()) {
            int room = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this).width() - END_CLEAR;
            if (!option.currentIcon.isNull()) room -= option.iconSize.width() + LABEL_GAP;
            if (option.fontMetrics.horizontalAdvance(option.currentText) > room) {
                option.currentText = room > 0 ? option.fontMetrics.elidedText(option.currentText, Qt::ElideRight, room) : QString();
            }
        }
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }

private:
    //! What a style sheet's combo box label leaves between the icon and the text.
    static constexpr int LABEL_GAP{6};
    //! The text ends this far before the field that clips it.
    static constexpr int END_CLEAR{2};
    Rule m_rule{Rule::Plain};
    QString m_least_text;
    int m_floor{0};
    bool hasIcons() const
    {
        for (int i = 0; i < count(); ++i) {
            if (!itemIcon(i).isNull()) return true;
        }
        return false;
    }
    int slack() const { return (hasIcons() ? LABEL_GAP - 4 : 0) + END_CLEAR + 2; }
};

/** The search field. In Abyss it asks for no more than its least width, so
 *  that where the row is short of room it gives way before the filter lists
 *  shorten their words (it still takes all the room left: it expands). The
 *  classic themes ask what a line edit asks. */
class SearchField : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

    void setModest(bool modest)
    {
        if (m_modest == modest) return;
        m_modest = modest;
        updateGeometry();
    }
    QSize sizeHint() const override
    {
        QSize size = QLineEdit::sizeHint();
        if (m_modest) size.setWidth(std::max(minimumSizeHint().width(), minimumWidth()));
        return size;
    }

private:
    bool m_modest{false};
};

/** The table's watch-only column in Abyss: the eye in the middle of its cell,
 *  whatever width the header gives the column (its least width follows the
 *  header's font). The classic themes paint the cell as the view always did. */
class WatchOnlyCellDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (!GUIUtil::isModernTheme()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem cell = option;
        initStyleOption(&cell, index);
        const QIcon eye = cell.icon;
        // The cell without its icon (the row's ground and selection), then
        // the icon in its middle, in the state the style would give it.
        cell.icon = QIcon();
        cell.features &= ~QStyleOptionViewItem::HasDecoration;
        QStyle* style = cell.widget != nullptr ? cell.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &cell, painter, cell.widget);
        if (eye.isNull()) return;
        QIcon::Mode mode = QIcon::Normal;
        if (!(cell.state & QStyle::State_Enabled)) {
            mode = QIcon::Disabled;
        } else if (cell.state & QStyle::State_Selected) {
            mode = QIcon::Selected;
        }
        eye.paint(painter, QStyle::alignedRect(cell.direction, Qt::AlignCenter, cell.decorationSize, cell.rect), Qt::AlignCenter, mode,
                  cell.state & QStyle::State_Open ? QIcon::On : QIcon::Off);
    }
};
} // namespace

TransactionView::TransactionView(QWidget* parent) :
    QWidget(parent)
{
    QSettings settings;
    // Build filter row
    setContentsMargins(0,0,0,0);

    QHBoxLayout *hlayout = new QHBoxLayout();
    hlayout->setContentsMargins(0,0,0,0);
    hlayout->setSpacing(1);
    hlayout->addSpacing(STATUS_COLUMN_WIDTH - 2);
    m_filter_row = hlayout;
    m_filter_status_gap = hlayout->itemAt(0)->spacerItem();

    watchOnlyWidget = new FilterBox(this);
    watchOnlyWidget->setFixedWidth(24);
    watchOnlyWidget->addItem("", TransactionFilterProxy::WatchOnlyFilter_All);
    watchOnlyWidget->addItem(GUIUtil::getIcon("eye_plus"), "", TransactionFilterProxy::WatchOnlyFilter_Yes);
    watchOnlyWidget->addItem(GUIUtil::getIcon("eye_minus"), "", TransactionFilterProxy::WatchOnlyFilter_No);
    for (int i = 0; i < 3; ++i) m_watch_classic_icons[i] = watchOnlyWidget->itemIcon(i);
    hlayout->addWidget(watchOnlyWidget);

    dateWidget = new FilterBox(this);
    dateWidget->setFixedWidth(DATE_COLUMN_WIDTH);
    dateWidget->addItem(tr("All"), All);
    dateWidget->addItem(tr("Today"), Today);
    dateWidget->addItem(tr("This week"), ThisWeek);
    dateWidget->addItem(tr("This month"), ThisMonth);
    dateWidget->addItem(tr("Last month"), LastMonth);
    dateWidget->addItem(tr("This year"), ThisYear);
    dateWidget->addItem(tr("Range…"), Range);
    dateWidget->setCurrentIndex(settings.value("transactionDate").toInt());
    hlayout->addWidget(dateWidget);

    QString strCoinJoinName = QString::fromStdString(gCoinJoinName);
    typeWidget = new FilterBox(this);
    typeWidget->setFixedWidth(TYPE_COLUMN_WIDTH - 1);
    typeWidget->addItem(tr("All"), TransactionFilterProxy::ALL_TYPES);
    typeWidget->addItem(tr("Most Common"), TransactionFilterProxy::COMMON_TYPES);
    typeWidget->addItem(tr("Received with"), TransactionFilterProxy::TYPE(TransactionRecord::RecvWithAddress) |
                                        TransactionFilterProxy::TYPE(TransactionRecord::RecvFromOther));
    typeWidget->addItem(tr("Sent to"), TransactionFilterProxy::TYPE(TransactionRecord::SendToAddress) |
                                  TransactionFilterProxy::TYPE(TransactionRecord::SendToOther));
    typeWidget->addItem(tr("%1 Send").arg(strCoinJoinName), TransactionFilterProxy::TYPE(TransactionRecord::CoinJoinSend));
    typeWidget->addItem(tr("%1 Make Collateral Inputs").arg(strCoinJoinName), TransactionFilterProxy::TYPE(TransactionRecord::CoinJoinMakeCollaterals));
    typeWidget->addItem(tr("%1 Create Denominations").arg(strCoinJoinName), TransactionFilterProxy::TYPE(TransactionRecord::CoinJoinCreateDenominations));
    typeWidget->addItem(tr("%1 Mixing").arg(strCoinJoinName), TransactionFilterProxy::TYPE(TransactionRecord::CoinJoinMixing));
    typeWidget->addItem(tr("%1 Collateral Payment").arg(strCoinJoinName), TransactionFilterProxy::TYPE(TransactionRecord::CoinJoinCollateralPayment));
    typeWidget->addItem(tr("To yourself"), TransactionFilterProxy::TYPE(TransactionRecord::SendToSelf));
    // No "Mined": the coin is not mined. A wallet's rows of that type (the
    // first blocks' rewards) stay in the list under their own name.
    typeWidget->addItem(tr("Platform Transfer"), TransactionFilterProxy::TYPE(TransactionRecord::PlatformTransfer));
    typeWidget->addItem(tr("Masternode Reward"), TransactionFilterProxy::TYPE(TransactionRecord::MasternodeReward));
    typeWidget->addItem(tr("Other"), TransactionFilterProxy::TYPE(TransactionRecord::Other));
    typeWidget->addItem(tr("Staked"), TransactionFilterProxy::TYPE(TransactionRecord::Staked));
    typeWidget->setCurrentIndex(settings.value("transactionType").toInt());

    hlayout->addWidget(typeWidget);

    search_widget = new SearchField(this);
    search_widget->setPlaceholderText(tr("Enter address, transaction id, or label to search"));
    search_widget->setObjectName("search_widget");
    hlayout->addWidget(search_widget);

    amountWidget = new QLineEdit(this);
    amountWidget->setPlaceholderText(tr("Min amount"));
    amountWidget->setFixedWidth(125);
    QDoubleValidator *amountValidator = new QDoubleValidator(0, 1e20, 8, this);
    QLocale amountLocale(QLocale::C);
    amountLocale.setNumberOptions(QLocale::RejectGroupSeparator);
    amountValidator->setLocale(amountLocale);
    amountWidget->setValidator(amountValidator);
    amountWidget->setObjectName("amountWidget");
    hlayout->addWidget(amountWidget);

    // Delay before filtering transactions
    static constexpr auto input_filter_delay{200ms};

    QTimer* amount_typing_delay = new QTimer(this);
    amount_typing_delay->setSingleShot(true);
    amount_typing_delay->setInterval(input_filter_delay);

    QTimer* prefix_typing_delay = new QTimer(this);
    prefix_typing_delay->setSingleShot(true);
    prefix_typing_delay->setInterval(input_filter_delay);

    QVBoxLayout *vlayout = new QVBoxLayout(this);
    vlayout->setContentsMargins(0,0,0,0);
    vlayout->setSpacing(0);

    transactionView = new QTableView(this);
    vlayout->addLayout(hlayout);
    vlayout->addWidget(createDateRangeWidget());
    vlayout->addWidget(transactionView);
    vlayout->setSpacing(0);
#ifndef Q_OS_MAC
    int width = transactionView->verticalScrollBar()->sizeHint().width();
    // Cover scroll bar width with spacing
    hlayout->addSpacing(width);
    m_filter_scrollbar_gap = hlayout->itemAt(hlayout->count() - 1)->spacerItem();
    m_filter_scrollbar_width = width;
    // Always show scroll bar
    transactionView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
#endif
    transactionView->setTabKeyNavigation(false);
    transactionView->setContextMenuPolicy(Qt::CustomContextMenu);

    transactionView->installEventFilter(this);

    transactionView->setObjectName("transactionView");
    transactionView->setItemDelegateForColumn(TransactionTableModel::Watchonly, new WatchOnlyCellDelegate(transactionView));

    // Abyss says what an empty history means: nothing yet, nothing that
    // matches the filters, or waiting for the sync. The header stays visible
    // above.
    m_empty_state = new EmptyStateWidget("history", tr("No transactions yet"),
                                         tr("Your incoming and outgoing transactions will appear here."), tr("Receive"), transactionView);
    m_empty_state->coverViewport(transactionView);
    connect(m_empty_state, &EmptyStateWidget::actionClicked, this, &TransactionView::receiveClicked);
    connect(m_empty_state, &EmptyStateWidget::clearFiltersClicked, this, &TransactionView::clearFilters);
    connect(m_empty_state, &EmptyStateWidget::listShown, this, &TransactionView::updateEmptyState);

    contextMenu = new QMenu(this);
    contextMenu->setObjectName("contextMenu");
    copyAddressAction = contextMenu->addAction(tr("&Copy address"), this, &TransactionView::copyAddress);
    copyLabelAction = contextMenu->addAction(tr("Copy &label"), this, &TransactionView::copyLabel);
    contextMenu->addAction(tr("Copy &amount"), this, &TransactionView::copyAmount);
    contextMenu->addAction(tr("Copy transaction &ID"), this, &TransactionView::copyTxID);
    contextMenu->addAction(tr("Copy &raw transaction"), this, &TransactionView::copyTxHex);
    contextMenu->addAction(tr("Copy full transaction &details"), this, &TransactionView::copyTxPlainText);
    contextMenu->addAction(tr("&Show transaction details"), this, &TransactionView::showDetails);
    contextMenu->addSeparator();
    abandonAction = contextMenu->addAction(tr("A&bandon transaction"), this, &TransactionView::abandonTx);
    resendAction = contextMenu->addAction(tr("Rese&nd transaction"), this, &TransactionView::resendTx);
    contextMenu->addAction(tr("&Edit address label"), this, &TransactionView::editLabel);
    [[maybe_unused]] QAction* showAddressQRCodeAction = contextMenu->addAction(tr("Show address &QR code"), this, &TransactionView::showAddressQRCode);
#ifndef USE_QRCODE
    showAddressQRCodeAction->setEnabled(false);
#endif

    connect(dateWidget, qOverload<int>(&QComboBox::activated), this, &TransactionView::chooseDate);
    connect(typeWidget, qOverload<int>(&QComboBox::activated), this, &TransactionView::chooseType);
    connect(watchOnlyWidget, qOverload<int>(&QComboBox::activated), this, &TransactionView::chooseWatchonly);
    connect(amountWidget, &QLineEdit::textChanged, amount_typing_delay, qOverload<>(&QTimer::start));
    connect(amount_typing_delay, &QTimer::timeout, this, &TransactionView::changedAmount);
    connect(search_widget, &QLineEdit::textChanged, prefix_typing_delay, qOverload<>(&QTimer::start));
    connect(prefix_typing_delay, &QTimer::timeout, this, &TransactionView::changedSearch);

    connect(transactionView, &QTableView::doubleClicked, this, &TransactionView::doubleClicked);
    connect(transactionView, &QTableView::clicked, this, &TransactionView::computeSum);
    connect(transactionView, &QTableView::customContextMenuRequested, this, &TransactionView::contextualMenu);

    // Double-clicking on a transaction on the transaction history page shows details
    connect(this, &TransactionView::doubleClicked, this, &TransactionView::showDetails);

    updateFilterPresentation();
}

void TransactionView::setModel(WalletModel *_model)
{
    QSettings settings;
    this->model = _model;
    if(_model)
    {
        transactionProxyModel = new TransactionFilterProxy(this);
        transactionProxyModel->setSourceModel(_model->getTransactionTableModel());
        transactionProxyModel->setDynamicSortFilter(true);
        transactionProxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
        transactionProxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

        transactionProxyModel->setSortRole(Qt::EditRole);

        transactionView->setModel(transactionProxyModel);
        // Rows the filters hide come and go without the proxy saying so, but
        // they decide between an empty and a filtered history.
        const auto watch_rows = [this](QAbstractItemModel* rows) {
            connect(rows, &QAbstractItemModel::rowsInserted, this, &TransactionView::updateEmptyState);
            connect(rows, &QAbstractItemModel::rowsRemoved, this, &TransactionView::updateEmptyState);
            connect(rows, &QAbstractItemModel::modelReset, this, &TransactionView::updateEmptyState);
            connect(rows, &QAbstractItemModel::layoutChanged, this, &TransactionView::updateEmptyState);
        };
        watch_rows(transactionProxyModel);
        watch_rows(_model->getTransactionTableModel());
        transactionView->setAlternatingRowColors(true);
        transactionView->setSelectionBehavior(QAbstractItemView::SelectRows);
        transactionView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        transactionView->horizontalHeader()->setSortIndicator(TransactionTableModel::Date, Qt::DescendingOrder);
        transactionView->setSortingEnabled(true);
        transactionView->verticalHeader()->hide();

        transactionView->setColumnWidth(TransactionTableModel::Status, STATUS_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Watchonly, WATCHONLY_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Date, DATE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Type, TYPE_COLUMN_WIDTH);
        transactionView->setColumnWidth(TransactionTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);

        // Note: it's a good idea to connect this signal AFTER the model is set
        connect(transactionView->selectionModel(), &QItemSelectionModel::selectionChanged, this, &TransactionView::computeSum);

        transactionView->horizontalHeader()->setMinimumSectionSize(MINIMUM_COLUMN_WIDTH);
        transactionView->horizontalHeader()->setSectionResizeMode(TransactionTableModel::Status, QHeaderView::Fixed);
        transactionView->horizontalHeader()->setSectionResizeMode(TransactionTableModel::Watchonly, QHeaderView::Fixed);
        transactionView->horizontalHeader()->setSectionResizeMode(TransactionTableModel::Date, QHeaderView::Interactive);
        transactionView->horizontalHeader()->setSectionResizeMode(TransactionTableModel::Type, QHeaderView::Interactive);
        transactionView->horizontalHeader()->setSectionResizeMode(TransactionTableModel::ToAddress, QHeaderView::Stretch);
        transactionView->horizontalHeader()->setSectionResizeMode(TransactionTableModel::Amount, QHeaderView::Stretch);

        if (_model->getOptionsModel())
        {
            // Add third party transaction URLs to context menu
            QStringList listUrls = GUIUtil::SplitSkipEmptyParts(_model->getOptionsModel()->getThirdPartyTxUrls(), "|");
            for (int i = 0; i < listUrls.size(); ++i)
            {
                QString url = listUrls[i].trimmed();
                QString host = QUrl(url, QUrl::StrictMode).host();
                if (!host.isEmpty())
                {
                    QAction *thirdPartyTxUrlAction = new QAction(host, this); // use host as menu item label
                    if (i == 0)
                        contextMenu->addSeparator();
                    contextMenu->addAction(thirdPartyTxUrlAction);
                    connect(thirdPartyTxUrlAction, &QAction::triggered, [this, url] { openThirdPartyTxUrl(url); });
                }
            }

            connect(_model->getOptionsModel(), &OptionsModel::coinJoinEnabledChanged, this, &TransactionView::updateCoinJoinVisibility);
        }

        // show/hide column Watch-only
        updateWatchOnlyColumn(_model->wallet().haveWatchOnly());

        // Watch-only signal
        connect(_model, &WalletModel::notifyWatchonlyChanged, this, &TransactionView::updateWatchOnlyColumn);

        // Update transaction list with persisted settings
        chooseType(settings.value("transactionType").toInt());
        chooseDate(settings.value("transactionDate").toInt());

        updateCoinJoinVisibility();
    }
    updateEmptyState();
}

void TransactionView::setOutOfSync(bool out_of_sync)
{
    m_out_of_sync = out_of_sync;
    updateEmptyState();
}

void TransactionView::clearFilters()
{
    watchOnlyWidget->setCurrentIndex(0);
    chooseWatchonly(0);
    dateWidget->setCurrentIndex(0);
    chooseDate(0);
    typeWidget->setCurrentIndex(0);
    chooseType(0);
    // Apply the cleared text now rather than after the typing delay.
    search_widget->clear();
    changedSearch();
    amountWidget->clear();
    changedAmount();
}

void TransactionView::updateEmptyState()
{
    if (m_empty_state == nullptr) return;
    if (!transactionProxyModel || !model) {
        m_empty_state->setState(EmptyStateWidget::State::Hidden);
        return;
    }
    m_empty_state->setState(EmptyStateWidget::stateFor(GUIUtil::isModernTheme(), transactionProxyModel->rowCount(),
                                                      model->getTransactionTableModel()->rowCount({}), m_out_of_sync));
}

void TransactionView::chooseDate(int idx)
{
    if (!transactionProxyModel) return;

    QSettings settings;
    QDate current = QDate::currentDate();
    dateRangeWidget->setVisible(false);
    switch(dateWidget->itemData(idx).toInt())
    {
    case All:
        transactionProxyModel->setDateRange(
                std::nullopt,
                std::nullopt);
        break;
    case Today:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(current),
                std::nullopt);
        break;
    case ThisWeek: {
        // Find last Monday
        QDate startOfWeek = current.addDays(-(current.dayOfWeek()-1));
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(startOfWeek),
                std::nullopt);

        } break;
    case ThisMonth:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1)),
                std::nullopt);
        break;
    case LastMonth:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1).addMonths(-1)),
                GUIUtil::StartOfDay(QDate(current.year(), current.month(), 1)));
        break;
    case ThisYear:
        transactionProxyModel->setDateRange(
                GUIUtil::StartOfDay(QDate(current.year(), 1, 1)),
                std::nullopt);
        break;
    case Range:
        dateRangeWidget->setVisible(true);
        dateRangeChanged();
        break;
    }
    // Persist new date settings
    settings.setValue("transactionDate", idx);
    if (dateWidget->itemData(idx).toInt() == Range){
        settings.setValue("transactionDateFrom", dateFrom->date().toString(PERSISTENCE_DATE_FORMAT));
        settings.setValue("transactionDateTo", dateTo->date().toString(PERSISTENCE_DATE_FORMAT));
    }
}

void TransactionView::chooseType(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setTypeFilter(
        typeWidget->itemData(idx).toUInt());
    // Persist settings
    QSettings settings;
    settings.setValue("transactionType", idx);
}

void TransactionView::chooseWatchonly(int idx)
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setWatchOnlyFilter(
        static_cast<TransactionFilterProxy::WatchOnlyFilter>(watchOnlyWidget->itemData(idx).toInt()));
}

void TransactionView::changedSearch()
{
    if(!transactionProxyModel)
        return;
    transactionProxyModel->setSearchString(search_widget->text());
}

void TransactionView::changedAmount()
{
    if(!transactionProxyModel)
        return;
    CAmount amount_parsed = 0;

    // Replace "," by "." so BitcoinUnits::parse will not fail for users entering "," as decimal separator
    QString newAmount = amountWidget->text();
    newAmount.replace(QString(","), QString("."));

    if (BitcoinUnits::parse(model->getOptionsModel()->getDisplayUnit(), newAmount, &amount_parsed)) {
        transactionProxyModel->setMinAmount(amount_parsed);
    }
    else
    {
        transactionProxyModel->setMinAmount(0);
    }
}

void TransactionView::exportClicked()
{
    if (!model || !model->getOptionsModel()) {
        return;
    }

    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(this,
        tr("Export Transaction History"), QString(),
        /*: Expanded name of the CSV file format.
            See: https://en.wikipedia.org/wiki/Comma-separated_values. */
        tr("Comma separated file") + QLatin1String(" (*.csv)"), nullptr);

    if (filename.isNull())
        return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(transactionProxyModel);
    writer.addColumn(tr("Confirmed"), 0, TransactionTableModel::ConfirmedRole);
    if (model->wallet().haveWatchOnly())
        writer.addColumn(tr("Watch-only"), TransactionTableModel::Watchonly);
    writer.addColumn(tr("Date"), 0, TransactionTableModel::DateRole);
    writer.addColumn(tr("Type"), TransactionTableModel::Type, Qt::EditRole);
    writer.addColumn(tr("Label"), 0, TransactionTableModel::LabelRole);
    writer.addColumn(tr("Address"), 0, TransactionTableModel::AddressRole);
    writer.addColumn(BitcoinUnits::getAmountColumnTitle(model->getOptionsModel()->getDisplayUnit()), 0, TransactionTableModel::FormattedAmountRole);
    writer.addColumn(tr("ID"), 0, TransactionTableModel::TxHashRole);

    if(!writer.write()) {
        Q_EMIT message(tr("Exporting Failed"), tr("There was an error trying to save the transaction history to %1.").arg(filename),
            CClientUIInterface::MSG_ERROR);
    }
    else {
        Q_EMIT message(tr("Exporting Successful"), tr("The transaction history was successfully saved to %1.").arg(filename),
            CClientUIInterface::MSG_INFORMATION);
    }
}

void TransactionView::contextualMenu(const QPoint &point)
{
    if (!transactionView || !transactionView->selectionModel())
        return;
    QModelIndex index = transactionView->indexAt(point);
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);
    if (selection.empty())
        return;

    // check if transaction can be abandoned, disable context menu action in case it doesn't
    uint256 hash;
    hash.SetHex(selection.at(0).data(TransactionTableModel::TxHashRole).toString().toStdString());
    abandonAction->setEnabled(model->wallet().transactionCanBeAbandoned(hash));
    resendAction->setEnabled(selection.size() == 1 && model->wallet().transactionCanBeResent(hash));
    copyAddressAction->setEnabled(GUIUtil::hasEntryData(transactionView, 0, TransactionTableModel::AddressRole));
    copyLabelAction->setEnabled(GUIUtil::hasEntryData(transactionView, 0, TransactionTableModel::LabelRole));

    if (index.isValid()) {
        GUIUtil::PopupMenu(contextMenu, transactionView->viewport()->mapToGlobal(point));
    }
}

void TransactionView::abandonTx()
{
    if(!transactionView || !transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);

    // get the hash from the TxHashRole (QVariant / QString)
    uint256 hash;
    QString hashQStr = selection.at(0).data(TransactionTableModel::TxHashRole).toString();
    hash.SetHex(hashQStr.toStdString());

    // Abandon the wallet transaction over the walletModel
    model->wallet().abandonTransaction(hash);
}

void TransactionView::resendTx()
{
    if(!transactionView || !transactionView->selectionModel()) {
        return;
    }
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);

    // get the hash from the TxHashRole (QVariant / QString)
    uint256 hash;
    QString hashQStr = selection.at(0).data(TransactionTableModel::TxHashRole).toString();
    hash.SetHex(hashQStr.toStdString());

    // Abandon the wallet transaction over the walletModel
    model->wallet().resendTransaction(hash);
}

void TransactionView::copyAddress()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::AddressRole);
}

void TransactionView::copyLabel()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::LabelRole);
}

void TransactionView::copyAmount()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::FormattedAmountRole);
}

void TransactionView::copyTxID()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxHashRole);
}

void TransactionView::copyTxHex()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxHexRole);
}

void TransactionView::copyTxPlainText()
{
    GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
}

void TransactionView::editLabel()
{
    if(!transactionView->selectionModel() ||!model)
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        AddressTableModel *addressBook = model->getAddressTableModel();
        if(!addressBook)
            return;
        QString address = selection.at(0).data(TransactionTableModel::AddressRole).toString();
        if(address.isEmpty())
        {
            // If this transaction has no associated address, exit
            return;
        }
        // Is address in address book? Address book can miss address when a transaction is
        // sent from outside the UI.
        int idx = addressBook->lookupAddress(address);
        if(idx != -1)
        {
            // Edit sending / receiving address
            QModelIndex modelIdx = addressBook->index(idx, 0, QModelIndex());
            // Determine type of address, launch appropriate editor dialog type
            QString type = modelIdx.data(AddressTableModel::TypeRole).toString();

            EditAddressDialog dlg(
                type == AddressTableModel::Receive
                ? EditAddressDialog::EditReceivingAddress
                : EditAddressDialog::EditSendingAddress, this);
            dlg.setModel(addressBook);
            dlg.loadRow(idx);
            dlg.exec();
        }
        else
        {
            // Add sending address
            EditAddressDialog dlg(EditAddressDialog::NewSendingAddress,
                this);
            dlg.setModel(addressBook);
            dlg.setAddress(address);
            dlg.exec();
        }
    }
}

void TransactionView::showDetails()
{
    if(!transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        TransactionDescDialog* dlg = new TransactionDescDialog(selection.at(0), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    }
}

void TransactionView::showAddressQRCode()
{
    QList<QModelIndex> entries = GUIUtil::getEntryData(transactionView, 0);
    if (entries.empty()) {
        return;
    }

    QString strAddress = entries.at(0).data(TransactionTableModel::AddressRole).toString();
    QRDialog* dialog = new QRDialog(this);

    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setInfo(tr("QR code"), "defcon:"+strAddress, "", strAddress);
    dialog->show();
}

/** Compute sum of all selected transactions */
void TransactionView::computeSum()
{
    qint64 amount = 0;
    int nDisplayUnit = model->getOptionsModel()->getDisplayUnit();
    if(!transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows();

    for (QModelIndex index : selection){
        amount += index.data(TransactionTableModel::AmountRole).toLongLong();
    }
    QString strAmount(BitcoinUnits::formatWithUnit(nDisplayUnit, amount, true, BitcoinUnits::SeparatorStyle::ALWAYS));
    if (amount < 0) strAmount = "<span style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR) + "'>" + strAmount + "</span>";
    Q_EMIT trxAmount(strAmount);
}

void TransactionView::openThirdPartyTxUrl(QString url)
{
    if(!transactionView || !transactionView->selectionModel())
        return;
    QModelIndexList selection = transactionView->selectionModel()->selectedRows(0);
    if(!selection.isEmpty())
         QDesktopServices::openUrl(QUrl::fromUserInput(url.replace("%s", selection.at(0).data(TransactionTableModel::TxHashRole).toString())));
}

QWidget *TransactionView::createDateRangeWidget()
{
    // Create default dates in case nothing is persisted
    QString defaultDateFrom = QDate::currentDate().toString(PERSISTENCE_DATE_FORMAT);
    QString defaultDateTo = QDate::currentDate().addDays(1).toString(PERSISTENCE_DATE_FORMAT);
    QSettings settings;

    dateRangeWidget = new QFrame();
    dateRangeWidget->setFrameStyle(static_cast<int>(QFrame::Panel) | static_cast<int>(QFrame::Raised));
    dateRangeWidget->setContentsMargins(1,1,1,1);
    QHBoxLayout *layout = new QHBoxLayout(dateRangeWidget);
    layout->setContentsMargins(0,0,0,0);
    layout->addSpacing(23);
    layout->addWidget(new QLabel(tr("Range:")));

    dateFrom = new QDateTimeEdit(this);
    dateFrom->setCalendarPopup(true);
    dateFrom->setMinimumWidth(100);
    // Load persisted FROM date
    dateFrom->setDate(QDate::fromString(settings.value("transactionDateFrom", defaultDateFrom).toString(), PERSISTENCE_DATE_FORMAT));

    layout->addWidget(dateFrom);
    layout->addWidget(new QLabel(tr("to")));

    dateTo = new QDateTimeEdit(this);
    dateTo->setCalendarPopup(true);
    dateTo->setMinimumWidth(100);
    // Load persisted TO date
    dateTo->setDate(QDate::fromString(settings.value("transactionDateTo", defaultDateTo).toString(), PERSISTENCE_DATE_FORMAT));

    layout->addWidget(dateTo);
    layout->addStretch();

    // Hide by default
    dateRangeWidget->setVisible(false);

    // Notify on change
    connect(dateFrom, &QDateTimeEdit::dateChanged, this, &TransactionView::dateRangeChanged);
    connect(dateTo, &QDateTimeEdit::dateChanged, this, &TransactionView::dateRangeChanged);

    updateCalendarWidgets();

    return dateRangeWidget;
}

void TransactionView::updateCalendarWidgets()
{
    auto adjustWeekEndColors = [](QCalendarWidget* w) {
        QTextCharFormat format = w->weekdayTextFormat(Qt::Saturday);
        format.setForeground(QBrush(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::DEFAULT), Qt::SolidPattern));

        w->setWeekdayTextFormat(Qt::Saturday, format);
        w->setWeekdayTextFormat(Qt::Sunday, format);
    };

    adjustWeekEndColors(dateFrom->calendarWidget());
    adjustWeekEndColors(dateTo->calendarWidget());
}

void TransactionView::dateRangeChanged()
{
    if(!transactionProxyModel)
        return;

    // Persist new date range
    QSettings settings;
    settings.setValue("transactionDateFrom", dateFrom->date().toString(PERSISTENCE_DATE_FORMAT));
    settings.setValue("transactionDateTo", dateTo->date().toString(PERSISTENCE_DATE_FORMAT));

    transactionProxyModel->setDateRange(
            GUIUtil::StartOfDay(dateFrom->date()),
            GUIUtil::StartOfDay(dateTo->date()));
}

void TransactionView::focusTransaction(const QModelIndex &idx)
{
    if(!transactionProxyModel)
        return;
    QModelIndex targetIdx = transactionProxyModel->mapFromSource(idx);
    transactionView->selectRow(targetIdx.row());
    computeSum();
    transactionView->scrollTo(targetIdx);
    transactionView->setCurrentIndex(targetIdx);
    transactionView->setFocus();
}

void TransactionView::focusTransaction(const uint256& txid)
{
    if (!transactionProxyModel)
        return;

    const QModelIndexList results = this->model->getTransactionTableModel()->match(
        this->model->getTransactionTableModel()->index(0,0),
        TransactionTableModel::TxHashRole,
        QString::fromStdString(txid.ToString()), -1);

    transactionView->setFocus();
    transactionView->selectionModel()->clearSelection();
    for (const QModelIndex& index : results) {
        const QModelIndex targetIndex = transactionProxyModel->mapFromSource(index);
        transactionView->selectionModel()->select(
            targetIndex,
            QItemSelectionModel::Rows | QItemSelectionModel::Select);
        // Called once per destination to ensure all results are in view, unless
        // transactions are not ordered by (ascending or descending) date.
        transactionView->scrollTo(targetIndex);
        // scrollTo() does not scroll far enough the first time when transactions
        // are ordered by ascending date.
        if (index == results[0]) transactionView->scrollTo(targetIndex);
    }
}

void TransactionView::updateFilterPresentation()
{
    const bool modern = GUIUtil::isModernTheme();
    dateWidget->setItemText(0, modern ? tr("Any date") : tr("All"));
    typeWidget->setItemText(0, modern ? tr("Any type") : tr("All"));
    dateWidget->setToolTip(modern ? tr("Show only transactions from this period") : QString());
    typeWidget->setToolTip(modern ? tr("Show only transactions of this type") : QString());
    amountWidget->setToolTip(modern ? tr("Show only transactions of at least this amount") : QString());
    // Abyss starts the row at the table's left edge, and its watch-only
    // filter says what each choice shows. The classic themes keep the filters
    // above their columns, and the narrow list of icons with its first row
    // empty.
    m_filter_status_gap->changeSize(modern ? 0 : STATUS_COLUMN_WIDTH - 2, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    const std::array<QString, 3> words{tr("All addresses"), tr("Watch-only"), tr("Spendable only")};
    const std::array<const char*, 3> eyes{"eye", "eye_plus", "eye_minus"};
    for (int i = 0; i < 3; ++i) {
        watchOnlyWidget->setItemText(i, modern ? words[i] : QString());
        watchOnlyWidget->setItemIcon(i, modern ? GUIUtil::getIcon(eyes[i]) : m_watch_classic_icons[i]);
    }
    watchOnlyWidget->setToolTip(modern ? tr("Show only transactions of these addresses") : QString());
    const auto horizontal = [](QWidget* widget, QSizePolicy::Policy horizontal_policy) {
        QSizePolicy policy = widget->sizePolicy();
        policy.setHorizontalPolicy(horizontal_policy);
        widget->setSizePolicy(policy);
    };
    // Abyss: the watch-only filter as wide as its widest choice, the type
    // filter as wide as before; where the row is short of room both give way
    // (FilterBox), the watch-only filter down to its eye, the type filter to
    // its first choice whole, so that the row needs no more room than it did;
    // the search field gives way first (SearchField).
    // The date filter as wide as its widest choice, never less than before,
    // so that a larger font does not cut it, also in a narrow row.
    using Rule = FilterBox::Rule;
    static_cast<FilterBox*>(watchOnlyWidget)->setRule(modern ? Rule::GivesWay : Rule::Plain);
    static_cast<FilterBox*>(typeWidget)->setRule(modern ? Rule::GivesWay : Rule::Plain, typeWidget->itemText(0));
    static_cast<FilterBox*>(dateWidget)->setRule(modern ? Rule::Whole : Rule::Plain, QString(), DATE_COLUMN_WIDTH);
    static_cast<SearchField*>(search_widget)->setModest(modern);
    horizontal(watchOnlyWidget, modern ? QSizePolicy::Maximum : QSizePolicy::Preferred);
    horizontal(dateWidget, modern ? QSizePolicy::Fixed : QSizePolicy::Preferred);
    if (modern) {
        watchOnlyWidget->setMinimumWidth(0);
        watchOnlyWidget->setMaximumWidth(QWIDGETSIZE_MAX);
        watchOnlyWidget->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        dateWidget->setMinimumWidth(0);
        dateWidget->setMaximumWidth(QWIDGETSIZE_MAX);
        dateWidget->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        typeWidget->setMinimumWidth(0);
        typeWidget->setMaximumWidth(TYPE_COLUMN_WIDTH - 1);
    } else {
        watchOnlyWidget->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
        watchOnlyWidget->setFixedWidth(24);
        dateWidget->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
        dateWidget->setFixedWidth(DATE_COLUMN_WIDTH);
        typeWidget->setFixedWidth(TYPE_COLUMN_WIDTH - 1);
    }
#ifndef Q_OS_MAC
    transactionView->setVerticalScrollBarPolicy(modern ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOn);
    m_filter_scrollbar_gap->changeSize(modern ? 0 : m_filter_scrollbar_width, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
#endif
    m_filter_row->invalidate();
}

void TransactionView::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::StyleChange) {
        updateCalendarWidgets();
        updateEmptyState();
        updateFilterPresentation();
    }
}

// Need to override default Ctrl+C action for amount as default behaviour is just to copy DisplayRole text
bool TransactionView::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_C && ke->modifiers().testFlag(Qt::ControlModifier))
        {
             GUIUtil::copyEntryData(transactionView, 0, TransactionTableModel::TxPlainTextRole);
             return true;
        }
    }
    if (event->type() == QEvent::Show) {
        // Give the search field the first focus on startup
        static bool fGotFirstFocus = false;
        if (!fGotFirstFocus) {
            search_widget->setFocus();
            fGotFirstFocus = true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

// show/hide column Watch-only
void TransactionView::updateWatchOnlyColumn(bool fHaveWatchOnly)
{
    watchOnlyWidget->setVisible(fHaveWatchOnly);
    transactionView->setColumnHidden(TransactionTableModel::Watchonly, !fHaveWatchOnly);
}

void TransactionView::updateCoinJoinVisibility()
{
    if (model == nullptr) {
        return;
    }
    bool fEnabled = model->node().coinJoinOptions().isEnabled();
    // If CoinJoin gets enabled use "All" else "Most common"
    int idx = fEnabled ? 0 : 1;
    chooseType(idx);
    typeWidget->setCurrentIndex(idx);
    // Hide all CoinJoin related filters
    QListView* typeList = qobject_cast<QListView*>(typeWidget->view());
    std::vector<int> vecRows{4, 5, 6, 7, 8};
    for (auto nRow : vecRows) {
        typeList->setRowHidden(nRow, !fEnabled);
    }
}
