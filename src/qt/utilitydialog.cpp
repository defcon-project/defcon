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
        QString strCoinJoinName = QString::fromStdString(gCoinJoinName);
        setWindowTitle(tr("%1 information").arg(strCoinJoinName));

        ui->aboutMessage->setTextFormat(Qt::RichText);
        ui->scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        ui->aboutMessage->setText(tr(
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
            "<li><b>Staking:</b> on the main network, mixed coins are smaller than the minimum stake, so they do not earn staking rewards, "
            "and staking never combines them or the mixing collaterals with your other coins. "
            "The target balance in Options sets roughly how much is mixed.</li>"
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
        ).arg(strCoinJoinName).arg(Params().PoolMinParticipants()));
        ui->aboutMessage->setWordWrap(true);
        ui->helpMessage->setVisible(false);
    }

    GUIUtil::handleCloseWindowShortcut(this);
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
