// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/sendcoinsdialog.h>
#include <qt/forms/ui_sendcoinsdialog.h>

#include <qt/addresstablemodel.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/sendcoinsentry.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/interface_ui.h>
#include <policy/fees.h>
#include <txmempool.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/wallet.h>
#include <chrono>

#include <array>
#include <fstream>
#include <memory>

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTextDocument>

#define SEND_CONFIRM_DELAY   3

static constexpr std::array confTargets{2, 4, 6, 12, 24, 48, 144, 504, 1008};
int getConfTargetForIndex(int index) {
    if (index+1 > static_cast<int>(confTargets.size())) {
        return confTargets.back();
    }
    if (index < 0) {
        return confTargets[0];
    }
    return confTargets[index];
}
int getIndexForConfTarget(int target) {
    for (unsigned int i = 0; i < confTargets.size(); i++) {
        if (confTargets[i] >= target) {
            return i;
        }
    }
    return confTargets.size() - 1;
}

SendCoinsDialog::SendCoinsDialog(bool _fCoinJoin, QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::SendCoinsDialog),
    clientModel(nullptr),
    model(nullptr),
    m_coin_control(new CCoinControl),
    fNewRecipientAllowed(true),
    fFeeMinimized(true)
{
    ui->setupUi(this);
    m_coinjoin = _fCoinJoin;

    GUIUtil::setFont({ui->labelCoinControlFeatures,
                      ui->labelCoinControlInsuffFunds,
                      ui->labelCoinControlQuantityText,
                      ui->labelCoinControlBytesText,
                      ui->labelCoinControlAmountText,
                      ui->labelCoinControlLowOutputText,
                      ui->labelCoinControlFeeText,
                      ui->labelCoinControlAfterFeeText,
                      ui->labelCoinControlChangeText,
                      ui->labelFeeHeadline,
                      ui->fallbackFeeWarningLabel
                     }, GUIUtil::FontWeight::Bold);

    GUIUtil::setFont({ui->labelBalance,
                      ui->labelBalanceName,
                     }, GUIUtil::FontWeight::Bold, 14);

    GUIUtil::setFont({ui->labelCoinControlFeatures
                     }, GUIUtil::FontWeight::Bold, 16);

    ui->checkBoxCoinControlChange->setEnabled(!_fCoinJoin);
    GUIUtil::setupAddressWidget(ui->lineEditCoinControlChange, this);

    addEntry();

    GUIUtil::updateFonts();

    connect(ui->addButton, &QPushButton::clicked, this, &SendCoinsDialog::addEntry);
    connect(ui->clearButton, &QPushButton::clicked, this, &SendCoinsDialog::clear);

    // Coin Control
    connect(ui->pushButtonCoinControl, &QPushButton::clicked, this, &SendCoinsDialog::coinControlButtonClicked);
    connect(ui->checkBoxCoinControlChange, &QCheckBox::stateChanged, this, &SendCoinsDialog::coinControlChangeChecked);
    connect(ui->lineEditCoinControlChange, &QValidatedLineEdit::textEdited, this, &SendCoinsDialog::coinControlChangeEdited);

    // Coin Control: clipboard actions
    QAction *clipboardQuantityAction = new QAction(tr("Copy quantity"), this);
    QAction *clipboardAmountAction = new QAction(tr("Copy amount"), this);
    QAction *clipboardFeeAction = new QAction(tr("Copy fee"), this);
    QAction *clipboardAfterFeeAction = new QAction(tr("Copy after fee"), this);
    QAction *clipboardBytesAction = new QAction(tr("Copy bytes"), this);
    QAction *clipboardLowOutputAction = new QAction(tr("Copy dust"), this);
    QAction *clipboardChangeAction = new QAction(tr("Copy change"), this);
    connect(clipboardQuantityAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardQuantity);
    connect(clipboardAmountAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardAmount);
    connect(clipboardFeeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardFee);
    connect(clipboardAfterFeeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardAfterFee);
    connect(clipboardBytesAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardBytes);
    connect(clipboardLowOutputAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardLowOutput);
    connect(clipboardChangeAction, &QAction::triggered, this, &SendCoinsDialog::coinControlClipboardChange);
    ui->labelCoinControlQuantity->addAction(clipboardQuantityAction);
    ui->labelCoinControlAmount->addAction(clipboardAmountAction);
    ui->labelCoinControlFee->addAction(clipboardFeeAction);
    ui->labelCoinControlAfterFee->addAction(clipboardAfterFeeAction);
    ui->labelCoinControlBytes->addAction(clipboardBytesAction);
    ui->labelCoinControlLowOutput->addAction(clipboardLowOutputAction);
    ui->labelCoinControlChange->addAction(clipboardChangeAction);

    // init transaction fee section
    QSettings settings;
    if (!settings.contains("fFeeSectionMinimized"))
        settings.setValue("fFeeSectionMinimized", true);
    if (!settings.contains("nFeeRadio") && settings.contains("nTransactionFee") && settings.value("nTransactionFee").toLongLong() > 0) // compatibility
        settings.setValue("nFeeRadio", 1); // custom
    if (!settings.contains("nFeeRadio"))
        settings.setValue("nFeeRadio", 0); // recommended
    if (!settings.contains("nSmartFeeSliderPosition"))
        settings.setValue("nSmartFeeSliderPosition", 0);
    if (!settings.contains("nTransactionFee"))
        settings.setValue("nTransactionFee", (qint64)DEFAULT_PAY_TX_FEE);
    ui->groupFee->setId(ui->radioSmartFee, 0);
    ui->groupFee->setId(ui->radioCustomFee, 1);
    ui->groupFee->button((int)std::max(0, std::min(1, settings.value("nFeeRadio").toInt())))->setChecked(true);
    ui->customFee->SetAllowEmpty(false);
    ui->customFee->setValue(settings.value("nTransactionFee").toLongLong());
    minimizeFeeSection(settings.value("fFeeSectionMinimized").toBool());

    if (_fCoinJoin) {
        ui->sendButton->setText(tr("S&end mixed funds"));
        ui->sendButton->setToolTip(tr("Confirm the %1 send action").arg(QString::fromStdString(gCoinJoinName)));
    } else {
        ui->sendButton->setText(tr("S&end"));
        ui->sendButton->setToolTip(tr("Confirm the send action"));
    }

    m_coin_control->UseCoinJoin(_fCoinJoin);

    GUIUtil::ExceptionSafeConnect(ui->sendButton, &QPushButton::clicked, this, &SendCoinsDialog::sendButtonClicked);
    m_presentation_ready = true;
    updateThemePresentation();
}

void SendCoinsDialog::setClientModel(ClientModel *_clientModel)
{
    this->clientModel = _clientModel;

    if (_clientModel) {
        connect(_clientModel, &ClientModel::numBlocksChanged, this, &SendCoinsDialog::updateNumberOfBlocks);
    }
}

