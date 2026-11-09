// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/receivecoinsdialog.h>
#include <qt/forms/ui_receivecoinsdialog.h>

#include <qt/abysstoast.h>
#include <qt/addresstablemodel.h>
#include <qt/editaddressdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/qrimagewidget.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/transactiontablemodel.h>
#include <qt/walletmodel.h>

#include <interfaces/wallet.h>
#include <key_io.h>
#include <uint256.h>

#include <algorithm>
#include <set>

#include <QAction>
#include <QCursor>
#include <QFontMetricsF>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QStyle>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QtMath>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h> /* for USE_QRCODE */
#endif

ReceiveCoinsDialog::ReceiveCoinsDialog(QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::ReceiveCoinsDialog),
    model(nullptr)
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_6}, GUIUtil::FontWeight::Bold, 16);
    GUIUtil::setFont({ui->label,
                      ui->label_2,
                      ui->label_3}, GUIUtil::FontWeight::Normal, 15);
    GUIUtil::updateFonts();

    // context menu
    contextMenu = new QMenu(this);
    contextMenu->addAction(tr("Copy &URI"), this, &ReceiveCoinsDialog::copyURI);
    contextMenu->addAction(tr("&Copy address"), this, &ReceiveCoinsDialog::copyAddress);
    copyLabelAction = contextMenu->addAction(tr("Copy &label"), this, &ReceiveCoinsDialog::copyLabel);
    copyMessageAction = contextMenu->addAction(tr("Copy &message"), this, &ReceiveCoinsDialog::copyMessage);
    copyAmountAction = contextMenu->addAction(tr("Copy &amount"), this, &ReceiveCoinsDialog::copyAmount);
    connect(ui->recentRequestsView, &QWidget::customContextMenuRequested, this, &ReceiveCoinsDialog::showMenu);

    connect(ui->clearButton, &QPushButton::clicked, this, &ReceiveCoinsDialog::clear);

    m_presentation_ready = true;
    updateThemePresentation();
}

