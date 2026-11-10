// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <key.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/commandbrowser.h>
#include <qt/starfieldwidget.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/rpcconsole.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <shutdown.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/string.h>
#include <validation.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCompleter>
#include <QTimer>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QScrollArea>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QString>
#include <QTest>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 4);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("regtest"));
}

//! Enter a command in the console. Once its reply is shown, return the text
//! the command added and the number of changes to the message view, or -1
//! changes if no reply came.
std::pair<QString, int> EnterCommand(RPCConsole* console, const QString& command)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    const int before = messagesWidget->toPlainText().size();
    QSignalSpy spy(messagesWidget, &QTextEdit::textChanged);
    QTest::keyClicks(lineEdit, command);
    QTest::keyClick(lineEdit, Qt::Key_Return);
    if (!spy.wait(5000)) return {QString{}, -1};
    return {messagesWidget->toPlainText().mid(before), spy.count()};
}

//! A secret typed into the console stays out of the echo, the error and the
//! history, and a reply that can show a secret has a warning above it.
void TestConsoleSecrets(RPCConsole* console)
{
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    const QString warning{"Warning: the reply below can contain private keys"};
    const QString key{"PROBEkey0123456789abcdefghijklmnopqrstuvwxyzABCDEF"};
    QString added;
    int changes;

    // The request line, "Executing…", its removal and the error.
    std::tie(added, changes) = EnterCommand(console, "importblsprivkey " + key);
    QCOMPARE(changes, 4);
    QVERIFY(added.contains("importblsprivkey(…)"));
    QVERIFY(!added.contains(key));
    QVERIFY(!added.contains(warning));
    // Undo in the emptied input line cannot bring the command back.
    QVERIFY(!lineEdit->isUndoAvailable());
    QTest::keyClick(lineEdit, Qt::Key_Z, Qt::ControlModifier);
    QVERIFY(lineEdit->text().isEmpty());
    QTest::keyClick(lineEdit, Qt::Key_Up);
    QCOMPARE(lineEdit->text(), QString("importblsprivkey(…)"));
    lineEdit->clear();

    // The parse error would repeat the whole array.
    std::tie(added, changes) = EnterCommand(console, "signrawtransactionwithkey 00 '[\"" + key + "\"'");
    QCOMPARE(changes, 4);
    QVERIFY(added.contains("Error parsing JSON"));
    QVERIFY(!added.contains(key));

    // An error that would repeat the key shows its code and a fixed text.
    std::tie(added, changes) = EnterCommand(console, "getdescriptorinfo 'pkh(" + key + ")'");
    QCOMPARE(changes, 4);
    QVERIFY(added.contains("getdescriptorinfo(…)"));
    QVERIFY(added.contains("The details of this error are hidden"));
    QVERIFY(added.contains("(code -5)"));
    QVERIFY(!added.contains(key));

    // The warning comes between the removal of "Executing…" and the reply.
    std::tie(added, changes) = EnterCommand(console, "bls fromsecret 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    QCOMPARE(changes, 5);
    QVERIFY(added.contains("bls fromsecret(…)"));
    QVERIFY(added.contains(warning));
    QVERIFY(added.indexOf(warning) < added.indexOf("\"public\""));

    std::tie(added, changes) = EnterCommand(console, "getblockcount");
    QCOMPARE(changes, 4);
    QVERIFY(!added.contains(warning));

    // A subcommand computed by a hidden nested command, and a hidden command
    // left open behind an unclosed inner parenthesis.
    std::tie(added, changes) = EnterCommand(console, "bls(importprivkey(fromsecret)[name], " + key + ")");
    QCOMPARE(changes, 4);
    QVERIFY(added.contains("bls(importprivkey(…))"));
    QVERIFY(!added.contains(key));
    QTest::keyClick(lineEdit, Qt::Key_Up);
    QCOMPARE(lineEdit->text(), QString("bls(importprivkey(…))"));
    lineEdit->clear();
    std::tie(added, changes) = EnterCommand(console, "walletpassphrase " + key + " echo(60");
    QCOMPARE(changes, 4);
    QVERIFY(added.contains("walletpassphrase(…)"));
    QVERIFY(!added.contains(key));
    QTest::keyClick(lineEdit, Qt::Key_Up);
    QCOMPARE(lineEdit->text(), QString("walletpassphrase(…)"));
    lineEdit->clear();
}

//! What one of two sets of names has and the other lacks, for a failure message.
std::string Difference(const std::set<std::string>& shown, const std::set<std::string>& expected)
{
    std::string text;
    for (const std::string& name : shown) {
        if (!expected.count(name)) text += " extra: " + name + ";";
    }
    for (const std::string& name : expected) {
        if (!shown.count(name)) text += " missing: " + name + ";";
    }
    return text;
}

//! One rule decides what every command list offers: the main help, the list
//! of subcommands in the help of a parent command, and the console's
//! completion. Each list is measured against the registered commands, on the
//! node the wallet runs, rather than against a copy of the list.
void TestCommandLists(RPCConsole* console)
{
    const std::vector<std::string> registered{tableRPC.listCommands()};
    const std::vector<std::string> listed_names{tableRPC.listedCommands()};
    const std::set<std::string> listed(listed_names.begin(), listed_names.end());
    const auto help = [](const std::string& command) { return tableRPC.help(command, JSONRPCRequest{}); };
    const auto is_registered = [&registered](const std::string& name) {
        return std::count(registered.begin(), registered.end(), name) == 1;
    };

    // The rule leaves developer, deprecated and hidden commands out, and
    // "help <command>" still describes them.
    std::vector<std::string> left_out{"faultinject", "faultinject set", "quorum dkgsimerror", "quorum platformsign",
                                      "getassetunlockstatuses", "masternode current", "masternode winner", "setmocktime", "echo"};
#ifdef ENABLE_WALLET
    left_out.emplace_back("getunconfirmedbalance");
#endif
    for (const std::string& name : left_out) {
        QVERIFY2(is_registered(name), name.c_str());
        QVERIFY2(!listed.count(name), name.c_str());
        QVERIFY2(help(name).rfind(name, 0) == 0, name.c_str());
    }
    for (const char* name : {"help", "getblockcount", "quorum", "quorum list", "masternode connect", "protx info", "dslstatus", "getstakinginfo"}) {
        QVERIFY2(listed.count(name), name);
    }

    // The main help: every listed command that is not a subcommand, and nothing else.
    std::set<std::string> in_help;
    for (const std::string& line : SplitString(help(""), '\n')) {
        if (!line.empty() && line.rfind("==", 0) != 0) in_help.insert(line.substr(0, line.find(' ')));
    }
    std::set<std::string> commands;
    for (const std::string& name : listed) {
        if (name.find(' ') == std::string::npos) commands.insert(name);
    }
    QVERIFY2(in_help == commands, Difference(in_help, commands).c_str());

    // Each parent's list of subcommands names exactly its listed subcommands.
    // Built without the wallet, a parent has fewer of them, and its list too.
    std::map<std::string, std::set<std::string>> subcommands;
    for (const std::string& name : registered) {
        const auto space = name.find(' ');
        if (space == std::string::npos) continue;
        auto& listed_here = subcommands[name.substr(0, space)];
        if (listed.count(name)) listed_here.insert(name.substr(space + 1));
    }
    const QRegularExpression entry("^  ([a-z][a-z_-]*) +- ");
    int parents = 0;
    for (const auto& [parent, expected] : subcommands) {
        // "evodb" has subcommands but no help of its own; "faultinject" is a
        // developer command, and its list is not offered either.
        if (!listed.count(parent)) continue;
        ++parents;
        const std::string text{help(parent)};
        const auto start = text.find("Available commands:");
        QVERIFY2(start != std::string::npos, parent.c_str());
        std::set<std::string> named;
        for (const std::string& line : SplitString(text.substr(start), '\n')) {
            const QRegularExpressionMatch match = entry.match(QString::fromStdString(line));
            if (match.hasMatch()) named.insert(match.captured(1).toStdString());
        }
        QVERIFY2(named == expected, (parent + ":" + Difference(named, expected)).c_str());
    }
    // quorum, masternode, protx, bls and gobject; coinjoin and coinjoinsalt with the wallet.
    QVERIFY(parents >= 5);

    // The completion offers each listed command and its help, and help-console.
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QVERIFY(lineEdit->completer());
    const QAbstractItemModel* model = lineEdit->completer()->model();
    std::set<std::string> offered;
    for (int row = 0; row < model->rowCount(); ++row) {
        offered.insert(model->index(row, 0).data().toString().toStdString());
    }
    std::set<std::string> words{"help-console"};
    for (const std::string& name : listed) {
        words.insert(name);
        words.insert("help " + name);
    }
    QVERIFY2(offered == words, Difference(offered, words).c_str());
}

//! The lines under "Getting started" in help-console: what is typed, each.
std::vector<std::string> GettingStarted(const QString& text)
{
    std::vector<std::string> typed;
    const QStringList lines = text.split('\n');
    const int first = lines.indexOf("Getting started");
    if (first < 0) return typed;
    const QRegularExpression step("^   (\\S.*?) - \\S");
    for (int i = first + 1; i < lines.size() && !lines[i].isEmpty(); ++i) {
        const QRegularExpressionMatch match = step.match(lines[i]);
        typed.push_back(match.hasMatch() ? match.captured(1).toStdString() : std::string{"(unreadable) "} + lines[i].toStdString());
    }
    return typed;
}

//! help-console starts with a way in: how to ask for the help of a command
//! and of a subcommand, then a few commands that only read. Every command it
//! names is one the help lists, so a build without the wallet, or a command
//! leaving the lists, takes its line away.
void TestConsoleHelp(RPCConsole* console)
{
    const std::vector<std::string> listed_names{tableRPC.listedCommands()};
    const std::set<std::string> listed(listed_names.begin(), listed_names.end());
    const std::vector<std::string> registered{tableRPC.listCommands()};
    const std::vector<std::string> steps{GettingStarted(RPCConsole::consoleHelp(listed_names))};
    bool command_help = false;
    bool subcommand_help = false;
    for (const std::string& line : steps) {
        const std::vector<std::string> words{SplitString(line, ' ')};
        QVERIFY2(!words.empty() && listed.count(words[0]), line.c_str());
        if (words[0] == "help" && words.size() > 1) {
            const std::string target{line.substr(5)};
            QVERIFY2(listed.count(target), line.c_str());
            (words.size() == 2 ? command_help : subcommand_help) = true;
        } else if (words.size() > 1) {
            // "masternode count" is a subcommand; "mnsync status" is mnsync with an argument.
            const std::string pair{words[0] + " " + words[1]};
            const bool subcommand = std::count(registered.begin(), registered.end(), pair) == 1;
            QVERIFY2(!subcommand || listed.count(pair), line.c_str());
        }
    }
    QVERIFY(command_help);
    QVERIFY(subcommand_help);
#ifdef ENABLE_WALLET
    QVERIFY(std::count(steps.begin(), steps.end(), "getwalletinfo") == 1);
#endif

    // Without the wallet's commands, their lines and the note on the selected wallet go.
    std::vector<std::string> no_wallet{listed_names};
    no_wallet.erase(std::remove(no_wallet.begin(), no_wallet.end(), "getwalletinfo"), no_wallet.end());
    const QString without = RPCConsole::consoleHelp(no_wallet);
    const std::vector<std::string> fewer{GettingStarted(without)};
    QVERIFY(std::count(fewer.begin(), fewer.end(), "getwalletinfo") == 0);
    QVERIFY(std::count(fewer.begin(), fewer.end(), "help protx info") == 1);
    QVERIFY(!without.contains("Wallet list"));

    // And the console answers help-console with it.
    QString added;
    int changes;
    std::tie(added, changes) = EnterCommand(console, "help-console");
    QCOMPARE(changes, 4);
    QVERIFY(added.contains("Getting started"));
    QVERIFY(added.contains("help protx info"));
    QVERIFY(added.contains("getblock(getblockhash(0),1)[tx][0]"));

    // A screenshot run captures the reply in each theme, scrolled to its start.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
        const QVariant saved = QSettings().value("theme");
        console->resize(1000, 700);
        QVERIFY(QDir().mkpath(output));
        for (const char* theme : {"Abyss", "Light", "Dark", "Traditional"}) {
            QSettings().setValue("theme", theme);
            GUIUtil::loadTheme(true);
            QApplication::processEvents();
            // A theme switch clears the console (RPCConsole::reloadThemedWidgets), so ask again.
            std::tie(added, changes) = EnterCommand(console, "help-console");
            QVERIFY(added.contains("Getting started"));
            messagesWidget->moveCursor(QTextCursor::End);
            QVERIFY(messagesWidget->find("Getting started", QTextDocument::FindBackward));
            QTextCursor cursor = messagesWidget->textCursor();
            cursor.clearSelection();
            messagesWidget->setTextCursor(cursor);
            QApplication::processEvents();
            QVERIFY(console->grab().save(QDir(output).filePath(QString("Console-help-%1.png").arg(QString::fromLatin1(theme)))));
        }
        if (saved.isValid()) QSettings().setValue("theme", saved); else QSettings().remove("theme");
        GUIUtil::loadTheme(true);
    }
}