void SendCoinsDialog::setModel(WalletModel *_model)
{
    this->model = _model;

    if(_model && _model->getOptionsModel())
    {
        for(int i = 0; i < ui->entries->count(); ++i)
        {
            SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
            if(entry)
            {
                entry->setModel(_model);
            }
        }

        interfaces::WalletBalances balances = _model->wallet().getBalances();
        setBalance(balances);
        connect(_model, &WalletModel::balanceChanged, this, &SendCoinsDialog::setBalance);
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::updateDisplayUnit);
        updateDisplayUnit();

        // Coin Control
        connect(_model->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
        connect(_model->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, this, &SendCoinsDialog::coinControlFeatureChanged);
        ui->frameCoinControl->setVisible(_model->getOptionsModel()->getCoinControlFeatures());
        coinControlUpdateLabels();

        connect(_model->getOptionsModel(), &OptionsModel::keepChangeAddressChanged, this, &SendCoinsDialog::keepChangeAddressChanged);
        fKeepChangeAddress = _model->getOptionsModel()->getKeepChangeAddress();

        QSettings settings;
        if (fKeepChangeAddress && settings.contains("sCustomChangeAddress")) {
            ui->checkBoxCoinControlChange->setChecked(true);
            ui->lineEditCoinControlChange->setText(settings.value("sCustomChangeAddress").toString());
            coinControlChangeEdited(ui->lineEditCoinControlChange->text());
        }

        // fee section
        for (const int n : confTargets) {
            ui->confTargetSelector->addItem(tr("%1 (%2 blocks)").arg(GUIUtil::formatNiceTimeOffset(n*Params().GetConsensus().nPowTargetSpacing)).arg(n));
        }
        connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &SendCoinsDialog::updateSmartFeeLabel);
        connect(ui->confTargetSelector, qOverload<int>(&QComboBox::currentIndexChanged), this, &SendCoinsDialog::coinControlUpdateLabels);

        connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::updateFeeSectionControls);
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::coinControlUpdateLabels);

        connect(ui->customFee, &BitcoinAmountField::valueChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
        connect(ui->groupFee, &QButtonGroup::idClicked, this, &SendCoinsDialog::updatePaymentSummary);
        connect(ui->customFee, &BitcoinAmountField::valueChanged, this, &SendCoinsDialog::updatePaymentSummary);
        CAmount requiredFee = model->wallet().getRequiredFee(1000);
        ui->customFee->SetMinValue(requiredFee);
        if (ui->customFee->value() < requiredFee) {
            ui->customFee->setValue(requiredFee);
        }
        updateFeeSectionControls();
        updateSmartFeeLabel();

        if (model->wallet().privateKeysDisabled()) {
            ui->sendButton->setText(tr("Cr&eate Unsigned"));
            ui->sendButton->setToolTip(tr("Creates a Partially Signed Blockchain Transaction (PSBT) for use with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
        }

        // set the smartfee-sliders default value (wallets default conf.target or last stored value)
        if (settings.value("nSmartFeeSliderPosition").toInt() != 0) {
            // migrate nSmartFeeSliderPosition to nConfTarget
            // nConfTarget is available since 0.15 (replaced nSmartFeeSliderPosition)
            int nConfirmTarget = 25 - settings.value("nSmartFeeSliderPosition").toInt(); // 25 == old slider range
            settings.setValue("nConfTarget", nConfirmTarget);
            settings.remove("nSmartFeeSliderPosition");
        }
        if (settings.value("nConfTarget").toInt() == 0)
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(model->wallet().getConfirmTarget()));
        else
            ui->confTargetSelector->setCurrentIndex(getIndexForConfTarget(settings.value("nConfTarget").toInt()));
    }
}

SendCoinsDialog::~SendCoinsDialog()
{
    QSettings settings;
    if (fKeepChangeAddress) {
        settings.setValue("sCustomChangeAddress", ui->lineEditCoinControlChange->text());
    }
    settings.setValue("fFeeSectionMinimized", fFeeMinimized);
    settings.setValue("nFeeRadio", ui->groupFee->checkedId());
    settings.setValue("nConfTarget", getConfTargetForIndex(ui->confTargetSelector->currentIndex()));
    settings.setValue("nTransactionFee", (qint64)ui->customFee->value());

    // removeItem leaves the preserved classic action layout parentless in Abyss.
    // Deleting its layout items does not delete the original child widgets.
    if (!ui->horizontalLayout->parent()) delete ui->horizontalLayout;
    delete ui;
}

bool SendCoinsDialog::PrepareSendText(QString& question_string, QString& informative_text, QString& detailed_text)
{
    QList<SendCoinsRecipient> recipients;
    bool valid = true;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            if(entry->validate(model->node()))
            {
                recipients.append(entry->getValue());
            }
            else if (valid)
            {
                ensureRecipientVisible(entry, true);
                valid = false;
            }
        }
    }

    if(!valid || recipients.isEmpty())
    {
        return false;
    }

    fNewRecipientAllowed = false;
    // request unlock only if was locked or unlocked for mixing:
    // this way we let users unlock by walletpassphrase or by menu
    // and make many transactions while unlocking through this dialog
    // will call relock
    WalletModel::EncryptionStatus encStatus = model->getEncryptionStatus();
    if(encStatus == model->Locked || encStatus == model->UnlockedForMixingOnly)
    {
        WalletModel::UnlockContext ctx(model->requestUnlock());
        if(!ctx.isValid())
        {
            // Unlock wallet was cancelled
            fNewRecipientAllowed = true;
            return false;
        }
        return send(recipients, question_string, informative_text, detailed_text);
    } // WalletModel::UnlockContext

    // already unlocked or not encrypted at all
    return send(recipients, question_string, informative_text, detailed_text);
}