void ReceiveCoinsDialog::buildAddressCard()
{
    m_abyss_scroll = new QScrollArea(this);
    m_abyss_scroll->setObjectName("abyssReceiveScroll");
    m_abyss_scroll->setFrameShape(QFrame::NoFrame);
    m_abyss_scroll->setWidgetResizable(true);
    m_abyss_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Not a stop of its own on the way through the page with Tab.
    m_abyss_scroll->setFocusPolicy(Qt::NoFocus);
    auto* content = new QWidget(m_abyss_scroll);
    content->setObjectName("abyssReceiveContent");
    m_abyss_column = new QVBoxLayout(content);
    m_abyss_column->setContentsMargins(0, 0, 0, 0);
    m_abyss_column->setSpacing(12);

    m_address_card = new QFrame(content);
    m_address_card->setObjectName("abyssReceiveCard");
    auto* row = new QHBoxLayout(m_address_card);
    row->setContentsMargins(16, 14, 16, 14);
    row->setSpacing(20);
    m_address_qr = new QRImageWidget(m_address_card);
    m_address_qr->setObjectName("abyssReceiveQR");
    m_address_qr->setImageSize(ADDRESS_QR_SIZE);
    m_address_qr->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_address_qr->setAccessibleName(tr("QR code of the receiving address"));
    row->addWidget(m_address_qr, 0, Qt::AlignTop);
    auto* details = new QVBoxLayout;
    details->setSpacing(8);
    auto* caption = new QLabel(tr("Your receiving address"), m_address_card);
    caption->setObjectName("abyssReceiveCaption");
    m_address_text = new QLabel(m_address_card);
    m_address_text->setObjectName("abyssReceiveAddress");
    m_address_text->setTextFormat(Qt::PlainText);
    m_address_text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_address_label = new QLabel(m_address_card);
    m_address_label->setObjectName("abyssReceiveLabel");
    m_address_label->setTextFormat(Qt::PlainText);
    m_address_label->setWordWrap(true);
    m_edit_label = new QPushButton(tr("Edit label"), m_address_card);
    m_edit_label->setObjectName("abyssReceiveEditLabel");
    m_edit_label->setCursor(Qt::PointingHandCursor);
    m_edit_label->setAutoDefault(false);
    auto* label_row = new QHBoxLayout;
    label_row->setSpacing(8);
    label_row->addWidget(m_address_label, 1);
    label_row->addWidget(m_edit_label, 0, Qt::AlignVCenter);
    // Without a label the link keeps to its text instead of filling the row.
    label_row->addStretch(0);
    m_copy_address = new QPushButton(tr("Copy"), m_address_card);
    m_copy_address->setObjectName("abyssReceiveCopy");
    m_copy_address->setToolTip(tr("Copy the address to the clipboard"));
    m_copy_address->setAutoDefault(false);
    m_new_address = new QPushButton(tr("New address"), m_address_card);
    m_new_address->setObjectName("abyssReceiveNewAddress");
    m_new_address->setToolTip(tr("Get a new receiving address; the current one keeps working"));
    m_new_address->setAutoDefault(false);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(m_copy_address);
    buttons->addWidget(m_new_address);
    buttons->addStretch(1);
    // The form for a request with an amount opens from the card, in the room the QR leaves beside it.
    m_request_toggle = new QPushButton(tr("Request a specific amount"), m_address_card);
    m_request_toggle->setObjectName("abyssReceiveRequestToggle");
    m_request_toggle->setCheckable(true);
    m_request_toggle->setCursor(Qt::PointingHandCursor);
    m_request_toggle->setAutoDefault(false);
    m_request_toggle->setToolTip(tr("Show the form for a payment request with an amount and a label"));
    m_toast = new AbyssToast(m_address_card);
    details->addWidget(caption);
    details->addWidget(m_address_text);
    details->addLayout(label_row);
    details->addLayout(buttons);
    details->addWidget(m_request_toggle, 0, Qt::AlignLeft);
    details->addStretch(1);
    details->addWidget(m_toast, 0, Qt::AlignLeft);
    row->addLayout(details, 1);
    GUIUtil::setFont({caption, m_address_label}, GUIUtil::FontWeight::Normal, 12);
    // The address's size follows its column (fitAddressFont), not the font registry.
    m_address_text->installEventFilter(this);

    m_abyss_column->addWidget(m_address_card);
    // The two cards of the classic page come in before this stretch.
    m_abyss_column->addStretch(1);
    m_abyss_scroll->setWidget(content);
    m_abyss_scroll->hide();

    // Folded, the history is one line: its title and that it has no request yet.
    m_history_head = new QWidget(ui->frame);
    m_history_head->setObjectName("abyssReceiveHistoryHead");
    auto* head = new QHBoxLayout(m_history_head);
    head->setContentsMargins(0, 0, 0, 0);
    head->setSpacing(12);
    auto* head_title = new QLabel(tr("Requested payments history"), m_history_head);
    head_title->setObjectName("abyssReceiveHistoryTitle");
    m_history_none = new QLabel(tr("No payment requests yet"), m_history_head);
    m_history_none->setObjectName("abyssReceiveHistoryNone");
    GUIUtil::setFont({head_title}, GUIUtil::FontWeight::Bold, 16);
    GUIUtil::setFont({m_history_none}, GUIUtil::FontWeight::Normal, 13);
    head->addWidget(head_title, 0, Qt::AlignVCenter);
    head->addWidget(m_history_none, 0, Qt::AlignVCenter);
    head->addStretch(1);
    m_history_head->hide();
    // Hidden, it takes no room in the classic card.
    ui->verticalLayout_2->insertWidget(0, m_history_head);

    // Keyboard order follows the page from the top; the classic fields keep theirs.
    QWidget* before = ui->reqLabel->previousInFocusChain();
    QWidget::setTabOrder(before, m_address_text);
    QWidget::setTabOrder(m_address_text, m_edit_label);
    QWidget::setTabOrder(m_edit_label, m_copy_address);
    QWidget::setTabOrder(m_copy_address, m_new_address);
    QWidget::setTabOrder(m_new_address, m_request_toggle);

    connect(m_copy_address, &QPushButton::clicked, this, &ReceiveCoinsDialog::copyReceivingAddress);
    connect(m_new_address, &QPushButton::clicked, this, &ReceiveCoinsDialog::newReceivingAddress);
    connect(m_edit_label, &QPushButton::clicked, this, &ReceiveCoinsDialog::editReceivingLabel);
    connect(m_request_toggle, &QPushButton::toggled, this, [this](bool open) {
        if (!m_modern_layout) return;
        ui->frame2->setVisible(open);
        if (open) {
            ui->reqLabel->setFocus(Qt::OtherFocusReason);
            m_abyss_scroll->ensureWidgetVisible(ui->frame2, 0, 0);
        }
    });
}

