// Copyright (c) 2011-2020 The Bitcoin Core developers
// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/utilitydialog.h>

#include <qt/forms/ui_helpmessagedialog.h>

#include <qt/guiutil.h>

#include <chainparams.h>
#include <clientversion.h>
#include <coinjoin/common.h>
#include <init.h>
#include <util/system.h>
#include <util/strencodings.h>

#include <stdio.h>

#include <QCloseEvent>
#include <QLabel>
#include <QMainWindow>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextTable>
#include <QVBoxLayout>

/** "Help message" or "About" dialog box */
HelpMessageDialog::HelpMessageDialog(QWidget *parent, HelpMode helpMode) :
    QDialog(parent, GUIUtil::dialog_flags),
    ui(new Ui::HelpMessageDialog)
{
    ui->setupUi(this);

    GUIUtil::updateFonts();

    QString version = QString{PACKAGE_NAME} + " " + tr("version") + " " + QString::fromStdString(FormatFullVersion());

    if (helpMode == about)
    {
        setWindowTitle(tr("About %1").arg(PACKAGE_NAME));

        std::string licenseInfo = LicenseInfo();
        /// HTML-format the license message from the core
        QString licenseInfoHTML = QString::fromStdString(licenseInfo);

        // Make URLs clickable
        const QRegularExpression uri(QStringLiteral("<(.*?)>")); // non-greedy: one link per angle-bracket pair
        licenseInfoHTML.replace(uri, QString("<a style=\"%1\"href=\"\\1\">\\1</a>").arg(GUIUtil::getThemedStyleQString(GUIUtil::ThemedStyle::TS_COMMAND)));
        // Replace newlines with HTML breaks
        licenseInfoHTML.replace("\n", "<br>");

        ui->aboutMessage->setTextFormat(Qt::RichText);
        ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        text = version + "\n" + QString::fromStdString(FormatParagraph(licenseInfo));
        ui->aboutMessage->setText(version + "<br><br>" + licenseInfoHTML);
        ui->aboutMessage->setWordWrap(true);
        ui->helpMessage->setVisible(false);
    } else if (helpMode == cmdline) {
        setWindowTitle(tr("Command-line options"));
        QString header = "Usage:  defcon-qt [command-line options]                     \n";
        QTextCursor cursor(ui->helpMessage->document());
        cursor.insertText(version);
        cursor.insertBlock();
        cursor.insertText(header);
        cursor.insertBlock();

        std::string strUsage = gArgs.GetHelpMessage();
        QString coreOptions = QString::fromStdString(strUsage);
        text = version + "\n\n" + header + "\n" + coreOptions;

        QTextTableFormat tf;
        tf.setBorderStyle(QTextFrameFormat::BorderStyle_None);
        tf.setCellPadding(2);
        QVector<QTextLength> widths;
        widths << QTextLength(QTextLength::PercentageLength, 35);
        widths << QTextLength(QTextLength::PercentageLength, 65);
        tf.setColumnWidthConstraints(widths);

        QTextCharFormat bold;
        bold.setFontWeight(QFont::Bold);

        for (const QString &line : coreOptions.split("\n")) {
            if (line.startsWith("  -"))
            {
                cursor.currentTable()->appendRows(1);
                cursor.movePosition(QTextCursor::PreviousCell);
                cursor.movePosition(QTextCursor::NextRow);
                cursor.insertText(line.trimmed());
                cursor.movePosition(QTextCursor::NextCell);
            } else if (line.startsWith("   ")) {
                cursor.insertText(line.trimmed()+' ');
            } else if (line.size() > 0) {
                //Title of a group
                if (cursor.currentTable())
                    cursor.currentTable()->appendRows(1);
                cursor.movePosition(QTextCursor::Down);
                cursor.insertText(line.trimmed(), bold);
                cursor.insertTable(1, 2, tf);
            }
        }

        ui->helpMessage->moveCursor(QTextCursor::Start);
        ui->scrollArea->setVisible(false);
    } else if (helpMode == pshelp) {
        setWindowTitle(tr("%1 information").arg(QString::fromStdString(gCoinJoinName)));

        ui->aboutMessage->setTextFormat(Qt::RichText);
        ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        const Consensus::Params& consensus = Params().GetConsensus();
        ui->aboutMessage->setText(coinJoinHelp(CoinJoin::DfcnScaleMixingHeight(consensus.nV231ActivationHeight,
                                                                               consensus.nDfcnScaleMixingHeightOverride)));
        ui->aboutMessage->setWordWrap(true);
        ui->helpMessage->setVisible(false);
    }

    GUIUtil::handleCloseWindowShortcut(this);
}