bool SendCoinsDialog::send(const QList<SendCoinsRecipient>& recipients, QString& question_string, QString& informative_text, QString& detailed_text)
{
    // prepare transaction for getting txFee earlier
    m_current_transaction = std::make_unique<WalletModelTransaction>(recipients);
    WalletModel::SendCoinsReturn prepareStatus;

    updateCoinControlState();

    prepareStatus = model->prepareTransaction(*m_current_transaction, *m_coin_control);

    // process prepareStatus and on error generate message shown to user
    processSendCoinsReturn(prepareStatus,
        BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), m_current_transaction->getTransactionFee()));

    if(prepareStatus.status != WalletModel::OK) {
        fNewRecipientAllowed = true;
          return false;
    }

    QStringList formatted;
    for (const SendCoinsRecipient &rcp : m_current_transaction->getRecipients())
    {
        // generate amount string with wallet name in case of multiwallet
        QString amount = BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), rcp.amount);
        if (model->isMultiwallet()) {
            amount.append(tr(" from wallet '%1'").arg(model->getWalletName()));
        }

        // generate address string
        QString address = rcp.address;

        QString recipientElement;

        {
            if(rcp.label.length() > 0) // label with address
            {
                recipientElement.append(tr("%1 to '%2'").arg(amount, rcp.label));
                recipientElement.append(QString(" (%1)").arg(address));
            }
            else // just address
            {
                recipientElement.append(tr("%1 to %2").arg(amount, address));
            }
        }
        formatted.append(recipientElement);
    }

    // Limit number of displayed entries
    QStringList formatted_short(formatted);
    if (formatted_short.size() > MAX_SEND_POPUP_ENTRIES) {
        formatted_short.erase(formatted_short.begin() + MAX_SEND_POPUP_ENTRIES, formatted_short.end());
    }

    if (model->wallet().privateKeysDisabled()) {
        question_string.append(tr("Do you want to draft this transaction?"));
    } else {
        question_string.append(tr("Are you sure you want to send?"));
    }
    if (model->wallet().privateKeysDisabled()) {
        question_string.append("<br /><span style='font-size:10pt;'>");
        question_string.append(tr("This will produce a Partially Signed Transaction (PSBT) which you can save or copy and then sign with e.g. an offline %1 wallet, or a PSBT-compatible hardware wallet.").arg(PACKAGE_NAME));
        question_string.append("</span>");
    }
    question_string.append("<br /><br />");
    question_string.append(formatted_short.join("<br />"));
    question_string.append("<br />");

    QString strCoinJoinName = QString::fromStdString(gCoinJoinName);

    if(m_coin_control->IsUsingCoinJoin()) {
        question_string.append(tr("using") + " <b>" + tr("%1 funds only").arg(strCoinJoinName) + "</b>");
    } else {
        question_string.append(tr("using") + " <b>" + tr("any available funds") + "</b>");
    }

    int messageEntries = formatted.size();
    int displayedEntries = formatted_short.size();

    if (displayedEntries < messageEntries) {
        question_string.append("<br />");
        question_string.append("<span style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING) + "'>");
        question_string.append(tr("<b>(%1 of %2 entries displayed)</b>").arg(displayedEntries).arg(messageEntries));
        question_string.append("</span>");
    }

    CAmount txFee = m_current_transaction->getTransactionFee();

    if(txFee > 0)
    {
        // append fee string if a fee is required
        question_string.append("<hr /><b>");
        question_string.append(QString("<b>%1</b>: <span style='%2'>%3</span>").arg(tr("Transaction fee"))
            .arg(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR))
            .arg(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), txFee)));

        if (m_coin_control->IsUsingCoinJoin()) {
            question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>%1</span>")
                .arg(tr("(%1 transactions have higher fees usually due to no change output being allowed)").arg(strCoinJoinName)));
        }
    }

    // Show some additioinal information
    question_string.append("<hr />");
    // append transaction size
    question_string.append(tr("Transaction size: %1").arg(QString::number((double)m_current_transaction->getTransactionSize() / 1000)) + " kB");
    question_string.append("<br />");
    CFeeRate feeRate(txFee, m_current_transaction->getTransactionSize());
    question_string.append(tr("Fee rate: %1").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), feeRate.GetFeePerK())) + "/kB");

    if (m_coin_control->IsUsingCoinJoin()) {
        // append number of inputs
        question_string.append("<hr />");
        int nInputs = m_current_transaction->getWtx()->vin.size();
        question_string.append(tr("This transaction will consume %n input(s)", "", nInputs));

        // warn about potential privacy issues when spending too many inputs at once
        if (nInputs >= 10 && m_coin_control->IsUsingCoinJoin()) {
            question_string.append("<br />");
            question_string.append("<span style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_WARNING) + "'>");
            question_string.append(tr("Warning: Using %1 with %2 or more inputs can harm your privacy and is not recommended").arg(strCoinJoinName).arg(10));
            question_string.append("<a style='" + GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_COMMAND) + "' href=\"https://docs.dash.org/en/stable/wallets/dashcore/coinjoin-instantsend.html#inputs\">");
            question_string.append(tr("Click to learn more"));
            question_string.append("</a>");
            question_string.append("</span> ");
        }
    }

    // add total amount in all subdivision units
    question_string.append("<hr />");
    CAmount totalAmount = m_current_transaction->getTotalTransactionAmount() + txFee;
    QStringList alternativeUnits;
    for (const BitcoinUnits::Unit u : BitcoinUnits::availableUnits())
    {
        if(u != model->getOptionsModel()->getDisplayUnit())
            alternativeUnits.append(BitcoinUnits::formatHtmlWithUnit(u, totalAmount));
    }

    // Show total amount + all alternative units
    question_string.append(QString("<b>%1</b>: <b>%2</b>").arg(tr("Total Amount"))
        .arg(BitcoinUnits::formatHtmlWithUnit(model->getOptionsModel()->getDisplayUnit(), totalAmount)));
    question_string.append(QString("<br /><span style='font-size:10pt; font-weight:normal;'>(=%1)</span>")
        .arg(alternativeUnits.join(" " + tr("or") + " ")));

    if (formatted.size() > 1) {
        informative_text = tr("To review recipient list click \"Show Details…\"");
        detailed_text = formatted.join("\n\n");
    }

    return true;
}

void SendCoinsDialog::sendButtonClicked([[maybe_unused]] bool checked)
{
    if(!model || !model->getOptionsModel())
        return;

    QString question_string, informative_text, detailed_text;
    if (!PrepareSendText(question_string, informative_text, detailed_text)) return;
    assert(m_current_transaction);

    const QString confirmation = model->wallet().privateKeysDisabled() ? tr("Confirm transaction proposal") : tr("Confirm send coins");
    const QString confirmButtonText = model->wallet().privateKeysDisabled() ? tr("Create Unsigned") : tr("Send");
    SendConfirmationDialog confirmationDialog(confirmation, question_string, informative_text, detailed_text, SEND_CONFIRM_DELAY, confirmButtonText, this);
    confirmationDialog.exec();
    QMessageBox::StandardButton retval = static_cast<QMessageBox::StandardButton>(confirmationDialog.result());

    if(retval != QMessageBox::Yes)
    {
        fNewRecipientAllowed = true;
        return;
    }

    bool send_failure = false;
    if (model->wallet().privateKeysDisabled()) {
        CMutableTransaction mtx = CMutableTransaction{*(m_current_transaction->getWtx())};
        PartiallySignedTransaction psbtx(mtx);
        bool complete = false;
        const TransactionError err = model->wallet().fillPSBT(SIGHASH_ALL, false /* sign */, true /* bip32derivs */, nullptr, psbtx, complete);
        assert(!complete);
        assert(err == TransactionError::OK);
        // Serialize the PSBT
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << psbtx;
        GUIUtil::setClipboard(EncodeBase64(ssTx.str()).c_str());
        QMessageBox msgBox;
        msgBox.setText("Unsigned Transaction");
        msgBox.setInformativeText("The PSBT has been copied to the clipboard. You can also save it.");
        msgBox.setStandardButtons(QMessageBox::Save | QMessageBox::Discard);
        msgBox.setDefaultButton(QMessageBox::Discard);
        switch (msgBox.exec()) {
        case QMessageBox::Save: {
            QString selectedFilter;
            QString fileNameSuggestion = "";
            bool first = true;
            for (const SendCoinsRecipient &rcp : m_current_transaction->getRecipients()) {
                if (!first) {
                    fileNameSuggestion.append(" - ");
                }
                QString labelOrAddress = rcp.label.isEmpty() ? rcp.address : rcp.label;
                QString amount = BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), rcp.amount);
                fileNameSuggestion.append(labelOrAddress + "-" + amount);
                first = false;
            }
            fileNameSuggestion.append(".psbt");
            QString filename = GUIUtil::getSaveFileName(this,
                tr("Save Transaction Data"), fileNameSuggestion,
                //: Expanded name of the binary PSBT file format. See: BIP 174.
                tr("Partially Signed Transaction (Binary)") + QLatin1String(" (*.psbt)"), &selectedFilter);
            if (filename.isEmpty()) {
                return;
            }
            std::ofstream out{filename.toLocal8Bit().data(), std::ofstream::out | std::ofstream::binary};
            out << ssTx.str();
            out.close();
            Q_EMIT message(tr("PSBT saved"), "PSBT saved to disk", CClientUIInterface::MSG_INFORMATION);
            break;
        }
        case QMessageBox::Discard:
            break;
        default:
            assert(false);
        }
    } else {
        // now send the prepared transaction
        WalletModel::SendCoinsReturn sendStatus = model->sendCoins(*m_current_transaction, m_coin_control->IsUsingCoinJoin());
        // process sendStatus and on error generate message shown to user
        processSendCoinsReturn(sendStatus);

        if (sendStatus.status == WalletModel::OK) {
            Q_EMIT coinsSent(m_current_transaction->getWtx()->GetHash());
        } else {
            send_failure = true;
        }
    }
    if (!send_failure) {
        accept();
        m_coin_control->UnSelectAll();
        coinControlUpdateLabels();
    }
    fNewRecipientAllowed = true;
    m_current_transaction.reset();
}

