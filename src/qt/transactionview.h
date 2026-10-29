// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TRANSACTIONVIEW_H
#define BITCOIN_QT_TRANSACTIONVIEW_H

#include <qt/guiutil.h>

#include <uint256.h>

#include <QWidget>
#include <QKeyEvent>

class EmptyStateWidget;
class TransactionFilterProxy;
class WalletModel;

QT_BEGIN_NAMESPACE
class QComboBox;
class QDateTimeEdit;
class QFrame;
class QItemSelectionModel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QTableView;
QT_END_NAMESPACE

/** Widget showing the transaction list for a wallet, including a filter row.
    Using the filter row, the user can view or export a subset of the transactions.
  */
class TransactionView : public QWidget
{
    Q_OBJECT

public:
    explicit TransactionView(QWidget* parent = nullptr);

    void setModel(WalletModel *model);
    //! While the wallet is still catching up, an empty history is loading.
    void setOutOfSync(bool out_of_sync);

    // Date ranges for filter
    enum DateEnum
    {
        All,
        Today,
        ThisWeek,
        ThisMonth,
        LastMonth,
        ThisYear,
        Range
    };

    enum ColumnWidths {
        STATUS_COLUMN_WIDTH = 30,
        WATCHONLY_COLUMN_WIDTH = 23,
        DATE_COLUMN_WIDTH = 120,
        TYPE_COLUMN_WIDTH = 240,
        AMOUNT_MINIMUM_COLUMN_WIDTH = 120,
        MINIMUM_COLUMN_WIDTH = 23
    };

private:
    WalletModel *model{nullptr};
    TransactionFilterProxy *transactionProxyModel{nullptr};
    QTableView *transactionView{nullptr};
    QComboBox *dateWidget;
    QComboBox *typeWidget;
    QComboBox *watchOnlyWidget;
    QLineEdit *search_widget;
    QLineEdit *amountWidget;

    QMenu *contextMenu;

    QFrame *dateRangeWidget;
    QDateTimeEdit *dateFrom;
    QDateTimeEdit *dateTo;
    QAction *abandonAction{nullptr};
    QAction *resendAction;
    QAction *copyAddressAction{nullptr};
    QAction *copyLabelAction{nullptr};
    EmptyStateWidget* m_empty_state{nullptr};
    bool m_out_of_sync{false};

    QWidget *createDateRangeWidget();
    void updateCalendarWidgets();
    void updateEmptyState();

    void changeEvent(QEvent* e) override;

    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void contextualMenu(const QPoint &);
    void dateRangeChanged();
    void showDetails();
    void showAddressQRCode();
    void copyAddress();
    void editLabel();
    void copyLabel();
    void copyAmount();
    void copyTxID();
    void copyTxHex();
    void copyTxPlainText();
    void openThirdPartyTxUrl(QString url);
    void updateWatchOnlyColumn(bool fHaveWatchOnly);
    void updateCoinJoinVisibility();
    void abandonTx();
    void resendTx();

Q_SIGNALS:
    void doubleClicked(const QModelIndex&);

    /**  Fired when a message should be reported to the user */
    void message(const QString &title, const QString &message, unsigned int style);

    /** Send computed sum back to wallet-view */
    void trxAmount(QString amount);

    /** The empty history's Receive button was clicked */
    void receiveClicked();

public Q_SLOTS:
    void chooseDate(int idx);
    void chooseType(int idx);
    void chooseWatchonly(int idx);
    void changedAmount();
    void changedSearch();
    /** Show every transaction again: no date, type, watch-only, text or amount filter */
    void clearFilters();
    void exportClicked();
    void focusTransaction(const QModelIndex&);
    void focusTransaction(const uint256& txid);
    void computeSum();
};

#endif // BITCOIN_QT_TRANSACTIONVIEW_H
