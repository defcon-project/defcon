// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_EMPTYSTATEWIDGET_H
#define BITCOIN_QT_EMPTYSTATEWIDGET_H

#include <QFrame>
#include <QString>

QT_BEGIN_NAMESPACE
class QAbstractScrollArea;
class QLabel;
class QPushButton;
class QWidget;
QT_END_NAMESPACE

/**
 * What Abyss shows where a list has nothing to show: a glyph, a title, one
 * sentence and one action. When only a filter hides the rows it offers to
 * clear the filter instead, and while the wallet is still catching up it
 * draws placeholder rows. Laid over a list's viewport, the list keeps its own
 * header. The pages decide the state; classic themes always pass Hidden, so
 * they never see it.
 */
class EmptyStateWidget : public QFrame
{
    Q_OBJECT

public:
    enum class State {
        Hidden,   //!< the list has rows, or the theme is classic
        Empty,    //!< nothing yet: the page's own sentence and action
        Filtered, //!< there are rows, but the filters hide all of them
        Loading,  //!< nothing yet, and the wallet is still synchronizing
    };

    //! The state for a list showing `rows` of `unfiltered_rows`.
    static State stateFor(bool modern, int rows, int unfiltered_rows, bool loading);

    EmptyStateWidget(const QString& glyph, const QString& title, const QString& text,
                     const QString& action, QWidget* parent);

    //! Lie over `list`'s viewport and follow it as the list is laid out.
    void coverViewport(QAbstractScrollArea* list);
    //! A second, quieter button below the action, for an existing operation.
    QPushButton* addSecondaryButton(const QString& text);

    void setState(State state);
    State state() const { return m_state; }
    QPushButton* actionButton() const { return m_action; }

Q_SIGNALS:
    //! The page's own action, offered in the Empty state.
    void actionClicked();
    //! Clear filters, offered in the Filtered state.
    void clearFiltersClicked();
    //! The list became visible. A page never shown before has not been
    //! polished, so a theme change passed it by without a StyleChange; it
    //! decides its state again here.
    void listShown();

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    State m_state{State::Hidden};
    QString m_glyph;
    QString m_title_text;
    QString m_text_text;
    QString m_action_text;
    QAbstractScrollArea* m_list{nullptr};
    //! The list's minimum height before this raised it, and what it raised it to.
    int m_list_minimum{0};
    int m_floor{0};
    QWidget* m_message{nullptr};
    QWidget* m_skeleton{nullptr};
    QLabel* m_symbol{nullptr};
    QLabel* m_title{nullptr};
    QLabel* m_text{nullptr};
    QPushButton* m_action{nullptr};

    void paintSymbol();
    void followViewport();
    void fitList();
};

#endif // BITCOIN_QT_EMPTYSTATEWIDGET_H