void SendCoinsDialog::clear()
{
    m_current_transaction.reset();

    // Clear coin control settings
    m_coin_control->UnSelectAll();
    if (!fKeepChangeAddress) {
        ui->checkBoxCoinControlChange->setChecked(false);
        ui->lineEditCoinControlChange->clear();
    }
    coinControlUpdateLabels();

    // Remove entries until only one left
    while(ui->entries->count())
    {
        ui->entries->takeAt(0)->widget()->deleteLater();
    }
    addEntry();

    updateTabsAndLabels();
}

void SendCoinsDialog::reject()
{
    clear();
}

void SendCoinsDialog::accept()
{
    clear();
}

SendCoinsEntry *SendCoinsDialog::addEntry()
{
    SendCoinsEntry* entry = new SendCoinsEntry(this);
    entry->setModel(model);
    ui->entries->addWidget(entry);
    connect(entry, &SendCoinsEntry::removeEntry, this, &SendCoinsDialog::removeEntry);
    connect(entry, &SendCoinsEntry::useAvailableBalance, this, &SendCoinsDialog::useAvailableBalance);
    connect(entry, &SendCoinsEntry::payAmountChanged, this, &SendCoinsDialog::coinControlUpdateLabels);
    connect(entry, &SendCoinsEntry::payAmountChanged, this, &SendCoinsDialog::updatePaymentSummary);
    connect(entry, &SendCoinsEntry::subtractFeeFromAmountChanged, this, &SendCoinsDialog::coinControlUpdateLabels);

    // Focus the field, so that entry can start immediately
    entry->clear();
    entry->setFocus();
    if (!m_modern_layout) {
        // Preserve the classic event/scroll order.
        ui->scrollAreaWidgetContents->resize(ui->scrollAreaWidgetContents->sizeHint());
        qApp->processEvents();
        if (QScrollBar* bar = ui->scrollArea->verticalScrollBar()) {
            bar->setSliderPosition(bar->maximum());
        }
    }
    entry->updateThemePresentation(GUIUtil::isModernTheme() && !m_coinjoin);
    updateTabsAndLabels();
    if (m_modern_layout) {
        qApp->processEvents();
        ensureRecipientVisible(entry, false);
    }
    updatePaymentSummary();
    return entry;
}

