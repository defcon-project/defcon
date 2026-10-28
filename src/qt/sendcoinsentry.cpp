// Copyright (c) 2011-2019 The Bitcoin Core developers
// Copyright (c) 2014-2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/sendcoinsentry.h>
#include <qt/forms/ui_sendcoinsentry.h>

#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>

#include <QApplication>
#include <QClipboard>
#include <QGridLayout>
#include <QStyle>

SendCoinsEntry::SendCoinsEntry(QWidget* parent) :
    QStackedWidget(parent),
    ui(new Ui::SendCoinsEntry),
    model(nullptr)
{
    ui->setupUi(this);

    GUIUtil::disableMacFocusRect(this);

    setCurrentWidget(ui->SendCoins);

    setButtonIcons();

    // normal dash address field
    GUIUtil::setupAddressWidget(ui->payTo, this, true);

    GUIUtil::setFont({ui->payToLabel,
                     ui->labellLabel,
                     ui->amountLabel,
                     ui->messageLabel}, GUIUtil::FontWeight::Normal, 15);

    GUIUtil::updateFonts();

    // Connect signals
    connect(ui->payAmount, &BitcoinAmountField::valueChanged, this, &SendCoinsEntry::payAmountChanged);
    connect(ui->checkboxSubtractFeeFromAmount, &QCheckBox::toggled, this, &SendCoinsEntry::subtractFeeFromAmountChanged);
    connect(ui->deleteButton, &QPushButton::clicked, this, &SendCoinsEntry::deleteClicked);
    connect(ui->deleteButton_is, &QPushButton::clicked, this, &SendCoinsEntry::deleteClicked);
    connect(ui->deleteButton_s, &QPushButton::clicked, this, &SendCoinsEntry::deleteClicked);
    connect(ui->useAvailableBalanceButton, &QPushButton::clicked, this, &SendCoinsEntry::useAvailableBalanceClicked);
}

