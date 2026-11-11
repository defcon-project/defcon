// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/commandbrowser.h>

#include <interfaces/node.h>
#include <qt/guiutil.h>
#include <rpc/commandflags.h>

#include <univalue.h>

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <map>
#include <string>
#include <utility>

namespace {
//! The data role of a command's item: its index in the entries. Topic items have none.
constexpr int ENTRY_ROLE = Qt::UserRole + 1;

//! A section heading of the help, after which the description is over.
bool IsHeading(const QString& line)
{
    return line == QLatin1String("Arguments:") || line.startsWith(QLatin1String("Result")) ||
           line == QLatin1String("Examples:") || line == QLatin1String("Available commands:");
}

void AppendOnce(QStringList& list, const QString& text)
{
    if (!list.contains(text)) list << text;
}
} // namespace

CommandBrowser::CommandBrowser(interfaces::Node& node, QWidget* parent)
    : QWidget(parent), m_node(node)
{
    setObjectName("commandBrowser");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 6, 0, 0);

    m_status = new QLabel(this);
    m_status->setObjectName("commandStatus");
    m_status->setWordWrap(true);
    m_status->setText(tr("The commands appear here once the node has started."));
    layout->addWidget(m_status);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    layout->addWidget(splitter, 1);

    auto* left = new QWidget(splitter);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 6, 0);
    m_search = new QLineEdit(left);
    m_search->setObjectName("commandSearch");
    m_search->setPlaceholderText(tr("Search commands"));
    m_search->setAccessibleName(tr("Search commands"));
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);
    left_layout->addWidget(m_search);
    setFocusProxy(m_search);
    m_list = new QTreeWidget(left);
    m_list->setObjectName("commandList");
    m_list->setAccessibleName(tr("Commands"));
    m_list->setHeaderHidden(true);
    m_list->setColumnCount(1);
    m_list->setMinimumWidth(200);
    left_layout->addWidget(m_list, 1);

    auto* scroll = new QScrollArea(splitter);
    scroll->setObjectName("commandDetailsArea");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_details = new QWidget(scroll);
    m_details->setObjectName("commandDetails");
    auto* details = new QVBoxLayout(m_details);
    details->setContentsMargins(6, 0, 6, 6);
    m_call = new QLabel(m_details);
    m_call->setObjectName("commandCall");
    m_call->setFont(GUIUtil::fixedPitchFont());
    m_call->setWordWrap(true);
    m_call->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    details->addWidget(m_call);
    m_summary = new QLabel(m_details);
    m_summary->setObjectName("commandSummary");
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    details->addWidget(m_summary);
    m_topic = new QLabel(m_details);
    m_topic->setObjectName("commandTopic");
    details->addWidget(m_topic);
    m_flags = new QLabel(m_details);
    m_flags->setObjectName("commandFlags");
    m_flags->setWordWrap(true);
    details->addWidget(m_flags);
    details->addSpacing(8);
    m_console_title = new QLabel(tr("In this console:"), m_details);
    details->addWidget(m_console_title);
    m_console_rows = new QWidget(m_details);
    m_console_rows->setObjectName("commandConsoleExamples");
    auto* rows = new QVBoxLayout(m_console_rows);
    rows->setContentsMargins(0, 0, 0, 0);
    details->addWidget(m_console_rows);
    details->addSpacing(8);
    m_command_line_title = new QLabel(tr("With defcon-cli and over HTTP:"), m_details);
    details->addWidget(m_command_line_title);
    m_command_line = new QPlainTextEdit(m_details);
    m_command_line->setObjectName("commandLineExamples");
    m_command_line->setAccessibleName(tr("Examples with defcon-cli and over HTTP"));
    m_command_line->setReadOnly(true);
    m_command_line->setFont(GUIUtil::fixedPitchFont());
    m_command_line->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_command_line->setMaximumHeight(120);
    details->addWidget(m_command_line);
    m_copy_command_line = new QPushButton(tr("Copy these examples"), m_details);
    m_copy_command_line->setObjectName("commandCopyCommandLine");
    connect(m_copy_command_line, &QPushButton::clicked, this, [this] { GUIUtil::setClipboard(m_command_line->toPlainText()); });
    details->addWidget(m_copy_command_line, 0, Qt::AlignLeft);
    m_no_examples = new QLabel(tr("The help of this command has no examples."), m_details);
    m_no_examples->setObjectName("commandNoExamples");
    details->addWidget(m_no_examples);
    details->addSpacing(8);
    m_full_help_button = new QPushButton(tr("Show the full help"), m_details);
    m_full_help_button->setObjectName("commandFullHelpButton");
    m_full_help_button->setCheckable(true);
    details->addWidget(m_full_help_button, 0, Qt::AlignLeft);
    m_full_help = new QPlainTextEdit(m_details);
    m_full_help->setObjectName("commandFullHelp");
    m_full_help->setAccessibleName(tr("Full help"));
    m_full_help->setReadOnly(true);
    m_full_help->setFont(GUIUtil::fixedPitchFont());
    m_full_help->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_full_help->setMinimumHeight(200);
    m_full_help->hide();
    details->addWidget(m_full_help, 1);
    details->addStretch();
    connect(m_full_help_button, &QPushButton::toggled, this, [this](bool shown) {
        m_full_help->setVisible(shown);
        m_full_help_button->setText(shown ? tr("Hide the full help") : tr("Show the full help"));
    });
    scroll->setWidget(m_details);
    // QScrollArea::setWidget turns the widget's background filling on, in the palette's window colour, which the
    // themes do not set: the details stood on a light grey block in Abyss and Dark. The page's own background shows.
    m_details->setAutoFillBackground(false);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    connect(m_search, &QLineEdit::textChanged, this, &CommandBrowser::filter);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        const QVariant index = item ? item->data(0, ENTRY_ROLE) : QVariant();
        showEntry(index.isValid() ? &m_entries.at(index.toUInt()) : nullptr);
    });
    showEntry(nullptr);
}