void ReceiveCoinsDialog::updateHistoryState()
{
    if (!m_history_none) return;
    const int rows = model && model->getRecentRequestsTableModel() ? model->getRecentRequestsTableModel()->rowCount({}) : 0;
    const bool folded = m_modern_layout && rows == 0;
    ui->label_6->setVisible(!folded);
    ui->recentRequestsView->setVisible(!folded);
    ui->showRequestButton->setVisible(!folded);
    ui->removeRequestButton->setVisible(!folded);
    m_history_head->setVisible(folded);
    // The buttons' row keeps its spacer's 20 px in the card's size hint even with both
    // buttons hidden; folded, it has no height. Its size as in the form otherwise.
    ui->horizontalSpacer_2->changeSize(40, folded ? 0 : 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    ui->horizontalLayout_2->invalidate();
    if (m_modern_layout) {
        // A history with rows takes the room left; a folded one leaves it below.
        m_abyss_column->setStretchFactor(ui->frame, folded ? 0 : 1);
        m_abyss_column->setStretch(m_abyss_column->count() - 1, folded ? 1 : 0);
    }
}

const QSet<QString>& ReceiveCoinsDialog::usedAddresses()
{
    if (!m_used_known && model) {
        // From the wallet's transactions, output by output: the transaction list is not a
        // record of them (a payment to self shows its addresses joined on one line, and an
        // outgoing payment does not show what it paid to the wallet's own address).
        m_used_addresses.clear();
        m_used_known = true;
        for (const interfaces::WalletTx& wtx : model->wallet().getWalletTxs()) noteOutputs(wtx);
    }
    return m_used_addresses;
}

void ReceiveCoinsDialog::noteOutputs(const interfaces::WalletTx& wtx)
{
    // Every output's address, the wallet's or not: one paid while it was somebody else's may
    // become the wallet's later (its key imported), and it has been paid all the same. Whether
    // the wallet can spend from an address is asked separately (addressUsable).
    for (const CTxDestination& dest : wtx.txout_address) {
        if (IsValidDestination(dest)) m_used_addresses.insert(QString::fromStdString(EncodeDestination(dest)));
    }
}

void ReceiveCoinsDialog::noteTransactions(int first, int last)
{
    TransactionTableModel* transactions = model ? model->getTransactionTableModel() : nullptr;
    if (!transactions || !m_used_known) return;
    // New rows of the list say which transactions came in; the wallet says what they paid.
    std::set<uint256> seen;
    for (int row = first; row <= last; ++row) {
        const uint256 hash = uint256S(transactions->index(row, 0).data(TransactionTableModel::TxHashRole).toString().toStdString());
        if (seen.insert(hash).second) noteOutputs(model->wallet().getWalletTx(hash));
    }
}

bool ReceiveCoinsDialog::addressUsable(const QString& address)
{
    // The one test every address the card shows passes: the one it showed, one of the
    // wallet's, and one a request for a new address returns. A receiving address of the
    // wallet's, a single key's that it holds the private key for, that no wallet transaction
    // has paid.
    if (address.isEmpty() || !model || usedAddresses().contains(address)) return false;
    // A wallet without private keys spends from none of its addresses, whatever a descriptor
    // reports as its own.
    if (model->wallet().privateKeysDisabled()) return false;
    const CTxDestination dest = DecodeDestination(address.toStdString());
    if (!IsValidDestination(dest)) return false;
    // Only the kind of address the wallet gives out, one key's. For a script of several keys
    // (a multisig imported with only some of them) the wallet says "spendable" without the
    // keys to spend; for one key's it says so only holding that key: a legacy wallet asks for
    // the private key, and a descriptor wallet with private keys takes no descriptor without
    // them, and a single-key descriptor with a key has all of its keys.
    if (!std::holds_alternative<PKHash>(dest)) return false;
    std::string purpose;
    // The wallet itself, not the address list: a new address reaches the list a moment later.
    if (!model->wallet().getAddress(dest, /*name=*/nullptr, /*is_mine=*/nullptr, &purpose)) return false;
    // Never offer a watched address: the wallet could not spend what is sent there.
    return purpose == "receive" && model->wallet().isSpendable(dest);
}

QString ReceiveCoinsDialog::firstUnusedAddress()
{
    AddressTableModel* table = model ? model->getAddressTableModel() : nullptr;
    if (!table) return {};
    for (int row = 0; row < table->rowCount({}); ++row) {
        const QModelIndex address = table->index(row, AddressTableModel::Address, QModelIndex());
        if (address.data(AddressTableModel::TypeRole).toString() != AddressTableModel::Receive) continue;
        // A label says the address was given out for something.
        if (!table->index(row, AddressTableModel::Label, QModelIndex()).data(Qt::EditRole).toString().isEmpty()) continue;
        const QString text = address.data(Qt::EditRole).toString();
        if (addressUsable(text)) return text;
    }
    return {};
}

bool ReceiveCoinsDialog::canMakeAddress()
{
    // A wallet without private keys would give watched addresses: not the card's.
    return model && !m_generation_off && model->wallet().canGetAddresses() && !model->wallet().privateKeysDisabled();
}

void ReceiveCoinsDialog::refreshAddress()
{
    if (!m_modern_layout || !m_address_card) return;
    if (!isVisible() || m_making_address) {
        // Nobody sees the card, or the wallet signals from inside a request for an address
        // (taking a key from its pool does, synchronously): look again later, make nothing now.
        m_address_stale = true;
        return;
    }
    if (!addressUsable(m_address)) {
        m_address = firstUnusedAddress();
        if (m_address.isEmpty() && !m_silent_off && canMakeAddress()) {
            // Only what the wallet gives without its passphrase. A locked wallet with no key
            // left in its pool gets the button instead, which asks for it as it always did.
            QScopedValueRollback<bool> making(m_making_address, true);
            CTxDestination dest;
            if (model->wallet().getNewDestination("", dest)) {
                const QString made = QString::fromStdString(EncodeDestination(dest));
                if (addressUsable(made)) {
                    m_address = made;
                } else {
                    // The wallet gave one the card cannot show (paid already, or watched):
                    // it is asked for no more without the user asking. A watched one also
                    // turns New address off: the pool holds keys the wallet cannot spend.
                    m_silent_off = true;
                    if (!model->wallet().isSpendable(dest)) m_generation_off = true;
                }
            }
        }
    }
    m_address_stale = false;
    showAddress();
}

void ReceiveCoinsDialog::showAddress()
{
    if (!m_address_card || !m_modern_layout) return;
    const bool shown = !m_address.isEmpty();
    bool drawn = false;
    if (shown) {
#ifdef USE_QRCODE
        if (m_qr_address != m_address) {
            const QString uri = GUIUtil::formatBitcoinURI(SendCoinsRecipient(m_address, QString(), 0, QString()));
            m_qr_address = m_address_qr->setQR(uri, m_address) ? m_address : QString();
        }
        drawn = m_qr_address == m_address;
#endif
        m_address_text->setText(m_address);
    } else {
        m_address_text->setText(tr("No receiving address yet."));
    }
    fitAddressFont();
    const QString label = shown && model && model->getAddressTableModel() ?
        model->getAddressTableModel()->labelForAddress(m_address) : QString();
    m_address_qr->setVisible(drawn);
    m_address_label->setText(label);
    m_address_label->setVisible(!label.isEmpty());
    m_edit_label->setVisible(shown);
    m_copy_address->setVisible(shown);
    m_new_address->setEnabled(canMakeAddress());
}

void ReceiveCoinsDialog::fitAddressFont()
{
    if (!m_address_text) return;
    // As the Overview fits its total: the size the card asks for, smaller in quarter points
    // while the whole address does not fit its column, down to a floor. The floor's width is
    // the label's minimum, so the card is never narrower than the whole address at the floor.
    QFont font = GUIUtil::getFont(GUIUtil::FontWeight::Normal, false, ADDRESS_FONT_SIZE);
    if (font.pointSizeF() <= 0) font.setPointSizeF(GUIUtil::getScaledFontSize(ADDRESS_FONT_SIZE));
    QFont floor_font = GUIUtil::getFont(GUIUtil::FontWeight::Normal, false, ADDRESS_FONT_FLOOR);
    const qreal floor = std::max(1.0, floor_font.pointSizeF() > 0 ? floor_font.pointSizeF() : qreal(GUIUtil::getScaledFontSize(ADDRESS_FONT_FLOOR)));
    floor_font = font;
    floor_font.setPointSizeF(std::min(floor, font.pointSizeF()));
    const QString text = m_address_text->text();
    const QMargins margins = m_address_text->contentsMargins();
    const int padding = margins.left() + margins.right() + 2 * m_address_text->margin() + 2;
    const int least = qCeil(QFontMetricsF(floor_font).horizontalAdvance(text)) + padding;
    if (m_address_text->minimumWidth() != least) m_address_text->setMinimumWidth(least);
    const int available = m_address_text->width() - padding;
    while (font.pointSizeF() > floor_font.pointSizeF() && QFontMetricsF(font).horizontalAdvance(text) > available) {
        const qreal previous = font.pointSizeF();
        font.setPointSizeF(std::max(floor_font.pointSizeF(), previous - 0.25));
        if (font.pointSizeF() >= previous) break;
    }
    if (m_address_text->font() != font) m_address_text->setFont(font);
}

bool ReceiveCoinsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_address_text && (event->type() == QEvent::Resize || event->type() == QEvent::ApplicationFontChange)) {
        fitAddressFont();
    }
    return QDialog::eventFilter(watched, event);
}

void ReceiveCoinsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (m_modern_layout && (m_address_stale || m_address.isEmpty())) refreshAddress();
}

void ReceiveCoinsDialog::copyReceivingAddress()
{
    if (m_address.isEmpty()) return;
    GUIUtil::setClipboard(m_address);
    m_toast->showMessage(tr("Address copied"));
}

void ReceiveCoinsDialog::newReceivingAddress()
{
    if (!model || !model->getAddressTableModel() || !canMakeAddress()) return;
    AddressTableModel* table = model->getAddressTableModel();
    QString address;
    {
        // The same request as the classic button: unlocks the wallet if it has to. What the
        // wallet signals meanwhile (it does, from inside) waits until the request has its answer.
        QScopedValueRollback<bool> making(m_making_address, true);
        address = table->addRow(AddressTableModel::Receive, "", "");
    }
    // Asking for the passphrase ran an event loop of its own, in which the wallet may have gone.
    if (!model) return;
    QString error;
    switch (table->getEditStatus()) {
    case AddressTableModel::EditStatus::OK:
        if (addressUsable(address)) {
            m_address = address;
            m_address_stale = false;
        } else {
            // Not one the card shows (paid already, or watched): the card keeps what it had and
            // makes none by itself. A watched one turns New address off; after a paid one the
            // next press asks again.
            m_silent_off = true;
            if (!model->wallet().isSpendable(DecodeDestination(address.toStdString()))) m_generation_off = true;
            error = tr("Could not generate new address");
        }
        break;
    case AddressTableModel::EditStatus::WALLET_UNLOCK_FAILURE:
        error = tr("Could not unlock wallet.");
        break;
    case AddressTableModel::EditStatus::KEY_GENERATION_FAILURE:
        error = tr("Could not generate new address");
        break;
    // Not answers to a request for a receiving address.
    case AddressTableModel::EditStatus::INVALID_ADDRESS:
    case AddressTableModel::EditStatus::DUPLICATE_ADDRESS:
    case AddressTableModel::EditStatus::NO_CHANGES:
        break;
    }
    // Whatever the wallet signalled while the request ran, now looked at.
    if (m_address_stale) refreshAddress(); else showAddress();
    // Last: the box runs an event loop of its own, in which the wallet may go (a shutdown
    // deletes the wallet models before their pages). Nothing here reads it after the box.
    if (!error.isEmpty()) QMessageBox::critical(this, windowTitle(), error, QMessageBox::Ok, QMessageBox::Ok);
}