QString HelpMessageDialog::coinJoinHelp(std::optional<int64_t> mixing_height)
{
    // What the text states is what the code does: the wallet makes and counts
    // the five standard amounts only (CoinJoin::vecStandardDenominations); the
    // three larger ones are made by the user and mixed from the height given
    // here, apart from the target (CCoinJoinClientSession::DoAutomaticDenominating);
    // no coin of any of the eight amounts stakes or is combined
    // (CStakeWallet::ClassifyForStaking, PlanCombine); a normal payment spares
    // only the five (KnapsackSolver), and a payment of mixed coins has no change:
    // what the coins hold above the amount is the fee, which has to reach the
    // required fee and stay under the wallet's limit, and subtracting the fee
    // from the amount takes that difference a second time
    // (CWallet::CreateTransaction). A larger coin is the wallet's own, and a
    // mixing candidate, only as a P2PKH output of a key the wallet holds and
    // recognises (IsMixableOutput): an address made from a pk() descriptor
    // pays to a script that descriptor does not track.
    const QString strCoinJoinName = QString::fromStdString(gCoinJoinName);
    const QString mixing_start = mixing_height
        ? tr("Mixing these amounts starts at block %1.").arg(QString::number(static_cast<qlonglong>(*mixing_height)))
        : tr("The block at which mixing these amounts starts is not set in this version yet.");
    return tr(
            "<h3>%1 basics</h3>"
            "<p>%1 makes it much harder to trace where your DFCN came from. "
            "It mixes your coins with the coins of other DeFCoN users in shared transactions. "
            "Your coins never leave your wallet, and only you can spend them.</p><hr>"
            "<b>How it works</b>"
            "<ol type=\"1\">"
            "<li>Your wallet splits part of your balance into standard amounts of slightly more than 0.001, 0.01, 0.1, 1 and 10 DFCN, "
            "for example 10.0001 DFCN. Everyone uses the same amounts, so the coins look alike.</li>"
            "<li>For one of these amounts, your wallet joins a mixing session that is waiting at a masternode, "
            "or asks a randomly chosen masternode to start a new one.</li>"
            "<li>When at least %2 participants have joined, the masternode combines their coins into one transaction. "
            "Each wallet checks and signs only its own part, and gets the same amounts back to new addresses of its own.</li>"
            "<li>Each completed session is one round. Your wallet repeats this several times. "
            "You can set the number of rounds and the target balance in Options.</li>"
            "<li>Mixing runs in the background while the wallet is open. "
            "When you send from your %1 balance, your coins are already mixed.</li>"
            "</ol><hr>"
            "<b>Good to know</b>"
            "<ul>"
            "<li><b>Staking:</b> coins of exactly a standard amount, or of one of the larger amounts below, do not earn staking rewards, "
            "whether they are mixed or not, and staking never combines them or the mixing collaterals with your other coins. "
            "The target balance in Options sets roughly how much is mixed in the standard amounts.</li>"
            "<li><b>Fees:</b> mixing transactions are free. Creating the standard amounts costs a normal small network fee. "
            "Now and then a tiny collateral of about 0.0001 DFCN goes to the network to prevent abuse.</li>"
            "<li><b>Privacy:</b> the masternode running a session sees which coins take part in it, "
            "and the other participants are not guaranteed to be different people. "
            "More rounds make your coins harder to follow. "
            "Sending many mixed coins together in one payment can link them again.</li>"
            "<li><b>Encrypted wallet:</b> if the wallet is locked when you start mixing, it asks for your passphrase "
            "and unlocks for mixing only, so sending still needs the passphrase.</li>"
            "<li><b>Backups:</b> each round uses up to 9 new addresses. "
            "%1 needs automatic wallet backups to be switched on; they are on by default.</li>"
            "</ul>"
        ).arg(strCoinJoinName).arg(Params().PoolMinParticipants())
        //: %1 is the sentence about the block at which mixing the larger amounts starts; it is translated on its own.
        + tr(
            "<hr><b>Larger amounts</b>"
            "<p>For larger sums there are three more amounts: 100.001, 1000.01 and 10000.1 DFCN. "
            "Your wallet mixes coins of these amounts, but it does not make them. "
            "You make them with one normal payment to yourself. "
            "Use this recipe only in a wallet whose receiving and change addresses are its own spendable P2PKH addresses, "
            "as they are by default: a wallet that was given other active descriptors may not recognise the coins it pays to itself.</p>"
            "<ol type=\"1\">"
            "<li>Create a new receiving address for each coin you want, and one more.</li>"
            "<li>On the Send page, add a recipient for each of these addresses. "
            "Pay exactly 100.001, 1000.01 or 10000.1 DFCN to each coin's address and 0.01 DFCN to the last one, "
            "with \"Subtract fee from amount\" switched off for every recipient.</li>"
            "</ol>"
            "<p>%1 From then on, while mixing is switched on, your wallet mixes every coin of these amounts "
            "that it holds on such an address, whatever the target balance is. "
            "To keep a coin out, lock it before you start mixing: "
            "enable the coin control features in Options, then use Inputs… on the Send page.</p>"
            "<ul>"
            "<li>The 0.01 DFCN is ordinary money for the mixing collaterals of these amounts. "
            "If your wallet has no ordinary, unmixed balance left for them, mixing these amounts can wait.</li>"
            "<li>A normal payment can spend these coins like any other coin, mixed or not. "
            "Lock the ones you want to keep before you start mixing, or choose the coins of the payment yourself. "
            "While mixing is running, choose coins that are not of a standard or a larger amount, "
            "and neither lock nor spend a coin of these amounts that is waiting to be mixed until its session has ended: "
            "a session that loses its coin can cost a collateral.</li>"
            "<li>A session needs other wallets that mix the same amount at the same time, "
            "so a larger amount can wait longer for its rounds.</li>"
            "</ul>"
        ).arg(mixing_start)
        // The cap is the target balance field's maximum in the options form; the
        // fee limit is the wallet's default (-maxtxfee).
        + tr(
            "<hr><b>How much to mix</b>"
            "<p>The standard amounts are at most about 10 DFCN, so on their own they suit smaller sums. "
            "The target balance in Options goes up to 10,000 DFCN, which already means a thousand mixed coins or more, "
            "and a payment can hold only so many coins: "
            "one payment from coins of the standard amounts can be at most about 6,700 DFCN. "
            "For more, use the larger amounts.</p>"
            "<p>A payment from your %1 balance has no change. "
            "The mixed coins have to add up to the amount you send; what is left over goes to the fee, "
            "and the fee can be at most 0.1 DFCN by default. "
            "With coins of the larger amounts, switch off \"Subtract fee from amount\" for every recipient "
            "and send a little less than the coins add up to: the difference is the fee, "
            "so it has to cover the normal network fee and stay within the wallet's fee limit. "
            "Or keep coins of the standard amounts to make up the rest.</p>"
        ).arg(strCoinJoinName);
}