QStringList CommandBrowser::featuredTopics()
{
    return {QStringLiteral("Staking"), QStringLiteral("Sentinel"), QStringLiteral("Masternodes")};
}

namespace {
//! The labels of a command's flags, each with what it means, in the order the line shows them.
std::vector<std::pair<QString, QString>> FlagTexts(uint8_t flags)
{
    if (flags & CMDFLAG_SUBCOMMANDS) {
        return {{CommandBrowser::tr("Depends on the subcommand"),
                 CommandBrowser::tr("What it does depends on the subcommand given: choose one in the list to see its own line.")}};
    }
    std::vector<std::pair<QString, QString>> texts;
    if (flags & CMDFLAG_CHANGES) {
        texts.emplace_back(CommandBrowser::tr("Can change the wallet, the node or the network"),
                           CommandBrowser::tr("With some arguments it changes the wallet, the node's state or settings, or sends something to the network."));
    } else {
        texts.emplace_back(CommandBrowser::tr("Only reads"),
                           CommandBrowser::tr("It changes neither the wallet nor the node and sends nothing to the network; it may write a file."));
    }
    if ((flags & CMDFLAG_WALLET) && (flags & CMDFLAG_MASTERNODE_NO_WALLET)) {
        texts.emplace_back(CommandBrowser::tr("Needs a loaded wallet, except on a masternode"),
                           CommandBrowser::tr("It works on a loaded wallet and refuses without one; a masternode answers it from its own state, without a wallet."));
    } else if (flags & CMDFLAG_WALLET) {
        texts.emplace_back(CommandBrowser::tr("Needs a loaded wallet"),
                           CommandBrowser::tr("It works on a loaded wallet; without one it refuses or finds nothing to act on."));
    }
    if (flags & CMDFLAG_UNLOCK) {
        texts.emplace_back(CommandBrowser::tr("May ask to unlock the wallet"),
                           CommandBrowser::tr("On an encrypted wallet, some of its uses refuse until the wallet is unlocked."));
    }
    if (flags & CMDFLAG_BEFORE_POS) {
        texts.emplace_back(CommandBrowser::tr("Only on a new network, before proof of stake begins"),
                           CommandBrowser::tr("It makes proof-of-work blocks, which the chain accepts only up to its last proof-of-work block "
                                              "(999 on mainnet, 5000 on regtest): on mainnet it fails."));
    }
    return texts;
}
} // namespace