void ReceiveCoinsDialog::editReceivingLabel()
{
    if (!model || !model->getAddressTableModel() || m_address.isEmpty()) return;
    AddressTableModel* table = model->getAddressTableModel();
    const int row = table->lookupAddress(m_address);
    if (row < 0) return;
    // The address book's own dialog. Opened without a nested event loop, so a wallet
    // closed meanwhile takes the dialog with its page instead of returning into it.
    auto* dialog = new EditAddressDialog(EditAddressDialog::EditReceivingAddress, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModel(table);
    dialog->loadRow(row);
    // At shutdown the wallet model goes before the page: the dialog goes with the model, so
    // nothing can be saved into an address list that is gone.
    connect(model, &QObject::destroyed, dialog, &QDialog::reject);
    dialog->open();
}

void ReceiveCoinsDialog::updateThemePresentation()
{
    if (!m_presentation_ready || m_updating_presentation) return;
    QScopedValueRollback<bool> guard(m_updating_presentation, true);
    const bool modern = GUIUtil::isModernTheme();
    if (modern == m_modern_layout) return;
    QWidget* focus = QApplication::focusWidget();
    const bool active_focus = focus && isAncestorOf(focus);
    if (!active_focus) focus = QWidget::focusWidget();
    const bool restore_focus = focus && isAncestorOf(focus) && (active_focus || !focus->isHidden());
    m_modern_layout = modern;
    if (!m_modern_header) {
        for (QLabel* label : {ui->label, ui->label_2, ui->label_3}) {
            label->setProperty("abyssOriginalAlignment", static_cast<int>(label->alignment()));
        }
        m_classic_form_stretch = ui->verticalLayout->stretch(ui->verticalLayout->indexOf(ui->frame2));
        m_classic_spacer_stretch = ui->verticalLayout->stretch(ui->verticalLayout->indexOf(ui->verticalSpacer_2));
        m_classic_history_stretch = ui->verticalLayout->stretch(ui->verticalLayout->indexOf(ui->frame));
        m_modern_header = GUIUtil::createPageHeader(this, tr("Receive DeFCoN"), tr("Share this address to get paid."));
        m_modern_form = new QWidget(ui->frame2);
        auto* grid = new QGridLayout(m_modern_form);
        grid->setContentsMargins(6, 6, 6, 6);
        grid->setHorizontalSpacing(16);
        grid->setVerticalSpacing(8);
        // No title of its own: it opens from the card's "Request a specific amount".
        buildAddressCard();
    }
    auto* destination = modern ? qobject_cast<QGridLayout*>(m_modern_form->layout()) : ui->gridLayout;
    for (QLabel* label : {ui->label, ui->label_2, ui->label_3}) {
        label->setAlignment(modern ? Qt::AlignLeft | Qt::AlignVCenter : Qt::Alignment(label->property("abyssOriginalAlignment").toInt()));
        GUIUtil::setFont({label}, GUIUtil::FontWeight::Normal, modern ? 13 : 15);
    }
    for (QLayout* child : {static_cast<QLayout*>(ui->horizontalLayout), static_cast<QLayout*>(ui->horizontalLayout_4)}) {
        if (modern) ui->gridLayout->removeItem(child);
        else m_modern_form->layout()->removeItem(child);
        child->setParent(nullptr);
    }
    if (modern) {
        destination->addWidget(ui->label_5, 1, 0, 1, 2);
        destination->addWidget(ui->label_2, 2, 0);
        destination->addWidget(ui->label, 2, 1);
        destination->addWidget(ui->reqLabel, 3, 0);
        destination->addLayout(ui->horizontalLayout_4, 3, 1);
        destination->addWidget(ui->label_3, 4, 0, 1, 2);
        destination->addWidget(ui->reqMessage, 5, 0, 1, 2);
        destination->addLayout(ui->horizontalLayout, 6, 0, 1, 2);
        ui->label_7->hide();
        // The card's toggle names the form; the message stays off the page (a label and
        // an amount are what a request needs). Both come back with the classic layout.
        ui->label_5->hide();
        ui->label_3->hide();
        ui->reqMessage->hide();
        ui->gridLayout->addWidget(m_modern_form, 0, 0, 8, 3);
        m_modern_form->show();
        // The address card first; the form and the history move under it, into the scroll area.
        ui->verticalLayout->removeWidget(ui->frame2);
        ui->verticalLayout->removeItem(ui->verticalSpacer_2);
        ui->verticalLayout->removeWidget(ui->frame);
        m_abyss_column->insertWidget(1, ui->frame2);
        m_abyss_column->insertWidget(2, ui->frame);
        ui->frame2->setVisible(m_request_toggle->isChecked());
        ui->verticalLayout->insertWidget(0, m_modern_header);
        ui->verticalLayout->insertWidget(1, m_abyss_scroll, 1);
        GUIUtil::alignPageHeader(m_modern_header);
        m_modern_header->show();
        m_abyss_scroll->show();
    } else {
        ui->gridLayout->removeWidget(m_modern_form);
        m_modern_form->hide();
        destination->addWidget(ui->label_5, 2, 2);
        destination->addWidget(ui->label_2, 4, 0);
        destination->addWidget(ui->reqLabel, 4, 2);
        destination->addWidget(ui->label, 5, 0);
        destination->addLayout(ui->horizontalLayout_4, 5, 2);
        destination->addWidget(ui->label_3, 6, 0);
        destination->addWidget(ui->reqMessage, 6, 2);
        destination->addLayout(ui->horizontalLayout, 7, 2);
        ui->label_7->show();
        ui->label_5->show();
        ui->label_3->show();
        ui->reqMessage->show();
        ui->verticalLayout->removeWidget(m_modern_header);
        ui->verticalLayout->removeWidget(m_abyss_scroll);
        m_modern_header->hide();
        m_abyss_scroll->hide();
        m_abyss_column->removeWidget(ui->frame2);
        m_abyss_column->removeWidget(ui->frame);
        ui->verticalLayout->insertWidget(0, ui->frame2, m_classic_form_stretch);
        ui->verticalLayout->insertItem(1, ui->verticalSpacer_2);
        ui->verticalLayout->setStretch(1, m_classic_spacer_stretch);
        ui->verticalLayout->insertWidget(2, ui->frame, m_classic_history_stretch);
        ui->frame2->show();
    }
    ui->frame2->style()->unpolish(ui->frame2);
    ui->frame2->style()->polish(ui->frame2);
    // The message field Abyss hides keeps hidden: showing it to keep its focus would undo that.
    if (restore_focus && !(modern && focus == ui->reqMessage) && focus->parentWidget()->isVisibleTo(this)) {
        // Retain hidden-page focus after the layout's queued reparent-show.
        if (!active_focus) focus->show();
        focus->setFocus(Qt::OtherFocusReason);
    }
    updateHistoryState();
    if (modern) {
        m_address_stale = true;
        refreshAddress();
    }
}

void ReceiveCoinsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::StyleChange) {
        updateThemePresentation();
        updateHistoryState();
    } else if (event->type() == QEvent::ParentChange && m_modern_layout) {
        // Built as a window, the page placed its header by a window's margins;
        // in the wallet view it has a child's.
        GUIUtil::alignPageHeader(m_modern_header);
    }
}

void ReceiveCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        _model->getRecentRequestsTableModel()->sort(RecentRequestsTableModel::Date, Qt::DescendingOrder);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &ReceiveCoinsDialog::updateDisplayUnit);
        updateDisplayUnit();

        QTableView* tableView = ui->recentRequestsView;

        tableView->verticalHeader()->hide();
        tableView->setModel(_model->getRecentRequestsTableModel());
        tableView->setAlternatingRowColors(true);
        tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
        tableView->setSelectionMode(QAbstractItemView::ContiguousSelection);
        tableView->setColumnWidth(RecentRequestsTableModel::Date, DATE_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Label, LABEL_COLUMN_WIDTH);
        tableView->setColumnWidth(RecentRequestsTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);
        tableView->horizontalHeader()->setMinimumSectionSize(MINIMUM_COLUMN_WIDTH);
        tableView->horizontalHeader()->setSectionResizeMode(RecentRequestsTableModel::Date, QHeaderView::Interactive);
        tableView->horizontalHeader()->setSectionResizeMode(RecentRequestsTableModel::Label, QHeaderView::Stretch);
        tableView->horizontalHeader()->setSectionResizeMode(RecentRequestsTableModel::Message, QHeaderView::Stretch);
        tableView->horizontalHeader()->setSectionResizeMode(RecentRequestsTableModel::Amount, QHeaderView::Fixed);

        connect(tableView->selectionModel(),
            &QItemSelectionModel::selectionChanged, this,
            &ReceiveCoinsDialog::recentRequestsView_selectionChanged);

        // Set the button to be enabled or disabled based on whether the wallet can give out new addresses.
        ui->receiveButton->setEnabled(model->wallet().canGetAddresses());

        // Enable/disable the receive button if the wallet is now able/unable to give out new addresses.
        connect(model, &WalletModel::canGetAddressesChanged, [this] {
            ui->receiveButton->setEnabled(model->wallet().canGetAddresses());
        });

        RecentRequestsTableModel* requests = _model->getRecentRequestsTableModel();
        connect(requests, &QAbstractItemModel::rowsInserted, this, &ReceiveCoinsDialog::updateHistoryState);
        connect(requests, &QAbstractItemModel::rowsRemoved, this, &ReceiveCoinsDialog::updateHistoryState);
        connect(requests, &QAbstractItemModel::modelReset, this, &ReceiveCoinsDialog::updateHistoryState);

        // Abyss's address card. A payment to the address it shows makes it look for another;
        // a label edited elsewhere shows at once; an unlock may let it get one at last.
        if (TransactionTableModel* transactions = _model->getTransactionTableModel()) {
            connect(transactions, &QAbstractItemModel::rowsInserted, this, [this](const QModelIndex&, int first, int last) {
                if (!m_used_known) return;
                const bool shown_was_paid = !m_address.isEmpty() && !m_used_addresses.contains(m_address);
                noteTransactions(first, last);
                if (shown_was_paid && m_used_addresses.contains(m_address)) refreshAddress();
            });
            connect(transactions, &QAbstractItemModel::modelReset, this, [this] {
                m_used_known = false;
                refreshAddress();
            });
        }
        if (AddressTableModel* addresses = _model->getAddressTableModel()) {
            // Any change of the address list makes the card look again: a changed entry may
            // have changed its purpose too, not only its label. The address it shows stays
            // while it passes (a label edited elsewhere just shows).
            connect(addresses, &QAbstractItemModel::dataChanged, this, &ReceiveCoinsDialog::refreshAddress);
            connect(addresses, &QAbstractItemModel::rowsInserted, this, &ReceiveCoinsDialog::refreshAddress);
            connect(addresses, &QAbstractItemModel::rowsRemoved, this, &ReceiveCoinsDialog::refreshAddress);
            connect(addresses, &QAbstractItemModel::modelReset, this, &ReceiveCoinsDialog::refreshAddress);
        }
        connect(_model, &WalletModel::encryptionStatusChanged, this, &ReceiveCoinsDialog::refreshAddress);
        connect(_model, &WalletModel::canGetAddressesChanged, this, &ReceiveCoinsDialog::refreshAddress);
        // At shutdown the wallet model goes before its page: the card offers nothing of it after.
        connect(_model, &QObject::destroyed, this, [this] {
            m_address.clear();
            m_used_addresses.clear();
            m_used_known = false;
            showAddress();
        });
    }
    m_used_known = false;
    m_used_addresses.clear();
    m_address.clear();
    m_address_stale = true;
    refreshAddress();
    showAddress();
    updateHistoryState();
}