//! The arguments the console hands to a command for what follows its name, read
//! back through the hidden echo command, which returns them as it got them.
std::string ConsoleArguments(interfaces::Node& node, const QString& rest)
{
    std::string result;
    const QString line = QStringLiteral("echo") + rest;
    RPCConsole::RPCExecuteCommandLine(node, result, line.toStdString());
    return result;
}

//! The Commands page: every command the help lists, by topic, searchable; each
//! with its help's examples, those for this console giving exactly the
//! arguments the defcon-cli example gives, ready to copy or to put into the
//! console's input line without running.
void TestCommandBrowser(RPCConsole* console, interfaces::Node& node)
{
    QApplication::setActiveWindow(console->window());
    auto* button = console->findChild<QPushButton*>("btnCommands");
    QVERIFY(button);
    button->click();
    QApplication::processEvents();
    auto* pages = console->findChild<QStackedWidget*>("stackedWidgetRPC");
    QVERIFY(pages);
    QCOMPARE(pages->currentWidget()->objectName(), QString("pageCommands"));
    QCOMPARE(console->tabTitle(RPCConsole::TabTypes::COMMANDS), button->text());
    auto* browser = console->findChild<CommandBrowser*>("commandBrowser");
    QVERIFY(browser);
    auto* search = browser->findChild<QLineEdit*>("commandSearch");
    auto* list = browser->findChild<QTreeWidget*>("commandList");
    QVERIFY(search && list);
    QVERIFY(search->hasFocus());

    // Every listed command once, with the topic the main help gives it; the
    // featured topics first.
    std::set<std::string> listed;
    for (const std::string& name : node.listRpcCommands()) listed.insert(name);
    std::set<std::string> shown;
    for (const CommandBrowser::Entry& entry : browser->entries()) {
        QVERIFY2(shown.insert(entry.name.toStdString()).second, qPrintable(entry.name));
        QVERIFY2(!entry.topic.isEmpty(), qPrintable(entry.name));
        QVERIFY2(entry.call.startsWith(entry.name), qPrintable(entry.name));
    }
    QVERIFY2(shown == listed, Difference(shown, listed).c_str());
    int items = 0;
    for (int g = 0; g < list->topLevelItemCount(); ++g) items += list->topLevelItem(g)->childCount();
    QCOMPARE(items, int(listed.size()));
    for (int g = 0; g < 3; ++g) QCOMPARE(list->topLevelItem(g)->text(0), CommandBrowser::featuredTopics()[g]);

    // Each defcon-cli example without an option before the command has a
    // console form, and the console hands the command the very words the shell
    // would: checked for every one of them through echo.
    int checked = 0;
    for (const CommandBrowser::Entry& entry : browser->entries()) {
        for (const QString& line : entry.command_line) {
            if (!line.startsWith("defcon-cli ")) continue;
            const QStringList words = CommandBrowser::shellWords(line);
            if (words.size() < 2 || words[1].startsWith('-')) continue;
            QStringList typed{words[1]};
            for (int w = 2; w < words.size(); ++w) typed << CommandBrowser::consoleWord(words[w]);
            const QString example = typed.join(' ');
            QVERIFY2(entry.console.contains(example), qPrintable(entry.name + ": " + line));
            UniValue expected(UniValue::VARR);
            for (int w = 2; w < words.size(); ++w) expected.push_back(words[w].toStdString());
            if (expected.size() > 10) continue; // echo takes ten
            QCOMPARE(QString::fromStdString(ConsoleArguments(node, example.mid(words[1].size()))),
                     QString::fromStdString(expected.write(2)));
            ++checked;
        }
    }
    QVERIFY2(checked > 100, qPrintable(QString::number(checked)));
    // The words that need the console's quoting, one by one.
    for (const QString& word : {QString(""), QString("a b"), QString("a,b"), QString("it's"), QString("say \"hi\""),
                                QString("back\\slash"), QString("(x)"), QString("[\"a\",1]"), QString("{\"k\": \"it's\"}")}) {
        UniValue expected(UniValue::VARR);
        expected.push_back(word.toStdString());
        QCOMPARE(QString::fromStdString(ConsoleArguments(node, " " + CommandBrowser::consoleWord(word))),
                 QString::fromStdString(expected.write(2)));
    }

    // Reading a help: the summary, the examples once each, no console form for
    // an option before the command, an open quote or a substitution.
    const CommandBrowser::Entry parsed = CommandBrowser::parseHelp("demo", "Topic",
        "demo \"a\"\n\nDoes a thing\nin two lines.\n\nMore.\n\nArguments:\n1. a\n\nExamples:\n"
        "> defcon-cli demo \"x y\"\n> defcon-cli demo \"x y\"\n> defcon-cli -named demo a=1\n"
        "> defcon-cli demo 'open\n> defcon-cli demo $(cat f)\n> curl --data-binary '{}' http://127.0.0.1:8193/\n");
    QCOMPARE(parsed.call, QString("demo \"a\""));
    QCOMPARE(parsed.summary, QString("Does a thing in two lines."));
    QCOMPARE(parsed.console, QStringList{"demo 'x y'"});
    QCOMPARE(parsed.command_line.size(), 5);

    // Search: by name or summary, case aside; down arrow goes into the list.
    QTest::keyClicks(search, "STAK");
    QApplication::processEvents();
    bool staking_shown = false;
    for (int g = 0; g < list->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = list->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c) {
            QTreeWidgetItem* item = group->child(c);
            if (item->isHidden()) continue;
            const CommandBrowser::Entry* entry = nullptr;
            for (const CommandBrowser::Entry& e : browser->entries()) if (e.name == item->text(0)) entry = &e;
            QVERIFY(entry);
            QVERIFY2(entry->name.contains("stak", Qt::CaseInsensitive) || entry->summary.contains("stak", Qt::CaseInsensitive), qPrintable(entry->name));
            if (entry->name == "getstakinginfo") staking_shown = true;
        }
    }
    QVERIFY(staking_shown);
    QVERIFY(list->currentItem() && !list->currentItem()->isHidden());
    QTest::keyClick(search, Qt::Key_Down);
    QVERIFY(list->hasFocus());
    search->clear();
    // A word only the summary has: dslstatus describes the Sentinel's probe.
    QTest::keyClicks(search, "sentinel");
    QApplication::processEvents();
    bool dslstatus_shown = false;
    for (int g = 0; g < list->topLevelItemCount(); ++g) {
        for (int c = 0; c < list->topLevelItem(g)->childCount(); ++c) {
            QTreeWidgetItem* item = list->topLevelItem(g)->child(c);
            if (item->text(0) == "dslstatus" && !item->isHidden()) dslstatus_shown = true;
        }
    }
    QVERIFY(dslstatus_shown);
    search->clear();

    // Put in console fills the input line on the Console page and runs nothing.
    QVERIFY(browser->select("getblockcount"));
    QApplication::processEvents();
    const QList<QPushButton*> puts = browser->findChildren<QPushButton*>("commandPutInConsole");
    QCOMPARE(puts.size(), 1);
    QPushButton* put = puts.front();
    QVERIFY(put->isVisible());
    auto* messages = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* line = console->findChild<QLineEdit*>("lineEdit");
    const QString before = messages->toPlainText();
    put->click();
    QApplication::processEvents();
    QCOMPARE(pages->currentWidget()->objectName(), QString("pageConsole"));
    QCOMPARE(line->text(), QString("getblockcount"));
    QVERIFY(line->hasFocus());
    QTest::qWait(200);
    QCOMPARE(messages->toPlainText(), before);
    line->clear();

    // In every theme the details stand on the page's own background: below a short
    // command's details, the pane has the colour of the page's bottom margin.
    {
        const QVariant saved = QSettings().value("theme");
        console->resize(1000, 700);
        button->click();
        QVERIFY(browser->select("dslstatus"));
        auto* page = console->findChild<QWidget*>("pageCommands");
        auto* area = browser->findChild<QScrollArea*>("commandDetailsArea");
        QVERIFY(page && area);
        for (const char* theme : {"Abyss", "Light", "Dark", "Traditional"}) {
            QSettings().setValue("theme", theme);
            GUIUtil::loadTheme(true);
            QApplication::processEvents();
            const QImage shot = console->grab().toImage();
            const qreal dpr = shot.devicePixelRatio();
            const QPoint pane = area->viewport()->mapTo(console, QPoint(area->viewport()->width() - 12, area->viewport()->height() - 12));
            const QPoint margin = page->mapTo(console, QPoint(page->width() / 2, page->height() - 2));
            const QColor pane_colour = shot.pixelColor(qRound(pane.x() * dpr), qRound(pane.y() * dpr));
            const QColor margin_colour = shot.pixelColor(qRound(margin.x() * dpr), qRound(margin.y() * dpr));
            QVERIFY2(pane_colour == margin_colour, qPrintable(QString("%1: details %2, page %3").arg(QString::fromLatin1(theme), pane_colour.name(), margin_colour.name())));
        }
        if (saved.isValid()) QSettings().setValue("theme", saved); else QSettings().remove("theme");
        GUIUtil::loadTheme(true);
    }

    // A screenshot run captures the page in each theme.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        const QVariant saved = QSettings().value("theme");
        console->resize(1000, 700);
        QVERIFY(QDir().mkpath(output));
        button->click();
        QVERIFY(browser->select("combineoutputs") || browser->select("getstakinginfo"));
        for (const char* theme : {"Abyss", "Light", "Dark", "Traditional"}) {
            QSettings().setValue("theme", theme);
            GUIUtil::loadTheme(true);
            QApplication::processEvents();
            QVERIFY(console->grab().save(QDir(output).filePath(QString("Console-commands-%1.png").arg(QString::fromLatin1(theme)))));
        }
        QTest::keyClicks(search, "sentinel");
        QApplication::processEvents();
        QVERIFY(console->grab().save(QDir(output).filePath("Console-commands-search.png")));
        search->clear();
        if (saved.isValid()) QSettings().setValue("theme", saved); else QSettings().remove("theme");
        GUIUtil::loadTheme(true);
    }
    console->setTabFocus(RPCConsole::TabTypes::CONSOLE);
}