QString CommandBrowser::flagsLine(uint8_t flags)
{
    QStringList labels;
    for (const auto& text : FlagTexts(flags)) labels << text.first;
    return labels.join(QStringLiteral(" · "));
}

QString CommandBrowser::flagsToolTip(uint8_t flags)
{
    QStringList lines;
    for (const auto& text : FlagTexts(flags)) lines << tr("%1: %2").arg(text.first, text.second);
    return lines.join('\n');
}

void CommandBrowser::ensureLoaded()
{
    if (!m_loaded) load();
}

void CommandBrowser::load()
{
    // The topics and their order come from the main help, which lists every listed command that is not a
    // subcommand under its topic; a subcommand has its parent's topic.
    std::map<std::string, QString> topic_of;
    QStringList topics;
    std::vector<Entry> entries;
    try {
        const UniValue main_help = m_node.executeRpc("help", UniValue(UniValue::VARR), "");
        QString topic;
        for (const QString& line : QString::fromStdString(main_help.get_str()).split('\n')) {
            if (line.startsWith(QLatin1String("== ")) && line.endsWith(QLatin1String(" =="))) {
                topic = line.mid(3, line.size() - 6);
                topics << topic;
            } else if (!line.isEmpty()) {
                topic_of[line.section(' ', 0, 0).toStdString()] = topic;
            }
        }
        for (const std::string& name : m_node.listRpcCommands()) {
            const auto space = name.find(' ');
            const std::string parent = name.substr(0, space);
            UniValue params(UniValue::VARR);
            params.push_back(parent);
            if (space != std::string::npos) params.push_back(name.substr(space + 1));
            const UniValue help = m_node.executeRpc("help", params, "");
            const auto it = topic_of.find(parent);
            Entry entry = parseHelp(QString::fromStdString(name), it == topic_of.end() ? QString() : it->second,
                                    QString::fromStdString(help.get_str()));
            entry.flags = m_node.getRpcCommandFlags(name);
            entries.push_back(std::move(entry));
        }
    } catch (...) {
        // While the node starts, every call answers that RPC is warming up; the page asks again next time.
        m_status->show();
        return;
    }
    m_entries = std::move(entries);
    m_loaded = true;
    m_status->hide();

    QStringList order;
    for (const QString& topic : featuredTopics()) {
        if (topics.contains(topic)) order << topic;
    }
    for (const QString& topic : topics) {
        if (!order.contains(topic)) order << topic;
    }
    m_list->clear();
    for (const QString& topic : order) {
        auto* group = new QTreeWidgetItem(m_list, {topic});
        group->setFlags(Qt::ItemIsEnabled);
        QFont bold = group->font(0);
        bold.setBold(true);
        group->setFont(0, bold);
        // m_entries follows the registry's order, which is by name.
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].topic != topic) continue;
            auto* item = new QTreeWidgetItem(group, {m_entries[i].name});
            item->setData(0, ENTRY_ROLE, static_cast<uint>(i));
            item->setToolTip(0, m_entries[i].summary);
        }
        group->setExpanded(featuredTopics().contains(topic));
    }
    filter(m_search->text());
}

bool CommandBrowser::select(const QString& name)
{
    for (int g = 0; g < m_list->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = m_list->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c) {
            if (group->child(c)->text(0) == name) {
                group->setExpanded(true);
                m_list->setCurrentItem(group->child(c));
                return true;
            }
        }
    }
    return false;
}