//! Let the form decide how small this page may become.
//!
//! The recipients live in a QScrollArea, whose whole purpose is to report
//! almost no minimum of its own: scroll rather than grow. That is the right
//! default, and it is why the Send page could be squeezed until the address
//! and amount fields were out of sight while the window happily got smaller.
//! In the modern theme the window is meant to be resized to the wallet rather
//! than the wallet scrolled inside a window too small for it, so the area is
//! asked to stand for what it holds.
//!
//! It has to be recomputed, not set once: a second recipient makes the form
//! taller, and so does switching Coin Control Features on. Nothing is clamped
//! here -- BitcoinGUI holds the window under the screen, and a scroll area
//! given less than it asked for degrades gracefully by doing its own job again.
void SendCoinsDialog::updateThemePresentation()
{
    if (!m_presentation_ready || m_updating_presentation) return;
    QScopedValueRollback<bool> guard(m_updating_presentation, true);
    QWidget* focus = QApplication::focusWidget();
    const bool active_focus = focus && isAncestorOf(focus);
    if (!active_focus) focus = QWidget::focusWidget();
    const bool restore_focus = focus && isAncestorOf(focus) && (active_focus || !focus->isHidden());
    const bool modern = GUIUtil::isModernTheme() && !m_coinjoin;
    setProperty("abyssPaymentPage", modern);
    if (modern != m_modern_layout) {
        if (!m_modern_header) {
            for (QLabel* label : {ui->fallbackFeeWarningLabel, ui->labelSmartFee2, ui->labelCustomFeeWarning}) {
                label->setProperty("abyssOriginalWordWrap", label->wordWrap());
            }
            m_original_margins = ui->verticalLayout->contentsMargins();
            m_original_fee_spacer = ui->verticalSpacerFee->sizeHint();
            m_original_fee_spacer_policy = ui->verticalSpacerFee->sizePolicy();
            m_original_control_spacer = ui->verticalSpacerCoinControl->sizeHint();
            m_original_control_spacer_policy = ui->verticalSpacerCoinControl->sizePolicy();
            m_original_spacing = ui->verticalLayout->spacing();
            m_original_recipient_scroll_policy = ui->scrollArea->verticalScrollBarPolicy();
            m_original_scroll_stretch = ui->verticalLayout->stretch(ui->verticalLayout->indexOf(ui->scrollArea));
            m_modern_header = GUIUtil::createPageHeader(this, tr("Send DeFCoN"), tr("Send to one or more recipients."));
            m_modern_body = new QWidget(this);
            auto* body = new QHBoxLayout(m_modern_body);
            body->setContentsMargins(0, 0, 0, 0);
            body->setSpacing(12);
            auto* fields = new QWidget(m_modern_body);
            fields->setObjectName("abyssSendFields");
            auto* left = new QVBoxLayout(fields);
            left->setContentsMargins(0, 0, 0, 0);
            left->setSpacing(12);
            auto* field_scroll = new QScrollArea(m_modern_body);
            m_modern_scroll = field_scroll;
            field_scroll->setObjectName("abyssSendFieldsScroll");
            field_scroll->setFrameShape(QFrame::NoFrame);
            field_scroll->setWidgetResizable(true);
            field_scroll->setWidget(fields);
            auto* summary = new QFrame(m_modern_body);
            m_modern_summary = summary;
            summary->setObjectName("abyssSendSummary");
            summary->setMinimumWidth(200);
            summary->setMaximumWidth(260);
            auto* right = new QVBoxLayout(summary);
            right->setContentsMargins(16, 16, 16, 16);
            right->setSpacing(12);
            auto* title = new QLabel(tr("Payment summary"), summary);
            title->setWordWrap(true);
            GUIUtil::setFont({title}, GUIUtil::FontWeight::Normal, 16);
            right->addWidget(title);
            m_summary_balance_name = new QLabel(summary);
            m_summary_balance = new QLabel(summary);
            m_summary_balance_name->setWordWrap(true);
            m_summary_balance->setWordWrap(true);
            m_summary_balance->setTextInteractionFlags(ui->labelBalance->textInteractionFlags());
            GUIUtil::setFont({m_summary_balance_name}, GUIUtil::FontWeight::Normal, 12);
            GUIUtil::setFont({m_summary_balance}, GUIUtil::FontWeight::Normal, 14);
            right->addWidget(m_summary_balance_name);
            right->addWidget(m_summary_balance);
            const auto addValue = [&](const QString& caption, const char* name) {
                auto* text = new QLabel(caption, summary);
                text->setWordWrap(true);
                text->setObjectName("abyssSummaryCaption");
                auto* value = new QLabel(summary);
                value->setObjectName(name);
                value->setWordWrap(true);
                GUIUtil::setFont({text}, GUIUtil::FontWeight::Normal, 12);
                GUIUtil::setFont({value}, GUIUtil::FontWeight::Normal, 13);
                right->addWidget(text);
                right->addWidget(value);
                return value;
            };
            m_summary_recipients = addValue(tr("Recipients"), "abyssRecipientCount");
            m_summary_amount = addValue(tr("Amount entered"), "abyssEnteredAmount");
            m_summary_fee = addValue(tr("Fee rate"), "abyssFeeRate");
            auto* note = new QLabel(tr("The final transaction fee is shown in the confirmation dialog."), summary);
            note->setObjectName("abyssSummaryNote");
            note->setWordWrap(true);
            GUIUtil::setFont({note}, GUIUtil::FontWeight::Normal, 12);
            right->addWidget(note);
            right->addWidget(ui->sendButton);
            body->addWidget(field_scroll, 1);
            body->addWidget(summary, 0, Qt::AlignTop);
        }
        auto* left = qobject_cast<QVBoxLayout*>(m_modern_body->findChild<QWidget*>("abyssSendFields")->layout());
        auto* right = qobject_cast<QVBoxLayout*>(m_modern_summary->layout());
        for (QLabel* label : {ui->fallbackFeeWarningLabel, ui->labelSmartFee2, ui->labelCustomFeeWarning}) {
            label->setWordWrap(modern || label->property("abyssOriginalWordWrap").toBool());
        }
        ui->verticalSpacerFee->changeSize(modern ? 0 : m_original_fee_spacer.width(),
            m_original_fee_spacer.height(), m_original_fee_spacer_policy.horizontalPolicy(),
            m_original_fee_spacer_policy.verticalPolicy());
        ui->frameFee->layout()->invalidate();
        ui->verticalSpacerCoinControl->changeSize(modern ? 0 : m_original_control_spacer.width(),
            m_original_control_spacer.height(), m_original_control_spacer_policy.horizontalPolicy(),
            m_original_control_spacer_policy.verticalPolicy());
        ui->frameCoinControl->layout()->invalidate();
        // Recommended/custom controls stack in the narrower Abyss column.
        for (QLayout* part : {static_cast<QLayout*>(ui->verticalLayoutFee4), static_cast<QLayout*>(ui->verticalLayoutFee3),
                 static_cast<QLayout*>(ui->verticalLayoutFee9), static_cast<QLayout*>(ui->verticalLayoutFee8)}) {
            ui->gridLayoutFee->removeItem(part);
            part->setParent(nullptr);
        }
        if (modern) {
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee4, 0, 0, 1, 2);
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee3, 1, 0, 1, 2);
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee9, 2, 0, 1, 2);
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee8, 3, 0, 1, 2);
        } else {
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee4, 0, 0);
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee3, 0, 1);
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee9, 1, 0);
            ui->gridLayoutFee->addLayout(ui->verticalLayoutFee8, 1, 1);
        }
        if (modern) {
            // Preserve the original action layout, including its spacer/stretch.
            ui->verticalLayout->removeItem(ui->horizontalLayout);
            // Detached while modern; the destructor also owns this case.
            ui->horizontalLayout->setParent(nullptr);
            ui->horizontalLayout->removeWidget(ui->sendButton);
            ui->horizontalLayout->removeWidget(ui->clearButton);
            ui->horizontalLayout->removeWidget(ui->addButton);
            qobject_cast<QHBoxLayout*>(m_modern_header->layout())->addWidget(ui->clearButton);
            // One outer viewport owns scrolling in Abyss. Keeping the original
            // inner viewport here clips growing recipient cards after font changes.
            ui->scrollArea->takeWidget();
            ui->verticalLayout->removeWidget(ui->scrollArea);
            ui->verticalLayout->removeWidget(ui->frameFee);
            ui->verticalLayout->removeWidget(ui->frameCoinControl);
            ui->scrollArea->hide();
            left->addWidget(ui->scrollAreaWidgetContents);
            ui->scrollAreaWidgetContents->show();
            left->addWidget(ui->addButton, 0, Qt::AlignLeft);
            left->addWidget(ui->frameFee);
            left->addWidget(ui->frameCoinControl);
            ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            if (ui->sendButton->parentWidget() != m_modern_summary) right->addWidget(ui->sendButton);
            // Mirror the formatted balance without moving the classic labels.
            ui->labelBalanceName->hide();
            ui->labelBalance->hide();
            ui->verticalLayout->insertWidget(0, m_modern_header);
            ui->verticalLayout->insertWidget(1, m_modern_body, 1);
            ui->horizontalLayoutCoinControl3->setDirection(QBoxLayout::TopToBottom);
            ui->horizontalLayoutCoinControl4->setDirection(QBoxLayout::TopToBottom);
            ui->horizontalLayoutSmartFee->setDirection(QBoxLayout::TopToBottom);
            ui->horizontalLayoutFee1->setDirection(QBoxLayout::TopToBottom);
            ui->horizontalLayoutFee9->setDirection(QBoxLayout::TopToBottom);
            ui->horizontalLayoutFee12->setDirection(QBoxLayout::TopToBottom);
            ui->horizontalLayoutFee13->setDirection(QBoxLayout::TopToBottom);
            ui->verticalLayout->setContentsMargins(16, 16, 16, 16);
            ui->verticalLayout->setSpacing(12);
            m_modern_header->show();
            m_modern_body->show();
        } else {
            // Move the same controls back; never substitute actions or values.
            ui->verticalLayout->removeWidget(m_modern_header);
            ui->verticalLayout->removeWidget(m_modern_body);
            m_modern_header->hide();
            m_modern_body->hide();
            left->removeWidget(ui->frameCoinControl);
            ui->verticalLayout->insertWidget(0, ui->frameCoinControl);
            ui->scrollArea->setVerticalScrollBarPolicy(m_original_recipient_scroll_policy);
            ui->horizontalLayoutCoinControl3->setDirection(QBoxLayout::LeftToRight);
            ui->horizontalLayoutCoinControl4->setDirection(QBoxLayout::LeftToRight);
            left->removeWidget(ui->scrollAreaWidgetContents);
            ui->scrollArea->setWidget(ui->scrollAreaWidgetContents);
            ui->scrollArea->show();
            left->removeWidget(ui->addButton);
            left->removeWidget(ui->frameFee);
            m_modern_header->layout()->removeWidget(ui->clearButton);
            right->removeWidget(ui->sendButton);
            ui->verticalLayout->insertWidget(1, ui->scrollArea, m_original_scroll_stretch);
            ui->verticalLayout->insertWidget(2, ui->frameFee);
            ui->horizontalLayout->insertWidget(0, ui->sendButton);
            ui->horizontalLayout->insertWidget(1, ui->clearButton);
            ui->horizontalLayout->insertWidget(2, ui->addButton);
            ui->labelBalanceName->show();
            ui->labelBalance->show();
            ui->horizontalLayout->setParent(nullptr);
            ui->verticalLayout->addLayout(ui->horizontalLayout);
            ui->horizontalLayoutSmartFee->setDirection(QBoxLayout::LeftToRight);
            ui->horizontalLayoutFee1->setDirection(QBoxLayout::LeftToRight);
            ui->horizontalLayoutFee9->setDirection(QBoxLayout::LeftToRight);
            ui->horizontalLayoutFee12->setDirection(QBoxLayout::LeftToRight);
            ui->horizontalLayoutFee13->setDirection(QBoxLayout::LeftToRight);
            ui->verticalLayout->setContentsMargins(m_original_margins);
            ui->verticalLayout->setSpacing(m_original_spacing);
        }
        m_modern_layout = modern;
    }
    for (int i = 0; i < ui->entries->count(); ++i) {
        if (auto* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget())) {
            entry->updateThemePresentation(modern);
        }
    }
    if (modern) {
        // Dynamic selector properties change after the stylesheet's first polish.
        for (QWidget* part : {static_cast<QWidget*>(ui->frameFee), static_cast<QWidget*>(m_modern_scroll),
                 m_modern_scroll->viewport(), m_modern_scroll->widget(), ui->scrollAreaWidgetContents}) {
            part->style()->unpolish(part);
            part->style()->polish(part);
        }
    }
    setupTabChain(nullptr);
    applyScrollAreaMinimum();
    updatePaymentSummary();
    if (restore_focus && !(modern && (focus == ui->labelBalance || focus == ui->labelBalanceName))
        && focus->parentWidget()->isVisibleTo(this)) {
        // A hidden page also remembers its last input. Resolve only that
        // input's queued reparent-show; intentionally hidden containers stay so.
        if (!active_focus) focus->show();
        focus->setFocus(Qt::OtherFocusReason);
    }
}

