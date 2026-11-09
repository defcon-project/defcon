// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_RECEIVECOINSDIALOG_H
#define BITCOIN_QT_RECEIVECOINSDIALOG_H

#include <qt/guiutil.h>

#include <QDialog>
#include <QHeaderView>
#include <QItemSelection>
#include <QKeyEvent>
#include <QMenu>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QVariant>

class AbyssToast;
class QRImageWidget;
class WalletModel;
namespace interfaces {
struct WalletTx;
} // namespace interfaces

namespace Ui {
    class ReceiveCoinsDialog;
}

QT_BEGIN_NAMESPACE
class QFrame;
class QLabel;
class QModelIndex;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
QT_END_NAMESPACE

/** Dialog for requesting payment of bitcoins */
class ReceiveCoinsDialog : public QDialog
{
    Q_OBJECT

public:
    enum ColumnWidths {
        DATE_COLUMN_WIDTH = 130,
        LABEL_COLUMN_WIDTH = 120,
        AMOUNT_MINIMUM_COLUMN_WIDTH = 180,
        MINIMUM_COLUMN_WIDTH = 130
    };

    explicit ReceiveCoinsDialog(QWidget* parent = nullptr);
    ~ReceiveCoinsDialog();

    void setModel(WalletModel *model);

    //! Side of the QR on Abyss's address card, in logical pixels.
    static constexpr int ADDRESS_QR_SIZE{240};
    //! The card's address in points: as large as its column allows, down to the floor.
    static constexpr int ADDRESS_FONT_SIZE{15};
    static constexpr int ADDRESS_FONT_FLOOR{10};

public Q_SLOTS:
    void clear();
    void reject() override;
    void accept() override;

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Ui::ReceiveCoinsDialog *ui;
    //! Null once the wallet model is gone: at shutdown the models go before their pages,
    //! maybe while a box or a dialog of this page runs its own event loop.
    QPointer<WalletModel> model;
    QMenu *contextMenu;
    QAction* copyLabelAction;
    QAction* copyMessageAction;
    QAction* copyAmountAction;
    void updateThemePresentation();
    QWidget* m_modern_header{nullptr};
    QWidget* m_modern_form{nullptr};
    bool m_modern_layout{false};
    bool m_presentation_ready{false};
    bool m_updating_presentation{false};
    //! Where the classic page keeps its two cards, to put them back exactly.
    int m_classic_form_stretch{0};
    int m_classic_spacer_stretch{0};
    int m_classic_history_stretch{0};

    //! Abyss: the page scrolls rather than pushing the window past the screen.
    QScrollArea* m_abyss_scroll{nullptr};
    QVBoxLayout* m_abyss_column{nullptr};
    //! Abyss: the receiving address first, with its QR and what to do with it.
    QFrame* m_address_card{nullptr};
    QRImageWidget* m_address_qr{nullptr};
    QLabel* m_address_text{nullptr};
    QLabel* m_address_label{nullptr};
    QPushButton* m_edit_label{nullptr};
    QPushButton* m_copy_address{nullptr};
    QPushButton* m_new_address{nullptr};
    AbyssToast* m_toast{nullptr};
    //! Abyss: opens the request form, which stays folded away until asked for.
    QPushButton* m_request_toggle{nullptr};
    //! Abyss: an empty payment history folded to one line, its title and what it says.
    QWidget* m_history_head{nullptr};
    QLabel* m_history_none{nullptr};
    //! The address the card shows, empty while it has none; kept while it is unused.
    QString m_address;
    //! The address whose QR is drawn.
    QString m_qr_address;
    //! The card's address has to be looked at again before it is shown.
    bool m_address_stale{true};
    //! Every address a wallet transaction pays, the wallet's or not, gathered when first needed.
    QSet<QString> m_used_addresses;
    bool m_used_known{false};
    //! A request for a new address is under way: what the wallet signals meanwhile waits for it.
    bool m_making_address{false};
    //! The wallet gave an address it cannot spend from: it is asked for no more this session.
    bool m_generation_off{false};
    //! A request for an address gave one the card cannot show: it makes none by itself after.
    bool m_silent_off{false};
    void buildAddressCard();
    //! Choose the card's address: the one shown, while unused; else an unused unlabeled one
    //! of the wallet's; else a new one, if the wallet gives it without asking for the passphrase.
    void refreshAddress();
    void showAddress();
    //! The address's font: as large as fits the card's column, down to a floor that always fits.
    void fitAddressFont();
    bool addressUsable(const QString& address);
    //! Whether the card may ask for new addresses at all: one it could spend from, that is.
    bool canMakeAddress();
    QString firstUnusedAddress();
    const QSet<QString>& usedAddresses();
    void noteOutputs(const interfaces::WalletTx& wtx);
    void noteTransactions(int first, int last);
    //! Abyss folds an empty payment history down to its title.
    void updateHistoryState();

    QModelIndex selectedRow();
    void copyColumnToClipboard(int column);

private Q_SLOTS:
    void copyReceivingAddress();
    void newReceivingAddress();
    void editReceivingLabel();
    void on_receiveButton_clicked();
    void on_showRequestButton_clicked();
    void on_removeRequestButton_clicked();
    void on_recentRequestsView_doubleClicked(const QModelIndex &index);
    void recentRequestsView_selectionChanged(const QItemSelection &selected, const QItemSelection &deselected);
    void updateDisplayUnit();
    void showMenu(const QPoint &point);
    void copyURI();
    void copyAddress();
    void copyLabel();
    void copyMessage();
    void copyAmount();
};

#endif // BITCOIN_QT_RECEIVECOINSDIALOG_H