void CommandBrowser::filter(const QString& text)
{
    const QString needle = text.trimmed();
    QTreeWidgetItem* first = nullptr;
    for (int g = 0; g < m_list->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = m_list->topLevelItem(g);
        int shown = 0;
        for (int c = 0; c < group->childCount(); ++c) {
            QTreeWidgetItem* item = group->child(c);
            const Entry& entry = m_entries.at(item->data(0, ENTRY_ROLE).toUInt());
            const bool match = needle.isEmpty() || entry.name.contains(needle, Qt::CaseInsensitive) ||
                               entry.summary.contains(needle, Qt::CaseInsensitive);
            item->setHidden(!match);
            if (match && !first) first = item;
            shown += match;
        }
        group->setHidden(shown == 0);
        if (!needle.isEmpty() && shown > 0) group->setExpanded(true);
    }
    QTreeWidgetItem* current = m_list->currentItem();
    if (!needle.isEmpty() && (!current || current->isHidden() || !current->data(0, ENTRY_ROLE).isValid())) {
        m_list->setCurrentItem(first);
        if (!first) showEntry(nullptr);
    }
}

bool CommandBrowser::eventFilter(QObject* object, QEvent* event)
{
    // From the search field, the down arrow goes on into the list.
    if (object == m_search && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Down) {
        m_list->setFocus();
        return true;
    }
    return QWidget::eventFilter(object, event);
}

void CommandBrowser::showEntry(const Entry* entry)
{
    m_details->setVisible(entry != nullptr);
    // The rows of the previous command go with it: out of the page at once, deleted when control is back in the
    // event loop (a row's own button may be what led here).
    for (QWidget* row : m_console_rows->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        row->hide();
        row->setParent(nullptr);
        row->deleteLater();
    }
    if (!entry) return;
    m_call->setText(entry->call);
    m_summary->setText(entry->summary);
    m_summary->setVisible(!entry->summary.isEmpty());
    m_topic->setText(tr("Topic: %1").arg(entry->topic));
    m_topic->setVisible(!entry->topic.isEmpty());
    m_flags->setVisible(entry->flags.has_value());
    m_flags->setText(entry->flags ? flagsLine(*entry->flags) : QString());
    m_flags->setToolTip(entry->flags ? flagsToolTip(*entry->flags) : QString());
    for (const QString& example : entry->console) {
        auto* row = new QWidget(m_console_rows);
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto* text = new QLineEdit(example, row);
        text->setReadOnly(true);
        text->setFont(GUIUtil::fixedPitchFont());
        text->setCursorPosition(0);
        text->setAccessibleName(tr("Example for this console"));
        line->addWidget(text, 1);
        auto* copy = new QPushButton(tr("Copy"), row);
        copy->setObjectName("commandCopyExample");
        connect(copy, &QPushButton::clicked, this, [example] { GUIUtil::setClipboard(example); });
        line->addWidget(copy);
        auto* put = new QPushButton(tr("Put in console"), row);
        put->setObjectName("commandPutInConsole");
        put->setToolTip(tr("Puts the example into the console's input line, to read or change before running it."));
        connect(put, &QPushButton::clicked, this, [this, example] { Q_EMIT putInConsole(example); });
        line->addWidget(put);
        m_console_rows->layout()->addWidget(row);
    }
    const bool console = !entry->console.isEmpty();
    const bool command_line = !entry->command_line.isEmpty();
    m_console_title->setVisible(console);
    m_console_rows->setVisible(console);
    m_command_line_title->setVisible(command_line);
    m_command_line->setVisible(command_line);
    m_copy_command_line->setVisible(command_line);
    m_command_line->setPlainText(entry->command_line.join('\n'));
    m_no_examples->setVisible(!console && !command_line);
    m_full_help->setPlainText(entry->help);
    m_full_help_button->setChecked(false);
}