void SendCoinsDialog::updatePaymentSummary()
{
    if (!m_modern_layout || !m_summary_amount) return;
    m_summary_balance_name->setText(ui->labelBalanceName->text());
    m_summary_balance->setText(ui->labelBalance->text());
    CAmount amount{0};
    bool valid{true};
    int count{0};
    for (int i = 0; i < ui->entries->count(); ++i) {
        if (auto* entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget())) {
            if (entry->isHidden()) continue; // Removed entries await deferred deletion.
            ++count;
            const CAmount value = entry->amount();
            if (!MoneyRange(value) || value > MAX_MONEY - amount) valid = false;
            else if (valid) amount += value;
        }
    }
    const int unit = model && model->getOptionsModel() ? model->getOptionsModel()->getDisplayUnit() : BitcoinUnits::DASH;
    m_summary_recipients->setText(QString::number(count));
    m_summary_amount->setText(valid ? BitcoinUnits::formatWithUnit(unit, amount) : QString(QChar(0x2013)));
    m_summary_fee->setText(ui->radioSmartFee->isChecked() ? ui->labelSmartFee->text()
        : BitcoinUnits::formatWithUnit(unit, ui->customFee->value()) + "/kB");
}

void SendCoinsDialog::applyScrollAreaMinimum()
{
    if (ui->scrollArea == nullptr || ui->scrollAreaWidgetContents == nullptr) {
        return;
    }
    if (!m_inherited_scroll_minimum.isValid()) {
        m_inherited_scroll_minimum = ui->scrollArea->minimumSize();
    }
    if (m_modern_layout) {
        // The original contents now live directly in the outer viewport.
        // Their layout supplies its scrollable size, including font growth.
        return;
    }
    if (!GUIUtil::isModernTheme()) {
        ui->scrollArea->setMinimumSize(m_inherited_scroll_minimum);
        return;
    }
    if (QLayout* held = ui->scrollAreaWidgetContents->layout()) {
        held->invalidate();
    }
    const QSize wanted = ui->scrollAreaWidgetContents->minimumSizeHint().expandedTo(m_inherited_scroll_minimum);
    // The right-hand actions stay visible while recipients scroll vertically.
    if (ui->scrollArea->minimumSize() != wanted) {
        ui->scrollArea->setMinimumSize(wanted);
    }
}

QScrollArea* SendCoinsDialog::recipientScrollArea() const
{
    return m_modern_layout ? m_modern_scroll : ui->scrollArea;
}

void SendCoinsDialog::ensureRecipientVisible(SendCoinsEntry* entry, bool validation)
{
    if (m_modern_layout) {
        // Refresh the outer viewport's range after a card changes size, before
        // ensureWidgetVisible reads its scrollbar bounds and input geometry.
        ui->entries->invalidate();
        ui->scrollAreaWidgetContents->updateGeometry();
        m_modern_scroll->widget()->layout()->invalidate();
        QEvent request(QEvent::LayoutRequest);
        QApplication::sendEvent(m_modern_scroll, &request);
        recipientScrollArea()->ensureWidgetVisible(validation ? entry->validationTarget() : entry->addressField());
    } else {
        recipientScrollArea()->ensureWidgetVisible(entry);
    }
}

void SendCoinsDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::StyleChange) {
        updateThemePresentation();
        applyScrollAreaMinimum();
    }
}

void SendCoinsDialog::updateTabsAndLabels()
{
    updatePaymentSummary();
    setupTabChain(nullptr);
    coinControlUpdateLabels();
    // A recipient was added or removed: the form is a different height now.
    applyScrollAreaMinimum();
}

void SendCoinsDialog::removeEntry(SendCoinsEntry* entry)
{
    entry->hide();

    // If the last entry is about to be removed add an empty one
    if (ui->entries->count() == 1)
        addEntry();

    entry->deleteLater();

    updateTabsAndLabels();
}

QWidget *SendCoinsDialog::setupTabChain(QWidget *prev)
{
    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry)
        {
            prev = entry->setupTabChain(prev);
        }
    }
    QWidget::setTabOrder(prev, ui->sendButton);
    QWidget::setTabOrder(ui->sendButton, ui->clearButton);
    QWidget::setTabOrder(ui->clearButton, ui->addButton);
    return ui->addButton;
}

void SendCoinsDialog::setAddress(const QString &address)
{
    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setAddress(address);
}

void SendCoinsDialog::pasteEntry(const SendCoinsRecipient &rv)
{
    if(!fNewRecipientAllowed)
        return;

    SendCoinsEntry *entry = nullptr;
    // Replace the first entry if it is still unused
    if(ui->entries->count() == 1)
    {
        SendCoinsEntry *first = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(0)->widget());
        if(first->isClear())
        {
            entry = first;
        }
    }
    if(!entry)
    {
        entry = addEntry();
    }

    entry->setValue(rv);
    updateTabsAndLabels();
}