ReceiveCoinsDialog::~ReceiveCoinsDialog()
{
    // Abyss takes the classic spacer out of its layout, which then no longer owns it.
    if (m_modern_layout) delete ui->verticalSpacer_2;
    delete ui;
}

void ReceiveCoinsDialog::clear()
{
    ui->reqAmount->clear();
    ui->reqLabel->setText("");
    ui->reqMessage->setText("");
    updateDisplayUnit();
}

void ReceiveCoinsDialog::reject()
{
    clear();
}

void ReceiveCoinsDialog::accept()
{
    clear();
}

void ReceiveCoinsDialog::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        ui->reqAmount->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    }
}

void ReceiveCoinsDialog::on_receiveButton_clicked()
{
    if(!model || !model->getOptionsModel() || !model->getAddressTableModel() || !model->getRecentRequestsTableModel())
        return;

    QString address;
    QString label = ui->reqLabel->text();
    /* Generate new receiving address */
    address = model->getAddressTableModel()->addRow(AddressTableModel::Receive, label, "");
    // Asking for the passphrase ran an event loop of its own, in which the wallet may have gone.
    if (!model) return;

    switch(model->getAddressTableModel()->getEditStatus())
    {
    case AddressTableModel::EditStatus::OK: {
        // Success
        // Abyss does not show the message field, so nothing typed there earlier goes along unseen.
        SendCoinsRecipient info(address, label,
            ui->reqAmount->value(), m_modern_layout ? QString() : ui->reqMessage->text());
        ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModel(model);
        dialog->setInfo(info);
        dialog->show();

        /* Store request for later reference */
        model->getRecentRequestsTableModel()->addNewRequest(info);
        break;
    }
    case AddressTableModel::EditStatus::WALLET_UNLOCK_FAILURE:
        QMessageBox::critical(this, windowTitle(),
            tr("Could not unlock wallet."),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    case AddressTableModel::EditStatus::KEY_GENERATION_FAILURE:
        QMessageBox::critical(this, windowTitle(),
            tr("Could not generate new address"),
            QMessageBox::Ok, QMessageBox::Ok);
        break;
    // These aren't valid return values for our action
    case AddressTableModel::EditStatus::INVALID_ADDRESS:
    case AddressTableModel::EditStatus::DUPLICATE_ADDRESS:
    case AddressTableModel::EditStatus::NO_CHANGES:
        assert(false);
    }
    clear();
}

void ReceiveCoinsDialog::on_recentRequestsView_doubleClicked(const QModelIndex &index)
{
    const RecentRequestsTableModel *submodel = model->getRecentRequestsTableModel();
    ReceiveRequestDialog *dialog = new ReceiveRequestDialog(this);
    dialog->setModel(model);
    dialog->setInfo(submodel->entry(index.row()).recipient);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void ReceiveCoinsDialog::recentRequestsView_selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    // Enable Show/Remove buttons only if anything is selected.
    bool enable = !ui->recentRequestsView->selectionModel()->selectedRows().isEmpty();
    ui->showRequestButton->setEnabled(enable);
    ui->removeRequestButton->setEnabled(enable);
}

void ReceiveCoinsDialog::on_showRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();

    for (const QModelIndex& index : selection) {
        on_recentRequestsView_doubleClicked(index);
    }
}