HelpMessageDialog::~HelpMessageDialog()
{
    delete ui;
}

void HelpMessageDialog::printToConsole()
{
    // On other operating systems, the expected action is to print the message to the console.
    tfm::format(std::cout, "%s", qPrintable(text));
}

void HelpMessageDialog::showOrPrint()
{
#if defined(WIN32)
    // On Windows, show a message box, as there is no stderr/stdout in windowed applications
    exec();
#else
    // On other operating systems, print help text to console
    printToConsole();
#endif
}

void HelpMessageDialog::on_okButton_accepted()
{
    close();
}


/** "Shutdown" window */
ShutdownWindow::ShutdownWindow(QWidget *parent, Qt::WindowFlags f):
    QWidget(parent, f)
{
    setObjectName("ShutdownWindow");

    QVBoxLayout *layout = new QVBoxLayout();
    layout->addWidget(new QLabel(
        tr("%1 is shutting down…").arg(PACKAGE_NAME) + "<br /><br />" +
        tr("Do not shut down the computer until this window disappears.")));
    setLayout(layout);

    GUIUtil::updateFonts();

    GUIUtil::handleCloseWindowShortcut(this);
}

QWidget* ShutdownWindow::showShutdownWindow(QMainWindow* window)
{
    assert(window != nullptr);

    // Show a simple window indicating shutdown status
    QWidget *shutdownWindow = new ShutdownWindow();
    shutdownWindow->setWindowTitle(window->windowTitle());

    // Center shutdown window at where main window was
    const QPoint global = window->mapToGlobal(window->rect().center());
    shutdownWindow->move(global.x() - shutdownWindow->width() / 2, global.y() - shutdownWindow->height() / 2);
    shutdownWindow->show();
    return shutdownWindow;
}

void ShutdownWindow::closeEvent(QCloseEvent *event)
{
    event->ignore();
}