bool SendCoinsDialog::handlePaymentRequest(const SendCoinsRecipient &rv)
{
    // Just paste the entry, all pre-checks
    // are done in paymentserver.cpp.
    pasteEntry(rv);
    return true;
}

void SendCoinsDialog::setBalance(const interfaces::WalletBalances& balances)
{
    if(model && model->getOptionsModel())
    {
        CAmount balance = 0;
        if (model->wallet().privateKeysDisabled()) {
            balance = balances.watch_only_balance;
            ui->labelBalanceName->setText(tr("Watch-only balance:"));
        } else if (m_coin_control->IsUsingCoinJoin()) {
            balance = balances.anonymized_balance;
        } else {
            balance = balances.balance;
        }

        ui->labelBalance->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), balance));
        updatePaymentSummary();
    }
}

void SendCoinsDialog::updateDisplayUnit()
{
    setBalance(model->wallet().getBalances());
    coinControlUpdateLabels();
    ui->customFee->setDisplayUnit(model->getOptionsModel()->getDisplayUnit());
    updateSmartFeeLabel();
    updatePaymentSummary();
}

void SendCoinsDialog::processSendCoinsReturn(const WalletModel::SendCoinsReturn &sendCoinsReturn, const QString &msgArg)
{
    QPair<QString, CClientUIInterface::MessageBoxFlags> msgParams;
    // Default to a warning message, override if error message is needed
    msgParams.second = CClientUIInterface::MSG_WARNING;

    // This comment is specific to SendCoinsDialog usage of WalletModel::SendCoinsReturn.
    // All status values are used only in WalletModel::prepareTransaction()
    switch(sendCoinsReturn.status)
    {
    case WalletModel::InvalidAddress:
        msgParams.first = tr("The recipient address is not valid. Please recheck.");
        break;
    case WalletModel::InvalidAmount:
        msgParams.first = tr("The amount to pay must be larger than 0.");
        break;
    case WalletModel::AmountExceedsBalance:
        msgParams.first = tr("The amount exceeds your balance.");
        break;
    case WalletModel::AmountWithFeeExceedsBalance:
        msgParams.first = tr("The total exceeds your balance when the %1 transaction fee is included.").arg(msgArg);
        break;
    case WalletModel::DuplicateAddress:
        msgParams.first = tr("Duplicate address found: addresses should only be used once each.");
        break;
    case WalletModel::TransactionCreationFailed:
        msgParams.first = tr("Transaction creation failed!");
        msgParams.second = CClientUIInterface::MSG_ERROR;
        break;
    case WalletModel::AbsurdFee:
        msgParams.first = tr("A fee higher than %1 is considered an absurdly high fee.").arg(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), model->wallet().getDefaultMaxTxFee()));
        break;
    // included to prevent a compiler warning.
    case WalletModel::OK:
    default:
        return;
    }

    Q_EMIT message(tr("Send Coins"), msgParams.first, msgParams.second);
}

void SendCoinsDialog::minimizeFeeSection(bool fMinimize)
{
    ui->labelFeeMinimized->setVisible(fMinimize);
    ui->buttonChooseFee  ->setVisible(fMinimize);
    ui->buttonMinimizeFee->setVisible(!fMinimize);
    ui->frameFeeSelection->setVisible(!fMinimize);
    ui->horizontalLayoutSmartFee->setContentsMargins(0, (fMinimize ? 0 : 6), 0, 0);
    fFeeMinimized = fMinimize;
}

void SendCoinsDialog::on_buttonChooseFee_clicked()
{
    minimizeFeeSection(false);
}

void SendCoinsDialog::on_buttonMinimizeFee_clicked()
{
    updateFeeMinimizedLabel();
    minimizeFeeSection(true);
}

void SendCoinsDialog::useAvailableBalance(SendCoinsEntry* entry)
{
    // Include watch-only for wallets without private key
    m_coin_control->fAllowWatchOnly = model->wallet().privateKeysDisabled();

    // Calculate available amount to send.
    CAmount amount = model->wallet().getAvailableBalance(*m_coin_control);
    for (int i = 0; i < ui->entries->count(); ++i) {
        SendCoinsEntry* e = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if (e && !e->isHidden() && e != entry) {
            amount -= e->getValue().amount;
        }
    }

    if (amount > 0) {
      entry->checkSubtractFeeFromAmount();
      entry->setAmount(amount);
    } else {
      entry->setAmount(0);
    }
}

void SendCoinsDialog::updateFeeSectionControls()
{
    ui->confTargetSelector      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee           ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee2          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelSmartFee3          ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelFeeEstimation      ->setEnabled(ui->radioSmartFee->isChecked());
    ui->labelCustomFeeWarning   ->setEnabled(ui->radioCustomFee->isChecked());
    ui->labelCustomPerKilobyte  ->setEnabled(ui->radioCustomFee->isChecked());
    ui->customFee               ->setEnabled(ui->radioCustomFee->isChecked());
}

void SendCoinsDialog::updateFeeMinimizedLabel()
{
    if(!model || !model->getOptionsModel())
        return;

    if (ui->radioSmartFee->isChecked())
        ui->labelFeeMinimized->setText(ui->labelSmartFee->text());
    else {
        ui->labelFeeMinimized->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), ui->customFee->value()) + "/kB");
    }
    updatePaymentSummary();
}

void SendCoinsDialog::updateCoinControlState()
{
    if (ui->radioCustomFee->isChecked()) {
        m_coin_control->m_feerate = CFeeRate(ui->customFee->value());
    } else {
        m_coin_control->m_feerate.reset();
    }
    // Avoid using global defaults when sending money from the GUI
    // Either custom fee will be used or if not selected, the confirmation target from dropdown box
    m_coin_control->m_confirm_target = getConfTargetForIndex(ui->confTargetSelector->currentIndex());
    // Include watch-only for wallets without private key
    m_coin_control->fAllowWatchOnly = model->wallet().privateKeysDisabled();
}

void SendCoinsDialog::updateNumberOfBlocks(int count, const QDateTime& blockDate, const QString& blockHash, double nVerificationProgress, SyncType synctype, SynchronizationState sync_state) {
    if (sync_state == SynchronizationState::POST_INIT) {
        updateSmartFeeLabel();
    }
}

void SendCoinsDialog::updateSmartFeeLabel()
{
    if(!model || !model->getOptionsModel())
        return;
    updateCoinControlState();
    m_coin_control->m_feerate.reset(); // Explicitly use only fee estimation rate for smart fee labels
    int returned_target;
    FeeReason reason;
    CFeeRate feeRate = CFeeRate(model->wallet().getMinimumFee(1000, *m_coin_control, &returned_target, &reason));

    ui->labelSmartFee->setText(BitcoinUnits::formatWithUnit(model->getOptionsModel()->getDisplayUnit(), feeRate.GetFeePerK()) + "/kB");

    if (reason == FeeReason::FALLBACK) {
        ui->labelSmartFee2->show(); // (Smart fee not initialized yet. This usually takes a few blocks...)
        ui->labelFeeEstimation->setText("");
        ui->fallbackFeeWarningLabel->setVisible(true);
    }
    else
    {
        ui->labelSmartFee2->hide();
        ui->labelFeeEstimation->setText(tr("Estimated to begin confirmation within %n block(s).", "", returned_target));
        ui->fallbackFeeWarningLabel->setVisible(false);
    }

    updateFeeMinimizedLabel();
}