void ReceiveCoinsDialog::on_removeRequestButton_clicked()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return;
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return;
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    model->getRecentRequestsTableModel()->removeRows(firstIndex.row(), selection.length(), firstIndex.parent());
}

QModelIndex ReceiveCoinsDialog::selectedRow()
{
    if(!model || !model->getRecentRequestsTableModel() || !ui->recentRequestsView->selectionModel())
        return QModelIndex();
    QModelIndexList selection = ui->recentRequestsView->selectionModel()->selectedRows();
    if(selection.empty())
        return QModelIndex();
    // correct for selection mode ContiguousSelection
    QModelIndex firstIndex = selection.at(0);
    return firstIndex;
}

// copy column of selected row to clipboard
void ReceiveCoinsDialog::copyColumnToClipboard(int column)
{
    QModelIndex firstIndex = selectedRow();
    if (!firstIndex.isValid()) {
        return;
    }
    GUIUtil::setClipboard(model->getRecentRequestsTableModel()->index(firstIndex.row(), column).data(Qt::EditRole).toString());
}

// context menu
void ReceiveCoinsDialog::showMenu(const QPoint &point)
{
    const QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    // disable context menu actions when appropriate
    const RecentRequestsTableModel* const submodel = model->getRecentRequestsTableModel();
    const RecentRequestEntry& req = submodel->entry(sel.row());
    copyLabelAction->setDisabled(req.recipient.label.isEmpty());
    copyMessageAction->setDisabled(req.recipient.message.isEmpty());
    copyAmountAction->setDisabled(req.recipient.amount == 0);

    contextMenu->exec(QCursor::pos());
}

// context menu action: copy URI
void ReceiveCoinsDialog::copyURI()
{
    QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    const RecentRequestsTableModel * const submodel = model->getRecentRequestsTableModel();
    const QString uri = GUIUtil::formatBitcoinURI(submodel->entry(sel.row()).recipient);
    GUIUtil::setClipboard(uri);
}

// context menu action: copy address
void ReceiveCoinsDialog::copyAddress()
{
    const QModelIndex sel = selectedRow();
    if (!sel.isValid()) {
        return;
    }

    const RecentRequestsTableModel* const submodel = model->getRecentRequestsTableModel();
    const QString address = submodel->entry(sel.row()).recipient.address;
    GUIUtil::setClipboard(address);
}

// context menu action: copy label
void ReceiveCoinsDialog::copyLabel()
{
    copyColumnToClipboard(RecentRequestsTableModel::Label);
}

// context menu action: copy message
void ReceiveCoinsDialog::copyMessage()
{
    copyColumnToClipboard(RecentRequestsTableModel::Message);
}

// context menu action: copy amount
void ReceiveCoinsDialog::copyAmount()
{
    copyColumnToClipboard(RecentRequestsTableModel::Amount);
}