void SendCoinsEntry::updateThemePresentation(bool modern)
{
    modern = modern && GUIUtil::isModernTheme();
    if (modern == m_modern_layout) return;
    QWidget* focus = QApplication::focusWidget();
    const bool active_focus = focus && isAncestorOf(focus);
    if (!active_focus) focus = QWidget::focusWidget();
    const bool restore_focus = focus && isAncestorOf(focus) && (active_focus || !focus->isHidden());
    const bool message_hidden = ui->messageLabel->isHidden();
    const bool text_hidden = ui->messageTextLabel->isHidden();
    m_modern_layout = modern;
    setProperty("abyssRecipientCard", modern);
    if (!m_modern_content) {
        m_original_subtract_stretch = ui->horizontalLayoutAmount->stretch(1);
        ui->payAmount->setProperty("abyssOriginalMaximumWidth", ui->payAmount->maximumWidth());
        for (QLabel* label : {ui->payToLabel, ui->labellLabel, ui->amountLabel, ui->messageLabel}) {
            label->setProperty("abyssOriginalAlignment", static_cast<int>(label->alignment()));
        }
        m_modern_content = new QWidget(ui->SendCoins);
        m_modern_content->setObjectName("abyssRecipientContent");
        auto* grid = new QGridLayout(m_modern_content);
        grid->setContentsMargins(6, 6, 6, 6);
        grid->setHorizontalSpacing(12);
        grid->setVerticalSpacing(8);
        auto* title = new QLabel(tr("Recipient"), m_modern_content);
        GUIUtil::setFont({title}, GUIUtil::FontWeight::Normal, 16);
        grid->addWidget(title, 0, 0);
    }
    ui->payAmount->setMaximumWidth(modern ? 300 : ui->payAmount->property("abyssOriginalMaximumWidth").toInt());
    auto* destination = modern ? qobject_cast<QGridLayout*>(m_modern_content->layout()) : ui->gridLayout;
    for (QLabel* label : {ui->payToLabel, ui->labellLabel, ui->amountLabel, ui->messageLabel}) {
        label->setAlignment(modern ? Qt::AlignLeft | Qt::AlignVCenter : Qt::Alignment(label->property("abyssOriginalAlignment").toInt()));
        GUIUtil::setFont({label}, GUIUtil::FontWeight::Normal, modern ? 13 : 15);
    }
    // Move child layouts with their QObject ownership detached.
    for (QLayout* child : {static_cast<QLayout*>(ui->payToLayout), static_cast<QLayout*>(ui->horizontalLayoutAmount)}) {
        if (modern) ui->gridLayout->removeItem(child);
        else m_modern_content->layout()->removeItem(child);
        child->setParent(nullptr);
    }
    if (modern) {
        ui->payToLayout->removeWidget(ui->deleteButton);
        ui->horizontalLayoutAmount->removeWidget(ui->checkboxSubtractFeeFromAmount);
        ui->horizontalLayoutAmount->removeWidget(ui->useAvailableBalanceButton);
        destination->addWidget(ui->deleteButton, 0, 1, Qt::AlignRight);
        destination->addWidget(ui->payToLabel, 1, 0, 1, 2);
        destination->addLayout(ui->payToLayout, 2, 0, 1, 2);
        destination->addWidget(ui->labellLabel, 3, 0, 1, 2);
        destination->addWidget(ui->addAsLabel, 4, 0, 1, 2);
        destination->addWidget(ui->amountLabel, 5, 0, 1, 2);
        destination->addLayout(ui->horizontalLayoutAmount, 6, 0, 1, 2);
        destination->setAlignment(ui->horizontalLayoutAmount, Qt::AlignLeft);
        destination->addWidget(ui->checkboxSubtractFeeFromAmount, 7, 0, 1, 2);
        destination->addWidget(ui->useAvailableBalanceButton, 8, 0, 1, 2, Qt::AlignRight);
        destination->addWidget(ui->messageLabel, 9, 0, 1, 2);
        destination->addWidget(ui->messageTextLabel, 10, 0, 1, 2);
        ui->line->hide();
        ui->gridLayout->addWidget(m_modern_content, 0, 0, 4, 2);
        m_modern_content->show();
    } else {
        ui->gridLayout->removeWidget(m_modern_content);
        m_modern_content->hide();
        m_modern_content->layout()->removeWidget(ui->deleteButton);
        m_modern_content->layout()->removeWidget(ui->checkboxSubtractFeeFromAmount);
        m_modern_content->layout()->removeWidget(ui->useAvailableBalanceButton);
        ui->payToLayout->addWidget(ui->deleteButton);
        ui->horizontalLayoutAmount->addWidget(ui->checkboxSubtractFeeFromAmount);
        ui->horizontalLayoutAmount->addWidget(ui->useAvailableBalanceButton);
        ui->horizontalLayoutAmount->setStretch(1, m_original_subtract_stretch);
        destination->addWidget(ui->payToLabel, 0, 0);
        destination->addLayout(ui->payToLayout, 0, 1);
        destination->addWidget(ui->labellLabel, 1, 0);
        destination->addWidget(ui->addAsLabel, 1, 1);
        destination->addWidget(ui->amountLabel, 2, 0);
        destination->addLayout(ui->horizontalLayoutAmount, 2, 1);
        destination->addWidget(ui->messageLabel, 3, 0);
        destination->addWidget(ui->messageTextLabel, 3, 1);
        ui->line->show();
    }
    // Repolish containers only; labels retain their registered GUIUtil fonts.
    ui->messageLabel->setVisible(!message_hidden);
    ui->messageTextLabel->setVisible(!text_hidden);
    ui->SendCoins->style()->unpolish(ui->SendCoins);
    ui->SendCoins->style()->polish(ui->SendCoins);
    if (restore_focus && focus->parentWidget()->isVisibleTo(this)) {
        // Retain hidden-page focus after the layout's queued reparent-show.
        if (!active_focus) focus->show();
        focus->setFocus(Qt::OtherFocusReason);
    }
}