#ifdef ENABLE_WALLET
//! Check the Abyss sidebar: the wordmark is centred, and each page's glyph is
//! its artwork, in both button states. An SVG that is missing or cannot be
//! decoded draws nothing without a word, so a glyph merely existing proves
//! little; and two pages once shared one.
void TestAbyssSidebar(BitcoinGUI* window)
{
    struct RestoreTheme {
        QVariant theme{QSettings().value("theme")};
        ~RestoreTheme()
        {
            if (theme.isValid()) QSettings().setValue("theme", theme); else QSettings().remove("theme");
            GUIUtil::loadTheme(true);
        }
    } restore;
    // The wallet's main() turns high-DPI pixmaps on and this binary does not.
    // Without them an icon is handed back at its logical size, scaled down
    // from what was drawn for the screen, which is not what the wallet shows.
    struct HighDpiPixmaps {
        const bool was{QCoreApplication::testAttribute(Qt::AA_UseHighDpiPixmaps)};
        HighDpiPixmaps() { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps); }
        ~HighDpiPixmaps() { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, was); }
    } high_dpi_pixmaps;
    const auto theme = [](const char* name) {
        QSettings().setValue("theme", name);
        GUIUtil::loadTheme(true);
        QApplication::processEvents();
    };

    auto* bar = window->findChild<QToolBar*>("navigationToolBar");
    QVERIFY(bar);
    QButtonGroup* group = nullptr;
    for (QToolButton* button : bar->findChildren<QToolButton*>()) {
        if (button->group()) group = button->group();
    }
    QVERIFY(group);
    const QList<QAbstractButton*> buttons = group->buttons();

    // In the order createToolBars adds the pages; the last three are optional.
    QStringList expected{"home", "send", "qrcode", "history", "arrows-shuffle"};
    QSettings settings;
    if (settings.value("fShowMultisigTab", false).toBool()) expected << "signature";
    if (settings.value("fShowMasternodesTab").toBool()) expected << "topology-star-3";
    if (settings.value("fShowGovernanceTab").toBool()) expected << "gavel";
    QCOMPARE(buttons.size(), expected.size());
    // appTests() turns every optional page on for a screenshot run.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) QCOMPARE(expected.size(), 8);

    theme("Abyss");
    if (!output.isEmpty()) {
        // The size AbyssTests captures at. A shorter window folds the last
        // pages into the toolbar's overflow menu, out of the picture.
        window->resize(1280, 850);
        QApplication::processEvents();
        QVERIFY(QDir().mkpath(output));
        QVERIFY(bar->grab().save(QDir(output).filePath("Abyss-navigation.png")));
    }

    // The wordmark sits in the middle of the sidebar. Measured on what the bar
    // draws rather than on the label's geometry: the image is painted inside
    // the label, so where the label sits says little about where the ink is.
    auto* logo = bar->findChild<QLabel*>("lblToolbarLogo");
    QVERIFY(logo);
    const QImage shot = bar->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const qreal dpr = shot.devicePixelRatio();
    // Only the rows the image is drawn in. The whole label reaches down to the
    // checked button below it, whose fill would read as a centred wordmark.
    const QRect canvas = logo->contentsRect().translated(logo->pos());
    const QRect band(QPoint(0, qRound(canvas.top() * dpr)), QSize(shot.width(), qRound(canvas.height() * dpr)));
    // The wordmark is blue on a near-black sidebar; the label's top-left
    // corner is margin, so it shows the sidebar itself.
    const int backdrop = qBlue(shot.pixel(qRound(logo->x() * dpr) + 1, band.top() + 1));
    int left = shot.width();
    int right = -1;
    for (int y = band.top(); y <= band.bottom(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            if (qBlue(shot.pixel(x, y)) - backdrop > 60) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    QVERIFY2(right >= left, "the wordmark draws nothing");
    const int gap_left = qRound(left / dpr);
    const int gap_right = bar->width() - qRound((right + 1) / dpr);
    QVERIFY2(std::abs(gap_left - gap_right) <= 2,
             qPrintable(QString("wordmark off centre: %1 px left, %2 px right").arg(gap_left).arg(gap_right)));

    // Each glyph is drawn for the screen's pixel ratio and shown one to one in
    // the 22 px the button lays out. So it is compared with its artwork drawn
    // at that same pixel size, not with a copy scaled down to the layout size.
    QVERIFY(window->windowHandle());
    const QSize logical(22, 22);
    const qreal ratio = qApp->devicePixelRatio();
    const QSize physical = logical * ratio;
    for (int i = 0; i < buttons.size(); ++i) {
        QCOMPARE(buttons[i]->iconSize(), logical);
        QImage art(physical, QImage::Format_ARGB32_Premultiplied);
        art.fill(Qt::transparent);
        QPainter painter(&art);
        QIcon(":/icons/tabler/" + expected[i] + ".svg").paint(&painter, art.rect());
        painter.end();

        for (const QIcon::State state : {QIcon::Off, QIcon::On}) {
            const QString what = expected[i] + (state == QIcon::On ? " (checked)" : "");
            const QColor tint = state == QIcon::On ? QColor(Qt::white) : GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BLUE);
            // What the button paints, asked for the way its style asks.
            const QPixmap shown = buttons[i]->icon().pixmap(window->windowHandle(), logical, QIcon::Normal, state);
            QCOMPARE(shown.size(), physical);
            QCOMPARE(shown.devicePixelRatio(), ratio);
            const QImage drawn = shown.toImage().convertToFormat(QImage::Format_ARGB32);
            int ink = 0;
            int worst = 0;
            QRgb densest = 0;
            for (int y = 0; y < physical.height(); ++y) {
                for (int x = 0; x < physical.width(); ++x) {
                    const QRgb px = drawn.pixel(x, y);
                    worst = std::max(worst, std::abs(qAlpha(px) - qAlpha(art.pixel(x, y))));
                    if (qAlpha(px) > 0) ++ink;
                    if (qAlpha(px) > qAlpha(densest)) densest = px;
                }
            }
            QVERIFY2(ink > 20, qPrintable("navigation glyph is empty: " + what));
            QVERIFY2(worst <= 2, qPrintable("navigation glyph is not its artwork: " + what));
            QVERIFY2(std::abs(qRed(densest) - tint.red()) <= 3 && std::abs(qGreen(densest) - tint.green()) <= 3 &&
                         std::abs(qBlue(densest) - tint.blue()) <= 3,
                     qPrintable("navigation glyph has the wrong tint: " + what));
        }
    }

    // Exercise the actual BitcoinGUI theme route, not only the sky setter.
    auto* sky = qobject_cast<StarfieldWidget*>(window->centralWidget());
    QVERIFY(sky);
    // The frame timer; the other one is the single shot that redraws an inactive window's real sky.
    QList<QTimer*> sky_timers;
    for (QTimer* timer : sky->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!timer->isSingleShot()) sky_timers << timer;
    }
    QCOMPARE(sky_timers.size(), 1);
    QApplication::setActiveWindow(window);
    QApplication::processEvents();
    QVERIFY(sky_timers.front()->isActive());
    if (!output.isEmpty()) QVERIFY(sky->grab().save(QDir(output).filePath("Abyss-central-sky.png")));
    if (!output.isEmpty()) QVERIFY(window->grab().save(QDir(output).filePath("Abyss-window.png")));
    for (const char* classic : {"Light", "Dark", "Traditional"}) {
        theme(classic);
        QVERIFY(!sky_timers.front()->isActive());
        for (QAbstractButton* button : buttons) QVERIFY(button->icon().isNull());
        theme("Abyss");
        QApplication::setActiveWindow(window);
        QApplication::processEvents();
        QVERIFY(sky_timers.front()->isActive());
    }

    // Leaving Abyss takes the glyphs away again: the classic bars are text only.
    theme("Light");
    for (QAbstractButton* button : buttons) {
        QVERIFY2(button->icon().isNull(), qPrintable(button->text()));
    }
}
#endif // ENABLE_WALLET
} // namespace

