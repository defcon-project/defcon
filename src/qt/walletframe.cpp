// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/walletframe.h>

#include <fs.h>
#include <node/interface_ui.h>
#include <psbt.h>
#include <qt/emptystatewidget.h>
#include <qt/governancelist.h>
#include <qt/guiutil.h>
#include <qt/masternodelist.h>
#include <qt/overviewpage.h>
#include <qt/psbtoperationsdialog.h>
#include <qt/walletmodel.h>
#include <qt/walletview.h>
#include <util/system.h>

#include <algorithm>
#include <cassert>
#include <fstream>
#include <string>

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
/** The frame's stack inside the frame's scroll area. In Abyss it asks for what
 *  the page on show needs, so a page scrolls only when it does not fit the
 *  window, and the window may be made smaller than its largest page. In a
 *  classic theme it asks for nothing: the scroll area then always gives it the
 *  viewport's size, exactly what the frame's layout gave it before. */
class CurrentPageStack : public QStackedWidget
{
public:
    explicit CurrentPageStack(QWidget* parent) : QStackedWidget(parent)
    {
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize minimumSizeHint() const override
    {
        const QWidget* page = currentWidget();
        if (!GUIUtil::isModernTheme() || page == nullptr) return QSize(0, 0);
        return page->minimumSizeHint().expandedTo(page->minimumSize());
    }
    /** The scroll area asks this whenever any page has a height for its width
     *  (wrapped text), and QStackedLayout would answer for every page at once.
     *  Abyss: the page on show only, at least its minimum. Classic: nothing,
     *  so the page keeps the viewport's height as the frame's layout gave it. */
    int heightForWidth(int width) const override
    {
        const QWidget* page = currentWidget();
        if (!GUIUtil::isModernTheme() || page == nullptr) return 0;
        return std::max(page->heightForWidth(width), minimumSizeHint().height());
    }
    //! What the bare stack asked for before it stood in a scroll area: the most any page needs.
    QSize allPagesMinimum() const { return QStackedWidget::minimumSizeHint(); }

protected:
    void changeEvent(QEvent* event) override
    {
        QStackedWidget::changeEvent(event);
        // A theme switch changes what the stack asks for.
        if (event->type() == QEvent::StyleChange) updateGeometry();
    }
};

/** The frame's scroll area. In Abyss it asks for no more than a scroll area
 *  does, so the window may be made smaller than the page, which then scrolls.
 *  In a classic theme it asks for what the bare stack asked for, the most any
 *  page needs, so the window keeps the minimum it always had there and the
 *  pages never scroll. */
class PageScrollArea : public QScrollArea
{
public:
    PageScrollArea(QWidget* parent, CurrentPageStack* stack) : QScrollArea(parent), m_stack(stack) {}
    QSize minimumSizeHint() const override
    {
        if (GUIUtil::isModernTheme()) return QScrollArea::minimumSizeHint();
        return m_stack->allPagesMinimum();
    }

protected:
    bool event(QEvent* event) override
    {
        // A page's minimum changed (the stack's layout, through the viewport):
        // a classic theme hands it on to the window as the frame's layout did.
        if (event->type() == QEvent::LayoutRequest && !GUIUtil::isModernTheme()) updateGeometry();
        return QScrollArea::event(event);
    }

private:
    const CurrentPageStack* m_stack;
};
} // namespace