SendCoinsEntry::~SendCoinsEntry()
{
    delete ui;
}

void SendCoinsEntry::on_pasteButton_clicked()
{
    // Paste text from clipboard into recipient field
    ui->payTo->setText(QApplication::clipboard()->text());
}

void SendCoinsEntry::on_addressBookButton_clicked()
{
    if(!model)
        return;
    AddressBookPage dlg(AddressBookPage::ForSelection, AddressBookPage::SendingTab, this);
    dlg.setModel(model->getAddressTableModel());
    if(dlg.exec())
    {
        ui->payTo->setText(dlg.getReturnValue());
        ui->payAmount->setFocus();
    }
}

void SendCoinsEntry::on_payTo_textChanged(const QString &address)
{
    SendCoinsRecipient rcp;
    if (GUIUtil::parseBitcoinURI(address, &rcp)) {
        ui->payTo->blockSignals(true);
        setValue(rcp);
        ui->payTo->blockSignals(false);
    } else {
        updateLabel(address);
    }
}

void SendCoinsEntry::setModel(WalletModel *_model)
{
    this->model = _model;

    if (_model && _model->getOptionsModel())
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsEntry::updateDisplayUnit);

    clear();
}

void SendCoinsEntry::clear()
{
    // clear UI elements for normal payment
    ui->payTo->clear();
    ui->addAsLabel->clear();
    ui->payAmount->clear();
    if (model && model->getOptionsModel()) {
        ui->checkboxSubtractFeeFromAmount->setChecked(model->getOptionsModel()->getSubFeeFromAmount());
    }
    ui->messageTextLabel->clear();
    ui->messageTextLabel->hide();
    ui->messageLabel->hide();
    // clear UI elements for unauthenticated payment request
    ui->payTo_is->clear();
    ui->memoTextLabel_is->clear();
    ui->payAmount_is->clear();
    // clear UI elements for authenticated payment request
    ui->payTo_s->clear();
    ui->memoTextLabel_s->clear();
    ui->payAmount_s->clear();

    // update the display unit, to not use the default ("BTC")
    updateDisplayUnit();
}

void SendCoinsEntry::checkSubtractFeeFromAmount()
{
    ui->checkboxSubtractFeeFromAmount->setChecked(true);
}

void SendCoinsEntry::deleteClicked()
{
    Q_EMIT removeEntry(this);
}

void SendCoinsEntry::useAvailableBalanceClicked()
{
    Q_EMIT useAvailableBalance(this);
}

bool SendCoinsEntry::validate(interfaces::Node& node)
{
    if (!model)
        return false;

    // Check input validity
    bool retval = true;

    if (!model->validateAddress(ui->payTo->text()))
    {
        ui->payTo->setValid(false);
        retval = false;
    }

    if (!ui->payAmount->validate())
    {
        retval = false;
    }

    // Sending a zero amount is invalid
    if (ui->payAmount->value(nullptr) <= 0)
    {
        ui->payAmount->setValid(false);
        retval = false;
    }

    // Reject dust outputs:
    if (retval && GUIUtil::isDust(node, ui->payTo->text(), ui->payAmount->value())) {
        ui->payAmount->setValid(false);
        retval = false;
    }

    return retval;
}

CAmount SendCoinsEntry::amount() const
{
    return ui->payAmount->value();
}

QWidget* SendCoinsEntry::addressField() const
{
    return ui->payTo;
}

QWidget* SendCoinsEntry::validationTarget() const
{
    return ui->payTo->isValid() ? static_cast<QWidget*>(ui->payAmount) : ui->payTo;
}

SendCoinsRecipient SendCoinsEntry::getValue()
{
    // Normal payment
    recipient.address = ui->payTo->text();
    recipient.label = ui->addAsLabel->text();
    recipient.amount = ui->payAmount->value();
    recipient.message = ui->messageTextLabel->text();
    recipient.fSubtractFeeFromAmount = (ui->checkboxSubtractFeeFromAmount->checkState() == Qt::Checked);

    return recipient;
}