//! Entry point for BitcoinApplication tests.
void AppTests::appTests()
{
    QVERIFY(GUIUtil::isValidTheme("Abyss"));
    // Nebula and Arctic were removed, along with the former names of all three
    // modern themes. None of them may be selectable any more.
    QVERIFY(!GUIUtil::isValidTheme("Nebula"));
    QVERIFY(!GUIUtil::isValidTheme("Arctic"));
    QVERIFY(!GUIUtil::isValidTheme("DeFCon Dark"));
    QVERIFY(!GUIUtil::isValidTheme("DeFCon Galaxy"));
    QVERIFY(!GUIUtil::isValidTheme("DeFCon Light"));

    // What matters more than the names is where a wallet that still has one
    // stored ends up. Falling back to the default would hand a white wallet to
    // someone who chose a dark one, silently, on the next start -- so each
    // removed theme resolves to a survivor of its own kind instead.
    {
        QSettings settings;
        const QVariant saved = settings.value("theme");
        const auto resolves = [&settings](const char* stored) {
            settings.setValue("theme", QString(stored));
            return GUIUtil::getActiveTheme();
        };
        QCOMPARE(resolves("Nebula"), QString("Abyss"));
        QCOMPARE(resolves("DeFCon Galaxy"), QString("Abyss"));
        QCOMPARE(resolves("DeFCon Dark"), QString("Abyss"));
        QCOMPARE(resolves("Arctic"), QString("Light"));
        QCOMPARE(resolves("DeFCon Light"), QString("Light"));
        // A name nobody ever shipped still falls back to the default, as before.
        QCOMPARE(resolves("NoSuchTheme"), GUIUtil::getDefaultTheme());
        if (saved.isValid()) {
            settings.setValue("theme", saved);
        } else {
            settings.remove("theme");
        }
    }

    // The stylesheets follow the themes: the survivors are in the resources and
    // the removed ones are gone. The two assertions here previously named
    // ":/css/DeFCon Galaxy" and ":/css/DeFCon Light", aliases the qrc stopped
    // carrying when the themes were renamed -- so this check had been failing
    // on every build since, unnoticed, which is how a resource can go missing
    // and a test still be described as covering it.
    // Which themes are dark. The window frame follows this and not the desktop,
    // so a wallet and the bar above it can never contradict each other. The
    // platform call that acts on the answer is Windows-only and is not even
    // compiled here -- which is the reason the answer lives apart from it, and
    // the reason this much can still be measured.
    {
        QSettings settings;
        const QVariant saved = settings.value("theme");
        const auto isDark = [&settings](const char* theme) {
            settings.setValue("theme", QString(theme));
            return GUIUtil::themeIsDark();
        };
        QVERIFY(isDark("Abyss"));
        QVERIFY(isDark("Dark"));
        QVERIFY(!isDark("Light"));
        QVERIFY(!isDark("Traditional"));
        // A removed theme is resolved before it is judged, so it inherits the
        // brightness of the survivor it maps to rather than the default.
        QVERIFY(isDark("Nebula"));
        QVERIFY(!isDark("Arctic"));
        // And a name nobody shipped follows the default, which is light.
        QVERIFY(!isDark("NoSuchTheme"));
        if (saved.isValid()) {
            settings.setValue("theme", saved);
        } else {
            settings.remove("theme");
        }
    }

    QVERIFY(QFile(":/css/Abyss").exists());
    QVERIFY(QFile(":/css/Light").exists());
    QVERIFY(!QFile(":/css/Nebula").exists());
    QVERIFY(!QFile(":/css/Arctic").exists());
    // Properties Qt Style Sheets do not implement. Qt drops them in silence, so
    // a rule written with one reads correctly in the file and does nothing on
    // screen. Abyss shipped text-transform and letter-spacing on the devnet
    // badge for exactly that reason, and a change before it shipped opacity on
    // a button. Neither was noticed by reading; a list is cheaper.
    {
        const QStringList unsupported{"text-transform", "letter-spacing", "box-shadow",
                                      "text-shadow", "transition", "z-index",
                                      "opacity", "float", "overflow"};
        QStringList sheets{"general"};
        for (const QString& theme : GUIUtil::listThemes()) {
            sheets << theme;
        }
        for (const QString& sheet : sheets) {
            QFile file(":/css/" + sheet);
            QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                     qPrintable("stylesheet resource missing: " + sheet));
            const QString css = QString::fromUtf8(file.readAll());
            for (const QString& property : unsupported) {
                // Anchored where a property may begin, so the word inside
                // "subcontrol-position" or a sentence in a comment is not a hit.
                const QRegularExpression re("(^|[{;])\\s*" + property + "\\s*:",
                                            QRegularExpression::MultilineOption);
                QVERIFY2(!re.match(css).hasMatch(),
                         qPrintable(sheet + " uses " + property + ", which Qt ignores"));
            }
        }
    }

    QCOMPARE(GUIUtil::getDefaultTheme(), QString("Light"));

