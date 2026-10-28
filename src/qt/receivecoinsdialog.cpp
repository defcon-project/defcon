// Copyright (c) 2011-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/receivecoinsdialog.h>
#include <qt/forms/ui_receivecoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/receiverequestdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/walletmodel.h>

#include <QAction>
#include <QCursor>
#include <QGridLayout>
#include <QMessageBox>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QStyle>
#include <QTextDocument>

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
        m_modern_header = GUIUtil::createPageHeader(this, tr("Receive DeFCoN"), tr("Create a payment request."));
        m_modern_form = new QWidget(ui->frame2);
        auto* grid = new QGridLayout(m_modern_form);
        grid->setContentsMargins(6, 6, 6, 6);
        grid->setHorizontalSpacing(16);
        grid->setVerticalSpacing(8);
        auto* title = new QLabel(tr("Payment request"), m_modern_form);
        GUIUtil::setFont({title}, GUIUtil::FontWeight::Normal, 16);
        grid->addWidget(title, 0, 0, 1, 2);
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
        ui->gridLayout->addWidget(m_modern_form, 0, 0, 8, 3);
        m_modern_form->show();
        ui->verticalLayout->insertWidget(0, m_modern_header);
        m_modern_header->show();
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
        ui->verticalLayout->removeWidget(m_modern_header);
        m_modern_header->hide();
    }
    ui->frame2->style()->unpolish(ui->frame2);
    ui->frame2->style()->polish(ui->frame2);
    if (restore_focus && focus->parentWidget()->isVisibleTo(this)) {
        // Retain hidden-page focus after the layout's queued reparent-show.
        if (!active_focus) focus->show();
        focus->setFocus(Qt::OtherFocusReason);
    }
}

void ReceiveCoinsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::StyleChange) updateThemePresentation();
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
    }
}

ReceiveCoinsDialog::~ReceiveCoinsDialog()
{
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

    switch(model->getAddressTableModel()->getEditStatus())
    {
    case AddressTableModel::EditStatus::OK: {
        // Success
        SendCoinsRecipient info(address, label,
            ui->reqAmount->value(), ui->reqMessage->text());
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
