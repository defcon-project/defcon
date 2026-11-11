// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.

#ifndef BITCOIN_QT_ABYSSMENUS_H
#define BITCOIN_QT_ABYSSMENUS_H

#include <QAction>
#include <QList>
#include <QMenu>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QStyleOptionMenuItem>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <QWidgetAction>

/** The Abyss theme's top menus (frontend roadmap U3-B). Presentation only:
 *  the actions shown here are the window's existing ones, shared with the
 *  classic menus, and nothing here changes their text, shortcut or state. */
namespace AbyssMenus {

/** One half of a paired menu row. It paints its action as an item of the
 *  row's menu, through that menu's style, so the menu's stylesheet applies;
 *  it takes no mouse events and no focus (the menu's controller handles both,
 *  see addPair). */
class PairCell : public QWidget
{
public:
    PairCell(QAction* action, QMenu* menu, bool submenu, QWidget* parent);
    QAction* action() const { return m_action; }
    /** Opens a list of its own (the right cell of a row given a submenu). */
    bool hasSubmenu() const { return m_submenu; }
    /** The row's current cell, drawn highlighted while its row is the menu's
     *  active action. */
    bool isSelected() const { return m_selected; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

    // Set by the menu's controller.
    void setSelected(bool selected);
    void setRowAction(QWidgetAction* row) { m_row = row; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QStyleOptionMenuItem option() const;
    QPointer<QAction> m_action;
    QPointer<QMenu> m_menu;
    QPointer<QWidgetAction> m_row;
    const bool m_submenu;
    bool m_selected{false};
};

/** Appends to `menu` a row holding two existing actions side by side, each in
 *  a cell of its own that highlights and triggers separately. The actions are
 *  not added to the menu (and never hidden: a hidden QAction is also a
 *  disabled one, and these are shared). With `rightSubmenu`, the right cell
 *  opens that menu instead of triggering its action. Rows are meant for
 *  top-level popup menus. Returns the row's QWidgetAction. */
QWidgetAction* addPair(QMenu* menu, QAction* left, QAction* right, QMenu* rightSubmenu = nullptr);

/** A row's two cells, left then right; empty for any other action. */
QList<PairCell*> pairCells(QAction* row);

/** Opens the menus of the given menu buttons when the pointer rests on them,
 *  switches to a neighbour's menu when the pointer moves onto that button
 *  while one is open, and closes a menu opened with the mouse once the
 *  pointer has left the button, the menu and its open submenus. A menu opened
 *  with the keyboard (or in which a key was pressed) is not closed by the
 *  pointer. Each button must have a menu shown by QToolButton::showMenu(). */
class HoverMenus : public QObject
{
public:
    static constexpr int OPEN_DELAY_MS = 250;
    static constexpr int CLOSE_DELAY_MS = 400;

    HoverMenus(const QList<QToolButton*>& buttons, QObject* parent);

    /** The buttons watched. */
    QList<QToolButton*> buttons() const;
    /** The button whose menu is open, if one is. */
    QToolButton* openButton() const { return m_open; }
    /** Whether the open menu closes when the pointer leaves it. */
    bool mouseSession() const { return m_mouse_session; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QToolButton* watchedButton(QObject* object) const;
    QToolButton* buttonAt(const QPoint& global) const;
    bool overOpenMenu(const QPoint& global) const;
    void request(QToolButton* button);
    void drainWhenUnwound(int tries = 0);
    void drain();
    void menuShown(QToolButton* button);
    void menuHidden(QToolButton* button);
    void pointerMoved(const QPoint& global);
    void closeIfLeft();

    QList<QPointer<QToolButton>> m_buttons;
    QTimer m_open_timer;
    QTimer m_close_timer;
    QPointer<QToolButton> m_hover;   // the button the open timer runs for
    QPointer<QToolButton> m_open;    // the button whose menu is open
    QPointer<QToolButton> m_pending; // the next menu to open
    QPointer<QToolButton> m_blocked; // no hover-open until the pointer leaves it
    bool m_next_is_mouse{false};
    bool m_mouse_session{false};
    bool m_draining{false};
    bool m_drain_queued{false};
    int m_shown_level{0}; // the event-loop level the open menu was shown from
};

} // namespace AbyssMenus

#endif // BITCOIN_QT_ABYSSMENUS_H