WalletFrame::WalletFrame(QWidget* parent)
    : QFrame(parent),
      m_size_hint(OverviewPage{nullptr}.sizeHint())
{
    // Leave HBox hook for adding a list view later
    QHBoxLayout *walletFrameLayout = new QHBoxLayout(this);
    setContentsMargins(0,0,0,0);
    auto* stack = new CurrentPageStack(this);
    walletStack = stack;
    walletFrameLayout->setContentsMargins(0,0,0,0);
    // A page that does not fit the window scrolls (Abyss): the window can be
    // made much smaller than the Overview of a wallet with many rows.
    auto* scroll = new PageScrollArea(this, stack);
    scroll->setObjectName("abyssPageScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    // Not a stop of its own on the way through a page with Tab.
    scroll->setFocusPolicy(Qt::NoFocus);
    scroll->setWidget(walletStack);
    // setWidget() turns the fill on; the pages draw themselves, and in Abyss
    // the backdrop has to show between them.
    walletStack->setAutoFillBackground(false);
    walletFrameLayout->addWidget(scroll);

    // hbox for no wallet
    no_wallet_group = new QGroupBox(walletStack);
    no_wallet_group->setObjectName("no_wallet_group");
    QVBoxLayout* no_wallet_layout = new QVBoxLayout(no_wallet_group);

    m_no_wallet_text = new QLabel(tr("No wallet has been loaded.\nGo to File > Open Wallet to load a wallet.\n- OR -"));
    m_no_wallet_text->setAlignment(Qt::AlignCenter);
    no_wallet_layout->addWidget(m_no_wallet_text, 0, Qt::AlignHCenter | Qt::AlignBottom);

    // A button for create wallet dialog
    m_create_wallet_button = new QPushButton(tr("Create a new wallet"), walletStack);
    connect(m_create_wallet_button, &QPushButton::clicked, this, &WalletFrame::createWalletButtonClicked);
    no_wallet_layout->addWidget(m_create_wallet_button, 0, Qt::AlignHCenter | Qt::AlignTop);

    // Abyss has no File menu to point at: its card lists the wallets to open
    // itself, and keeps the same Create a new wallet button.
    m_no_wallet_state = new EmptyStateWidget("wallet", tr("No wallet loaded"), tr("Open a wallet to view its information."),
                                             tr("Open wallet"), no_wallet_group);
    m_no_wallet_state->actionButton()->setEnabled(false);
    connect(m_no_wallet_state->addSecondaryButton(tr("Create a new wallet")), &QPushButton::clicked, this, &WalletFrame::createWalletButtonClicked);
    no_wallet_layout->addWidget(m_no_wallet_state, 1);
    no_wallet_group->setLayout(no_wallet_layout);
    updateThemePresentation();

    walletStack->addWidget(no_wallet_group);

    masternodeListPage = new MasternodeList();
    walletStack->addWidget(masternodeListPage);

    governanceListPage = new GovernanceList();
    walletStack->addWidget(governanceListPage);
}

WalletFrame::~WalletFrame()
{
}

void WalletFrame::setOpenWalletMenu(QMenu* menu)
{
    m_no_wallet_state->actionButton()->setMenu(menu);
    m_no_wallet_state->actionButton()->setEnabled(menu != nullptr);
}

void WalletFrame::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::StyleChange) updateThemePresentation();
}

void WalletFrame::updateThemePresentation()
{
    if (m_no_wallet_state == nullptr) return;
    const bool modern = GUIUtil::isModernTheme();
    m_no_wallet_text->setVisible(!modern);
    m_create_wallet_button->setVisible(!modern);
    m_no_wallet_state->setState(modern ? EmptyStateWidget::State::Empty : EmptyStateWidget::State::Hidden);
}

QSize WalletFrame::pageMinimum() const
{
    return walletStack->minimumSizeHint();
}

QSize WalletFrame::pageWhole() const
{
    QSize whole = walletStack->minimumSizeHint();
    // A page with wrapped text may ask for more height at its least width
    // than its minimum says; the scroll area gives it that height.
    if (walletStack->hasHeightForWidth()) whole.setHeight(std::max(whole.height(), walletStack->heightForWidth(whole.width())));
    return whole;
}

void WalletFrame::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    masternodeListPage->setClientModel(_clientModel);
    governanceListPage->setClientModel(_clientModel);

    for (auto i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i) {
        i.value()->setClientModel(_clientModel);
    }
}