#ifdef Q_OS_MAC
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        QWARN("Skipping AppTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
              "with 'QT_QPA_PLATFORM=cocoa test_dash-qt' on mac, or else use a linux or windows build.");
        return;
    }
#endif

    fs::create_directories([] {
        BasicTestingSetup test{CBaseChainParams::REGTEST}; // Create a temp data directory to backup the gui settings to
        return gArgs.GetDataDirNet() / "blocks";
    }());

    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    GUIUtil::loadFonts();
    m_app.createOptionsModel(true /* reset settings */);
    QScopedPointer<const NetworkStyle> style(
        NetworkStyle::instantiate(Params().NetworkIDString()));
    // A screenshot run shows every page the sidebar can carry, so the optional
    // glyphs are checked and captured too. After the network style, which
    // renames the application and with it the settings the window reads. Put
    // back afterwards: later suites capture the options dialog, which shows
    // these switches.
    std::vector<std::pair<QString, QVariant>> optional_tabs;
    if (!qEnvironmentVariable("DEFCON_QT_SCREENSHOTS").isEmpty()) {
        QSettings settings;
        for (const char* tab : {"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab"}) {
            optional_tabs.emplace_back(tab, settings.value(tab));
            settings.setValue(tab, true);
        }
    }
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    QSettings().setValue("fAppearanceSetupDone", true); // skip appearance setup
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    for (const auto& [tab, value] : optional_tabs) {
        if (value.isValid()) QSettings().setValue(tab, value); else QSettings().remove(tab);
    }
    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
    AbortShutdown();
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};
#ifdef ENABLE_WALLET
    TestAbyssSidebar(window);
#endif
    connect(window, &BitcoinGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
    TestConsoleSecrets(console);
    TestCommandLists(console);
    TestConsoleHelp(console);
    TestCommandBrowser(console, m_app.node());
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.quit();
    }
}
