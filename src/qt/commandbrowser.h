// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_COMMANDBROWSER_H
#define BITCOIN_QT_COMMANDBROWSER_H

#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

namespace interfaces {
class Node;
}

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
QT_END_NAMESPACE

/** The Commands page of the tools window: the commands the help lists, by topic and searchable, each with what
 *  its help says and its examples, those for this console ready to copy or to put into the console's input line.
 *  Everything comes from the node's own help; nothing is run from this page. */
class CommandBrowser : public QWidget
{
    Q_OBJECT

public:
    //! What the help of one command says, as the page shows it.
    struct Entry {
        QString name;
        QString topic;
        //! The first line of the help: the command and its arguments.
        QString call;
        //! The first paragraph of the description.
        QString summary;
        //! The help's defcon-cli examples written for this console: the same arguments, without "defcon-cli".
        QStringList console;
        //! The help's defcon-cli and curl examples, as the help gives them.
        QStringList command_line;
        //! The whole help.
        QString help;
    };

    explicit CommandBrowser(interfaces::Node& node, QWidget* parent = nullptr);

    //! Fill the page from the node the first time it is shown. While the node is still starting, the page says so
    //! and tries again the next time.
    void ensureLoaded();
    //! Show a command, as choosing it in the list does. False if the list does not have it.
    bool select(const QString& name);
    const std::vector<Entry>& entries() const { return m_entries; }

    //! Read one command's help.
    static Entry parseHelp(const QString& name, const QString& topic, const QString& help);
    //! The words a POSIX shell makes of a simple command line. Empty when a quote does not close or the line uses
    //! anything beyond words and quotes (a substitution, a pipe, a redirection).
    static QStringList shellWords(const QString& line);
    //! A word written so that the console's parser reads it back unchanged.
    static QString consoleWord(const QString& word);
    //! The topics the list starts with.
    static QStringList featuredTopics();

Q_SIGNALS:
    //! Put this text into the console's input line. Nothing is run.
    void putInConsole(const QString& command);

private:
    void load();
    void filter(const QString& text);
    void showEntry(const Entry* entry);
    bool eventFilter(QObject* object, QEvent* event) override;

    interfaces::Node& m_node;
    bool m_loaded{false};
    std::vector<Entry> m_entries;
    QLineEdit* m_search;
    QTreeWidget* m_list;
    QLabel* m_status;
    QWidget* m_details;
    QLabel* m_call;
    QLabel* m_summary;
    QLabel* m_topic;
    QLabel* m_console_title;
    QWidget* m_console_rows;
    QLabel* m_command_line_title;
    QPlainTextEdit* m_command_line;
    QPushButton* m_copy_command_line;
    QLabel* m_no_examples;
    QPushButton* m_full_help_button;
    QPlainTextEdit* m_full_help;
};

#endif // BITCOIN_QT_COMMANDBROWSER_H