// Coin Control: copy label "Quantity" to clipboard
void SendCoinsDialog::coinControlClipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
}

// Coin Control: copy label "Amount" to clipboard
void SendCoinsDialog::coinControlClipboardAmount()
{
    GUIUtil::setClipboard(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" ")));
}

// Coin Control: copy label "Fee" to clipboard
void SendCoinsDialog::coinControlClipboardFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "After fee" to clipboard
void SendCoinsDialog::coinControlClipboardAfterFee()
{
    GUIUtil::setClipboard(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Bytes" to clipboard
void SendCoinsDialog::coinControlClipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace(ASYMP_UTF8, ""));
}

// Coin Control: copy label "Dust" to clipboard
void SendCoinsDialog::coinControlClipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelCoinControlLowOutput->text());
}

// Coin Control: copy label "Change" to clipboard
void SendCoinsDialog::coinControlClipboardChange()
{
    GUIUtil::setClipboard(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace(ASYMP_UTF8, ""));
}

// Coin Control: settings menu - coin control enabled/disabled by user
void SendCoinsDialog::coinControlFeatureChanged(bool checked)
{
    ui->frameCoinControl->setVisible(checked);

    if (!checked && model) { // coin control features disabled
        m_coin_control = std::make_unique<CCoinControl>(m_coin_control->nCoinType);
    }

    coinControlUpdateLabels();
}

// Coin Control: button inputs -> show actual coin control dialog
void SendCoinsDialog::coinControlButtonClicked()
{
    CoinControlDialog dlg(*m_coin_control, model, this);
    dlg.exec();
    coinControlUpdateLabels();
}

// Coin Control: checkbox custom change address
void SendCoinsDialog::coinControlChangeChecked(int state)
{
    if (state == Qt::Unchecked && !fKeepChangeAddress)
    {
        m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->clear();
    }
    else
        // use this to re-validate an already entered address
        coinControlChangeEdited(ui->lineEditCoinControlChange->text());

    ui->lineEditCoinControlChange->setEnabled(state == Qt::Checked || fKeepChangeAddress);
}

// Coin Control: custom change address changed
void SendCoinsDialog::coinControlChangeEdited(const QString& text)
{
    if (model && model->getAddressTableModel())
    {
        // Default to no change address until verified
        m_coin_control->destChange = CNoDestination();
        ui->labelCoinControlChangeLabel->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_ERROR));

        const CTxDestination dest = DecodeDestination(text.toStdString());

        if (text.isEmpty()) // Nothing entered
        {
            ui->labelCoinControlChangeLabel->setText("");
        }
        else if (!IsValidDestination(dest)) // Invalid address
        {
            ui->labelCoinControlChangeLabel->setText(tr("Warning: Invalid DeFCoN address"));
        }
        else // Valid address
        {
            if (!model->wallet().isSpendable(dest)) {
                ui->labelCoinControlChangeLabel->setText(tr("Warning: Unknown change address"));

                // confirmation dialog
                QMessageBox::StandardButton btnRetVal = QMessageBox::question(this, tr("Confirm custom change address"), tr("The address you selected for change is not part of this wallet. Any or all funds in your wallet may be sent to this address. Are you sure?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);

                if(btnRetVal == QMessageBox::Yes)
                    m_coin_control->destChange = dest;
                else
                {
                    ui->lineEditCoinControlChange->setText("");
                    ui->labelCoinControlChangeLabel->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_PRIMARY));
                    ui->labelCoinControlChangeLabel->setText("");
                }
            }
            else // Known change address
            {
                ui->labelCoinControlChangeLabel->setStyleSheet(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_PRIMARY));

                // Query label
                QString associatedLabel = model->getAddressTableModel()->labelForAddress(text);
                if (!associatedLabel.isEmpty())
                    ui->labelCoinControlChangeLabel->setText(associatedLabel);
                else
                    ui->labelCoinControlChangeLabel->setText(tr("(no label)"));

                m_coin_control->destChange = dest;
            }
        }
    }
}

// Coin Control: update labels
void SendCoinsDialog::coinControlUpdateLabels()
{
    if (!model || !model->getOptionsModel())
        return;

    updateCoinControlState();

    // set pay amounts
    CoinControlDialog::payAmounts.clear();
    CoinControlDialog::fSubtractFeeFromAmount = false;

    for(int i = 0; i < ui->entries->count(); ++i)
    {
        SendCoinsEntry *entry = qobject_cast<SendCoinsEntry*>(ui->entries->itemAt(i)->widget());
        if(entry && !entry->isHidden())
        {
            SendCoinsRecipient rcp = entry->getValue();
            CoinControlDialog::payAmounts.append(rcp.amount);
            if (rcp.fSubtractFeeFromAmount)
                CoinControlDialog::fSubtractFeeFromAmount = true;
        }
    }

    if (m_coin_control->HasSelected())
    {
        // actual coin control calculation
        CoinControlDialog::updateLabels(*m_coin_control, model, this);

        // show coin control stats
        ui->labelCoinControlAutomaticallySelected->hide();
        ui->widgetCoinControl->show();
    }
    else
    {
        // hide coin control stats
        ui->labelCoinControlAutomaticallySelected->show();
        ui->widgetCoinControl->hide();
        ui->labelCoinControlInsuffFunds->hide();
    }
}

// Settings menu - keep change address enabled/disabled by user
void SendCoinsDialog::keepChangeAddressChanged(bool checked)
{
    fKeepChangeAddress = checked;
}

SendConfirmationDialog::SendConfirmationDialog(const QString& title, const QString& text, const QString& informative_text, const QString& detailed_text, int _secDelay, const QString& _confirmButtonText, QWidget* parent)
    : QMessageBox(parent), secDelay(_secDelay), confirmButtonText(_confirmButtonText)
{
    GUIUtil::updateFonts();
    setIcon(QMessageBox::Question);
    setWindowTitle(title); // On macOS, the window title is ignored (as required by the macOS Guidelines).
    setText(text);
    setInformativeText(informative_text);
    setDetailedText(detailed_text);
    setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    setDefaultButton(QMessageBox::Cancel);
    yesButton = button(QMessageBox::Yes);
    if (confirmButtonText.isEmpty()) {
        confirmButtonText = yesButton->text();
    }
    updateYesButton();
    connect(&countDownTimer, &QTimer::timeout, this, &SendConfirmationDialog::countDown);
}

int SendConfirmationDialog::exec()
{
    updateYesButton();
    countDownTimer.start(1s);
    return QMessageBox::exec();
}

void SendConfirmationDialog::countDown()
{
    secDelay--;
    updateYesButton();

    if(secDelay <= 0)
    {
        countDownTimer.stop();
    }
}

void SendConfirmationDialog::updateYesButton()
{
    if(secDelay > 0)
    {
        yesButton->setEnabled(false);
        yesButton->setText(confirmButtonText + " (" + QString::number(secDelay) + ")");
    }
    else
    {
        yesButton->setEnabled(true);
        yesButton->setText(confirmButtonText);
    }
}