CommandBrowser::Entry CommandBrowser::parseHelp(const QString& name, const QString& topic, const QString& help)
{
    Entry entry;
    entry.name = name;
    entry.topic = topic;
    entry.help = help;
    const QStringList lines = help.split('\n');
    if (lines.isEmpty()) return entry;
    entry.call = lines.front();

    // The summary: the description's first paragraph, from the second line to an empty line or a heading.
    QStringList paragraph;
    int i = 1;
    while (i < lines.size() && lines[i].trimmed().isEmpty()) ++i;
    for (; i < lines.size() && !lines[i].trimmed().isEmpty() && !IsHeading(lines[i]); ++i) {
        paragraph << lines[i].trimmed();
    }
    entry.summary = paragraph.join(' ');

    // The examples: the lines under "Examples:" that begin with "> ".
    const int examples = lines.indexOf(QStringLiteral("Examples:"));
    if (examples < 0) return entry;
    for (int j = examples + 1; j < lines.size(); ++j) {
        if (!lines[j].startsWith(QLatin1String("> "))) continue;
        const QString command = lines[j].mid(2).trimmed();
        if (command.startsWith(QLatin1String("curl "))) {
            AppendOnce(entry.command_line, command);
            continue;
        }
        if (!command.startsWith(QLatin1String("defcon-cli "))) continue;
        AppendOnce(entry.command_line, command);
        // In the console the same words, without the client's name. A line with an option before the command
        // (-named, -rpcwallet) has no console form: the console takes positional arguments only.
        const QStringList words = shellWords(command);
        if (words.size() < 2 || words[0] != QLatin1String("defcon-cli") || words[1].startsWith('-')) continue;
        QStringList typed{words[1]};
        for (int w = 2; w < words.size(); ++w) typed << consoleWord(words[w]);
        AppendOnce(entry.console, typed.join(' '));
    }
    return entry;
}

QStringList CommandBrowser::shellWords(const QString& line)
{
    enum class Quote { Bare, InSingle, InDouble } quote{Quote::Bare};
    QStringList words;
    QString word;
    bool in_word = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        switch (quote) {
        case Quote::Bare:
            if (c == ' ' || c == '\t') {
                if (in_word) words << word;
                word.clear();
                in_word = false;
            } else if (c == '\'') {
                quote = Quote::InSingle;
                in_word = true;
            } else if (c == '"') {
                quote = Quote::InDouble;
                in_word = true;
            } else if (c == '\\') {
                if (i + 1 >= line.size()) return {};
                word += line[++i];
                in_word = true;
            } else if (QStringLiteral("$`|&;<>()").contains(c)) {
                return {};
            } else {
                word += c;
                in_word = true;
            }
            break;
        case Quote::InSingle:
            if (c == '\'') quote = Quote::Bare; else word += c;
            break;
        case Quote::InDouble:
            if (c == '"') {
                quote = Quote::Bare;
            } else if (c == '\\' && i + 1 < line.size() && QStringLiteral("$`\"\\").contains(line[i + 1])) {
                word += line[++i];
            } else if (c == '$' || c == '`') {
                return {};
            } else {
                word += c;
            }
            break;
        }
    }
    if (quote != Quote::Bare) return {};
    if (in_word) words << word;
    return words;
}

QString CommandBrowser::consoleWord(const QString& word)
{
    // The console splits on spaces and commas, nests on parentheses and reads quotes and backslashes.
    static const QString special = QStringLiteral(" \t\n,'\"\\()[]{}");
    bool plain = !word.isEmpty();
    for (const QChar c : word) {
        if (special.contains(c)) {
            plain = false;
            break;
        }
    }
    if (plain) return word;
    // Inside single quotes the console takes everything as it is, up to the next single quote.
    if (!word.contains('\'')) return QLatin1Char('\'') + word + QLatin1Char('\'');
    // Inside double quotes a backslash keeps the next quote or backslash.
    QString escaped = word;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}