QWidget *SendCoinsEntry::setupTabChain(QWidget *prev)
{
    QWidget::setTabOrder(prev, ui->payTo);
    QWidget::setTabOrder(ui->payTo, ui->addAsLabel);
    QWidget *w = ui->payAmount->setupTabChain(ui->addAsLabel);
    QWidget::setTabOrder(w, ui->checkboxSubtractFeeFromAmount);
    QWidget::setTabOrder(ui->checkboxSubtractFeeFromAmount, ui->addressBookButton);
    QWidget::setTabOrder(ui->addressBookButton, ui->pasteButton);
    QWidget::setTabOrder(ui->pasteButton, ui->deleteButton);
    return ui->deleteButton;
}

void SendCoinsEntry::setValue(const SendCoinsRecipient &value)
{
    recipient = value;
    {
        // message
        ui->messageTextLabel->setText(recipient.message);
        ui->messageTextLabel->setVisible(!recipient.message.isEmpty());
        ui->messageLabel->setVisible(!recipient.message.isEmpty());

        ui->payTo->setText(recipient.address);
        ui->addAsLabel->setText(recipient.label);
        ui->payAmount->setValue(recipient.amount);
    }

    updateLabel(recipient.address);
}

void SendCoinsEntry::setAddress(const QString &address)
{
    ui->payTo->setText(address);
    ui->payAmount->setFocus();
}

void SendCoinsEntry::setAmount(const CAmount &amount)
{
    ui->payAmount->setValue(amount);
}

bool SendCoinsEntry::isClear()
{
    return ui->payTo->text().isEmpty() && ui->payTo_is->text().isEmpty() && ui->payTo_s->text().isEmpty();
}

void SendCoinsEntry::setFocus()
{
    ui->payTo->setFocus();
}

void SendCoinsEntry::updateDisplayUnit()
{
    if(model && model->getOptionsModel())
    {
        // Update payAmount with the current unit
        ui->payAmount->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
        ui->payAmount_is->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
        ui->payAmount_s->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    }
}

void SendCoinsEntry::changeEvent(QEvent* e)
{
    QStackedWidget::changeEvent(e);
    if (e->type() == QEvent::StyleChange) {
        // Adjust button icon colors on theme changes
        setButtonIcons();
    }
}

void SendCoinsEntry::setButtonIcons()
{
    GUIUtil::setIcon(ui->addressBookButton, "address-book");
    GUIUtil::setIcon(ui->pasteButton, "editpaste");
    GUIUtil::setIcon(ui->deleteButton, "remove", GUIUtil::ThemedColor::RED);
    GUIUtil::setIcon(ui->deleteButton_is, "remove", GUIUtil::ThemedColor::RED);
    GUIUtil::setIcon(ui->deleteButton_s, "remove", GUIUtil::ThemedColor::RED);
    const bool modern = GUIUtil::isModernTheme();
    const auto present = [modern](QAbstractButton* button, const char* classic, const char* glyph, GUIUtil::ThemedColor color) {
        if (modern) GUIUtil::setThemedIcon(button, classic, glyph, button->iconSize(), color);
        button->setAccessibleName(modern ? button->toolTip() : QString());
    };
    present(ui->addressBookButton, "address-book", "address-book", GUIUtil::ThemedColor::BLUE);
    present(ui->pasteButton, "editpaste", "clipboard", GUIUtil::ThemedColor::BLUE);
    for (auto* button : {ui->deleteButton, ui->deleteButton_is, ui->deleteButton_s}) {
        present(button, "remove", "trash", GUIUtil::ThemedColor::RED);
    }
}

bool SendCoinsEntry::updateLabel(const QString &address)
{
    if(!model)
        return false;

    // Fill in label from address book, if address has an associated label
    QString associatedLabel = model->getAddressTableModel()->labelForAddress(address);
    if(!associatedLabel.isEmpty())
    {
        ui->addAsLabel->setText(associatedLabel);
        return true;
    }

    return false;
}