bool WalletFrame::addWallet(WalletModel* walletModel, WalletView* walletView)
{
    if (!clientModel || !walletModel) return false;

    if (mapWalletViews.count(walletModel) > 0) return false;

    walletView->setClientModel(clientModel);
    walletView->setWalletModel(walletModel);
    walletView->showOutOfSyncWarning(bOutOfSync);

    WalletView* current_wallet_view = currentWalletView();
    if (current_wallet_view) {
        walletView->setCurrentIndex(current_wallet_view->currentIndex());
    } else {
        walletView->gotoOverviewPage();
    }

    walletStack->addWidget(walletView);
    mapWalletViews[walletModel] = walletView;

    return true;
}

void WalletFrame::setCurrentWallet(WalletModel* wallet_model)
{
    if (mapWalletViews.count(wallet_model) == 0) return;

    // Stop the effect of hidden widgets on the size hint of the shown one in QStackedWidget.
    WalletView* view_about_to_hide = currentWalletView();
    if (view_about_to_hide) {
        QSizePolicy sp = view_about_to_hide->sizePolicy();
        sp.setHorizontalPolicy(QSizePolicy::Ignored);
        view_about_to_hide->setSizePolicy(sp);
    }

    WalletView *walletView = mapWalletViews.value(wallet_model);
    assert(walletView);

    // Set or restore the default QSizePolicy which could be set to QSizePolicy::Ignored previously.
    QSizePolicy sp = walletView->sizePolicy();
    sp.setHorizontalPolicy(QSizePolicy::Preferred);
    walletView->setSizePolicy(sp);
    walletView->updateGeometry();

    walletStack->setCurrentWidget(walletView);

    Q_EMIT currentWalletSet();
}

void WalletFrame::removeWallet(WalletModel* wallet_model)
{
    if (mapWalletViews.count(wallet_model) == 0) return;

    WalletView *walletView = mapWalletViews.take(wallet_model);
    walletStack->removeWidget(walletView);
    delete walletView;
}

void WalletFrame::removeAllWallets()
{
    QMap<WalletModel*, WalletView*>::const_iterator i;
    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i) {
        walletStack->removeWidget(i.value());
        delete i.value();
    }
    mapWalletViews.clear();
}

bool WalletFrame::handlePaymentRequest(const SendCoinsRecipient &recipient)
{
    WalletView *walletView = currentWalletView();
    if (!walletView)
        return false;

    return walletView->handlePaymentRequest(recipient);
}

void WalletFrame::showOutOfSyncWarning(bool fShow)
{
    bOutOfSync = fShow;
    QMap<WalletModel*, WalletView*>::const_iterator i;
    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i)
        i.value()->showOutOfSyncWarning(fShow);
}

void WalletFrame::setStakingStatus(const OverviewPage::StakingStatus& node_status)
{
    for (auto i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i) {
        i.value()->setStakingStatus(node_status);
    }
}

void WalletFrame::gotoGovernancePage()
{
    QMap<WalletModel*, WalletView*>::const_iterator i;

    if (mapWalletViews.empty()) {
        walletStack->setCurrentWidget(governanceListPage);
        return;
    }

    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i) {
        i.value()->gotoGovernancePage();
    }
}


void WalletFrame::gotoOverviewPage()
{
    QMap<WalletModel*, WalletView*>::const_iterator i;

    if (mapWalletViews.empty()) {
        walletStack->setCurrentWidget(no_wallet_group);
        return;
    }

    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i)
        i.value()->gotoOverviewPage();
}

void WalletFrame::gotoHistoryPage()
{
    QMap<WalletModel*, WalletView*>::const_iterator i;
    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i)
        i.value()->gotoHistoryPage();
}

void WalletFrame::gotoMasternodePage()
{
    QMap<WalletModel*, WalletView*>::const_iterator i;

    if (mapWalletViews.empty()) {
        walletStack->setCurrentWidget(masternodeListPage);
        return;
    }

    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i)
        i.value()->gotoMasternodePage();
}

void WalletFrame::gotoMultisigPage()
{
    QMap<WalletModel*, WalletView*>::const_iterator i;

    // The multisig tab button is disabled while no wallet is loaded.
    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i)
        i.value()->gotoMultisigPage();
}

void WalletFrame::gotoReceiveCoinsPage()
{
    QMap<WalletModel*, WalletView*>::const_iterator i;
    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i)
        i.value()->gotoReceiveCoinsPage();
}

void WalletFrame::gotoSendCoinsPage(QString addr)
{
    QMap<WalletModel*, WalletView*>::const_iterator i;
    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i)
        i.value()->gotoSendCoinsPage(addr);
}

void WalletFrame::gotoCoinJoinCoinsPage(QString addr)
{
    QMap<WalletModel*, WalletView*>::const_iterator i;
    for (i = mapWalletViews.constBegin(); i != mapWalletViews.constEnd(); ++i) {
        i.value()->gotoCoinJoinCoinsPage(addr);
    }
}

void WalletFrame::gotoSignMessageTab(QString addr)
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->gotoSignMessageTab(addr);
}

void WalletFrame::gotoVerifyMessageTab(QString addr)
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->gotoVerifyMessageTab(addr);
}

void WalletFrame::gotoLoadPSBT(bool from_clipboard)
{
    std::vector<unsigned char> data;

    if (from_clipboard) {
        std::string raw = QApplication::clipboard()->text().toStdString();
        auto result = DecodeBase64(raw);
        if (!result) {
            Q_EMIT message(tr("Error"), tr("Unable to decode PSBT from clipboard (invalid base64)"), CClientUIInterface::MSG_ERROR);
            return;
        }
        data = std::move(*result);
    } else {
        QString filename = GUIUtil::getOpenFileName(this,
            tr("Load Transaction Data"), QString(),
            tr("Partially Signed Transaction (*.psbt)"), nullptr);
        if (filename.isEmpty()) return;
        if (GetFileSize(filename.toLocal8Bit().data(), MAX_FILE_SIZE_PSBT) == MAX_FILE_SIZE_PSBT) {
            Q_EMIT message(tr("Error"), tr("PSBT file must be smaller than 100 MiB"), CClientUIInterface::MSG_ERROR);
            return;
        }
        std::ifstream in{filename.toLocal8Bit().data(), std::ios::binary};
        data.assign(std::istreambuf_iterator<char>{in}, {});
    }

    std::string error;
    PartiallySignedTransaction psbtx;
    if (!DecodeRawPSBT(psbtx, MakeByteSpan(data), error)) {
        Q_EMIT message(tr("Error"), tr("Unable to decode PSBT") + "\n" + QString::fromStdString(error), CClientUIInterface::MSG_ERROR);
        return;
    }

    PSBTOperationsDialog* dlg = new PSBTOperationsDialog(this, currentWalletModel(), clientModel);
    dlg->openWithPSBT(psbtx);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

void WalletFrame::encryptWallet()
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->encryptWallet();
}

void WalletFrame::backupWallet()
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->backupWallet();
}

void WalletFrame::changePassphrase()
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->changePassphrase();
}

void WalletFrame::unlockWallet()
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->unlockWallet();
}

void WalletFrame::lockWallet()
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->lockWallet();
}

void WalletFrame::usedSendingAddresses()
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->usedSendingAddresses();
}

void WalletFrame::usedReceivingAddresses()
{
    WalletView *walletView = currentWalletView();
    if (walletView)
        walletView->usedReceivingAddresses();
}

WalletView* WalletFrame::currentWalletView() const
{
    return qobject_cast<WalletView*>(walletStack->currentWidget());
}

WalletModel* WalletFrame::currentWalletModel() const
{
    WalletView* wallet_view = currentWalletView();
    return wallet_view ? wallet_view->getWalletModel() : nullptr;
}
