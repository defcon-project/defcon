// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.

#include <qt/abyssmenus.h>

#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QFrame>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QThread>
#include <QToolButton>
#include <QWidgetAction>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace AbyssMenus {
namespace {

constexpr int DIVIDER_WIDTH = 1;

/** A menu text as a screen reader or a status line gives it: the mnemonic
 *  marker removed, "&&" back to "&". */
QString PlainText(const QString& text)
{
    QString plain;
    plain.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
                plain += QLatin1Char('&');
                ++i;
            }
            continue;
        }
        plain += text.at(i);
    }
    return plain;
}

/** The key a menu text's mnemonic answers to, compared the way QMenu does it. */
int MnemonicKey(const QString& text)
{
    const QKeySequence sequence = QKeySequence::mnemonic(text);
    return sequence.isEmpty() ? 0 : (sequence[0] & 0xffff);
}

QRect GlobalRect(const QWidget* widget)
{
    return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
}

/** A paired row: the two cells in equal halves with a divider between them,
 *  so the dividers of one menu line up. Mouse events pass through to the
 *  menu, whose controller handles them. */
class PairRow : public QWidget
{
public:
    PairRow(QMenu* menu, QAction* left, QAction* right, bool submenu)
    {
        setObjectName("abyssMenuPair");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        cells[0] = new PairCell(left, menu, false, this);
        cells[1] = new PairCell(right, menu, submenu, this);
        divider = new QFrame(this);
        divider->setObjectName("abyssMenuPairDivider");
        divider->setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    QSize sizeHint() const override
    {
        const QSize first = cells[0]->sizeHint();
        const QSize second = cells[1]->sizeHint();
        return {2 * std::max(first.width(), second.width()) + DIVIDER_WIDTH, std::max(first.height(), second.height())};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

    /** The cell under a point of this row; the divider counts to the nearer half. */
    int cellAt(const QPoint& point) const
    {
        for (int i : {0, 1}) {
            if (cells[i]->geometry().contains(point)) return i;
        }
        const int middle = divider->geometry().center().x();
        const bool first_side = point.x() < middle;
        return (first_side == (layoutDirection() == Qt::LeftToRight)) ? 0 : 1;
    }

    PairCell* cells[2];
    QFrame* divider;

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        const int half = (width() - DIVIDER_WIDTH) / 2;
        QRect first(0, 0, half, height());
        QRect second(half + DIVIDER_WIDTH, 0, width() - half - DIVIDER_WIDTH, height());
        if (layoutDirection() == Qt::RightToLeft) std::swap(first, second);
        cells[0]->setGeometry(first);
        cells[1]->setGeometry(second);
        const int inset = height() / 4;
        divider->setGeometry(half, inset, DIVIDER_WIDTH, std::max(0, height() - 2 * inset));
    }
};

/** One per menu holding paired rows: keeps each row's current cell and takes
 *  the menu's mouse and key events for its rows (design note: U3-B, §2). */
class PairedMenu : public QObject
{
public:
    static PairedMenu* of(QMenu* menu)
    {
        for (QObject* child : menu->children()) {
            if (auto* paired = dynamic_cast<PairedMenu*>(child)) return paired;
        }
        return new PairedMenu(menu);
    }

    QWidgetAction* add(QAction* left, QAction* right, QMenu* submenu)
    {
        auto pair = std::make_unique<Pair>();
        Pair* p = pair.get();
        p->action = new QWidgetAction(m_menu);
        p->action->setObjectName("abyssMenuPairAction");
        p->row = new PairRow(m_menu, left, right, submenu != nullptr);
        p->submenu = submenu;
        for (PairCell* cell : p->row->cells) cell->setRowAction(p->action);
        p->action->setDefaultWidget(p->row);
        m_pairs.push_back(std::move(pair));
        for (QAction* shared : {left, right}) {
            connect(shared, &QAction::changed, this, [this, p] { cellChanged(*p); });
        }
        sync(*p);
        m_menu->addAction(p->action);
        return p->action;
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != m_menu) return false;
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonRelease:
            return mouse(static_cast<QMouseEvent*>(event));
        case QEvent::KeyPress:
            return key(static_cast<QKeyEvent*>(event));
        case QEvent::Hide:
            // A new popup starts without a current cell, as QMenu starts without a current action.
            m_take_row = nullptr;
            for (auto& pair : m_pairs) setCurrent(*pair, -1);
            break;
        default:
            break;
        }
        return false;
    }

private:
    struct Pair {
        QWidgetAction* action{nullptr};
        PairRow* row{nullptr};
        QPointer<QMenu> submenu;
        int current{-1};
    };

    explicit PairedMenu(QMenu* menu) : QObject(menu), m_menu(menu)
    {
        menu->installEventFilter(this);
        connect(menu, &QMenu::hovered, this, [this](QAction* action) { hovered(action); });
    }

    QAction* cellAction(const Pair& p, int cell) const { return p.row->cells[cell]->action(); }

    bool enabled(const Pair& p, int cell) const
    {
        if (cell < 0 || cell > 1) return false;
        const QAction* action = cellAction(p, cell);
        return action && action->isEnabled() && action->isVisible();
    }

    int firstEnabled(const Pair& p) const { return enabled(p, 0) ? 0 : (enabled(p, 1) ? 1 : -1); }

    Pair* pairOf(const QAction* action) const
    {
        if (!action) return nullptr;
        for (const auto& pair : m_pairs) {
            if (pair->action == action) return pair.get();
        }
        return nullptr;
    }

    Pair* pairAt(const QPoint& menu_point) const
    {
        for (const auto& pair : m_pairs) {
            if (pair->row->isVisible() && pair->row->geometry().contains(menu_point)) return pair.get();
        }
        return nullptr;
    }

    /** The row action carries what QMenu shows for it: the current cell's text
     *  (without a mnemonic, so the row answers to none of its own) and status
     *  tip; it is enabled while either cell is. */
    void sync(Pair& p)
    {
        p.action->setEnabled(enabled(p, 0) || enabled(p, 1));
        const QAction* shown = cellAction(p, p.current >= 0 ? p.current : 0);
        if (shown) {
            p.action->setText(PlainText(shown->text()));
            p.action->setStatusTip(shown->statusTip());
        }
        for (int i : {0, 1}) p.row->cells[i]->setSelected(i == p.current);
    }

    void setCurrent(Pair& p, int cell)
    {
        if (!enabled(p, cell)) cell = -1;
        p.current = cell;
        // QMenu opens the current action's menu, and the current action is the
        // whole row, so the list may hang on the row only while its own cell is
        // current. A list still on screen while QMenu moves elsewhere is QMenu's
        // to close (it may be the pointer's way into it).
        QMenu* hang = nullptr;
        if (p.submenu) {
            if (cell == 1) {
                hang = p.submenu;
            } else if (cell == -1 && p.action->menu() == p.submenu && p.submenu->isVisible()) {
                hang = p.submenu;
            }
        }
        if (p.action->menu() != hang) {
            QMenu* was = p.action->menu();
            const bool was_open = was && was->isVisible();
            p.action->setMenu(hang);
            // An immediate popup request makes QMenu close the open list, whose
            // menu action is no longer the current one (QMenu::internalDelayedPopup).
            if (was_open && m_menu->activeAction() == p.action) m_menu->setActiveAction(p.action);
        }
        sync(p);
    }

    void cellChanged(Pair& p)
    {
        for (PairCell* cell : p.row->cells) {
            if (cell->action()) {
                cell->setAccessibleName(PlainText(cell->action()->text()));
                cell->setAccessibleDescription(cell->action()->statusTip());
            }
            cell->update();
        }
        p.row->updateGeometry();
        setCurrent(p, p.current);
    }

    void hovered(QAction* action)
    {
        // QMenu also says hovered() for an entry of an open submenu
        // (QMenuPrivate::activateCausedStack); the row of that submenu stays current.
        if (!m_menu->actions().contains(action)) return;
        for (auto& pair : m_pairs) {
            Pair& p = *pair;
            if (p.action == action) {
                const int cell = m_take_row == action ? m_take_cell : (p.current >= 0 ? p.current : firstEnabled(p));
                setCurrent(p, cell);
            } else if (p.current != -1) {
                setCurrent(p, -1);
            }
        }
    }

    /** QMenu shows the status tip and moves a screen reader's focus when it
     *  changes its item; a change of cell inside a row is ours to tell. */
    void announce(Pair& p)
    {
        if (p.current < 0) return;
        if (QAction* action = cellAction(p, p.current)) action->showStatusText(m_menu->parentWidget());
#ifndef QT_NO_ACCESSIBILITY
        if (QAccessible::isActive()) {
            QAccessibleEvent focus(m_menu, QAccessible::Focus);
            focus.setChild(m_menu->actions().indexOf(p.action));
            QAccessible::updateAccessibility(&focus);
        }
#endif
    }

    /** Close the menu, then trigger, the order QMenu uses for its own items. */
    void activate(Pair& p, int cell)
    {
        QPointer<QAction> action = cellAction(p, cell);
        m_menu->hide();
        if (action && action->isEnabled()) action->trigger();
    }

    void take(Pair& p, int cell)
    {
        m_take_row = p.action;
        m_take_cell = cell;
        setCurrent(p, cell);
    }

    bool mouse(QMouseEvent* event)
    {
        Pair* p = pairAt(event->pos());
        if (!p) {
            if (event->type() == QEvent::MouseMove) m_take_row = nullptr;
            return false;
        }
        const int at = p->row->cellAt(p->row->mapFrom(m_menu, event->pos()));
        const int cell = enabled(*p, at) ? at : -1;
        const bool list = cell == 1 && p->submenu;
        switch (event->type()) {
        case QEvent::MouseMove:
            // Then QMenu: its highlight, status tip and list timing stay in charge.
            take(*p, cell);
            return false;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            take(*p, cell);
            return !list; // QMenu opens the list on a press
        case QEvent::MouseButtonRelease:
            if (list) return false; // QMenu does nothing on a release over a list item
            if (cell >= 0 && event->button() == Qt::LeftButton) activate(*p, cell);
            return true; // QMenu would trigger the row action and close the menu
        default:
            return false;
        }
    }

    bool key(QKeyEvent* event)
    {
        m_take_row = nullptr;
        if (Pair* p = pairOf(m_menu->activeAction())) {
            int key = event->key();
            if (m_menu->isRightToLeft()) {
                if (key == Qt::Key_Left) {
                    key = Qt::Key_Right;
                } else if (key == Qt::Key_Right) {
                    key = Qt::Key_Left;
                }
            }
            switch (key) {
            case Qt::Key_Left:
            case Qt::Key_Right: {
                const int target = key == Qt::Key_Right ? 1 : 0;
                if (p->current == target) {
                    // Right on the list's cell: QMenu opens the list, its first entry active.
                    return !(target == 1 && p->submenu && enabled(*p, 1));
                }
                if (enabled(*p, target)) {
                    take(*p, target);
                    announce(*p);
                }
                return true;
            }
            case Qt::Key_Space:
                if (!m_menu->style()->styleHint(QStyle::SH_Menu_SpaceActivatesItem, nullptr, m_menu)) break;
                [[fallthrough]];
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (p->current < 0) return true;
                if (p->current == 1 && p->submenu) return false; // QMenu opens the list
                activate(*p, p->current);
                return true;
            default:
                break;
            }
        }
        return mnemonic(event);
    }

    /** QMenu finds mnemonics in its actions' texts only; a row's cells are
     *  matched here, with QMenu's rule: one match acts, several are visited
     *  one after the other. Only enabled entries match: a disabled one can be
     *  neither selected nor triggered, and visiting it would stop the turn at
     *  it (with every wallet closed, Wallet's "B" would never reach "Show
     *  Automatic Backups" behind the disabled "Backup Wallet…"). */
    bool mnemonic(QKeyEvent* event)
    {
        const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
        if (event->text().size() != 1) return false;
        if (modifiers != Qt::NoModifier && modifiers != Qt::AltModifier && modifiers != Qt::ShiftModifier) return false;
        if (modifiers == Qt::NoModifier && m_menu->style()->styleHint(QStyle::SH_Menu_KeyboardSearch, nullptr, m_menu)) return false;
        const int key = event->text().at(0).toUpper().unicode();
        if (key == 0) return false;
        struct Match {
            QAction* plain;
            Pair* pair;
            int cell;
        };
        std::vector<Match> matches;
        int current = -1;
        for (QAction* action : m_menu->actions()) {
            if (action->isSeparator() || !action->isVisible()) continue;
            if (Pair* p = pairOf(action)) {
                for (int i : {0, 1}) {
                    if (!enabled(*p, i) || MnemonicKey(cellAction(*p, i)->text()) != key) continue;
                    if (m_menu->activeAction() == action && p->current == i) current = static_cast<int>(matches.size());
                    matches.push_back({nullptr, p, i});
                }
            } else if (action->isEnabled() && MnemonicKey(action->text()) == key) {
                if (m_menu->activeAction() == action) current = static_cast<int>(matches.size());
                matches.push_back({action, nullptr, -1});
            }
        }
        if (std::none_of(matches.begin(), matches.end(), [](const Match& m) { return m.pair != nullptr; })) return false;
        const Match next = (matches.size() == 1 || current < 0) ? matches.front() : matches[(current + 1) % matches.size()];
        if (next.plain) {
            m_menu->setActiveAction(next.plain);
            return true;
        }
        Pair& p = *next.pair;
        take(p, next.cell);
        if (matches.size() == 1 && !(next.cell == 1 && p.submenu)) {
            activate(p, next.cell);
        } else {
            // Selects the cell; the list's cell opens its list, as QMenu opens
            // a submenu reached by its mnemonic.
            m_menu->setActiveAction(p.action);
            announce(p);
        }
        return true;
    }

    QMenu* const m_menu;
    std::vector<std::unique_ptr<Pair>> m_pairs;
    QPointer<QAction> m_take_row; // the row whose current cell the next hovered() keeps
    int m_take_cell{-1};
};

} // namespace

PairCell::PairCell(QAction* action, QMenu* menu, bool submenu, QWidget* parent)
    : QWidget(parent), m_action(action), m_menu(menu), m_submenu(submenu)
{
    setObjectName("abyssMenuCell");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(PlainText(action->text()));
    setAccessibleDescription(action->statusTip());
}

void PairCell::setSelected(bool selected)
{
    if (m_selected == selected) return;
    m_selected = selected;
    update();
}

QStyleOptionMenuItem PairCell::option() const
{
    // As QMenu::initStyleOption fills it for an item of this menu.
    QStyleOptionMenuItem option;
    option.initFrom(m_menu);
    option.rect = rect();
    option.state = QStyle::State_None;
    if (m_menu->isActiveWindow()) option.state |= QStyle::State_Active;
    const bool enabled = m_action && m_action->isEnabled() && m_menu->isEnabled();
    if (enabled) {
        option.state |= QStyle::State_Enabled;
    } else {
        option.palette.setCurrentColorGroup(QPalette::Disabled);
    }
    if (enabled && m_selected && m_row && m_menu->activeAction() == m_row) option.state |= QStyle::State_Selected;
    option.font = m_action ? m_action->font().resolve(m_menu->font()) : m_menu->font();
    // A widget's font() comes with nothing marked as set, and a painter
    // resolves its font against the device: on this cell, QPainter::setFont
    // would put the cell's own font (the application's) in place of the
    // menu's. QMenu paints on itself, so its entries keep the menu's font,
    // which a desktop sets apart (Windows, macOS, KDE: the classes' fonts).
    // Marked as set in full, the menu's font wins here too, as it measures.
    option.font.resolve(QFont::AllPropertiesResolved);
    option.fontMetrics = QFontMetrics(option.font);
    option.menuItemType = m_submenu ? QStyleOptionMenuItem::SubMenu : QStyleOptionMenuItem::Normal;
    option.checkType = QStyleOptionMenuItem::NotCheckable;
    option.menuHasCheckableItems = false;
    QString text = m_action ? m_action->text() : QString();
    if (m_action && !text.contains(QLatin1Char('\t')) && !m_action->shortcut().isEmpty()) {
        text += QLatin1Char('\t') + m_action->shortcut().toString(QKeySequence::NativeText);
    }
    option.text = text;
    option.tabWidth = 0;
    option.maxIconWidth = 0;
    option.menuRect = m_menu->rect();
    return option;
}

QSize PairCell::sizeHint() const
{
    if (!m_menu) return QWidget::sizeHint();
    const QStyleOptionMenuItem option = this->option();
    QString text = option.text;
    QString shortcut;
    const int tab = text.indexOf(QLatin1Char('\t'));
    if (tab >= 0) {
        shortcut = text.mid(tab + 1);
        text = text.left(tab);
    }
    // QMenu's own measure (QMenuPrivate::updateActionRects).
    QSize size(option.fontMetrics.boundingRect(QRect(), Qt::TextSingleLine | Qt::TextShowMnemonic, text).width(),
               std::max(option.fontMetrics.height(), m_menu->fontMetrics().height()));
    size = m_menu->style()->sizeFromContents(QStyle::CT_MenuItem, &option, size, m_menu);
    if (!shortcut.isEmpty()) size.rwidth() += m_menu->fontMetrics().horizontalAdvance(shortcut);
    return size;
}

void PairCell::paintEvent(QPaintEvent*)
{
    if (!m_menu) return;
    QPainter painter(this);
    const QStyleOptionMenuItem option = this->option();
    m_menu->style()->drawControl(QStyle::CE_MenuItem, &option, &painter, m_menu);
}

QWidgetAction* addPair(QMenu* menu, QAction* left, QAction* right, QMenu* rightSubmenu)
{
    return PairedMenu::of(menu)->add(left, right, rightSubmenu);
}

QList<PairCell*> pairCells(QAction* row)
{
    auto* action = qobject_cast<QWidgetAction*>(row);
    auto* pair = action ? dynamic_cast<PairRow*>(action->defaultWidget()) : nullptr;
    if (!pair) return {};
    return {pair->cells[0], pair->cells[1]};
}

HoverMenus::HoverMenus(const QList<QToolButton*>& buttons, QObject* parent) : QObject(parent)
{
    m_open_timer.setSingleShot(true);
    m_open_timer.setInterval(OPEN_DELAY_MS);
    m_close_timer.setSingleShot(true);
    m_close_timer.setInterval(CLOSE_DELAY_MS);
    connect(&m_open_timer, &QTimer::timeout, this, [this] {
        QPointer<QToolButton> button = m_hover;
        m_hover = nullptr;
        if (!button || m_open || !button->isVisible() || !button->isEnabled()) return;
        if (!button->window()->isActiveWindow() || QApplication::activePopupWidget()) return;
        request(button);
    });
    connect(&m_close_timer, &QTimer::timeout, this, [this] { closeIfLeft(); });
    for (QToolButton* button : buttons) {
        if (!button || !button->menu()) continue;
        m_buttons.append(button);
        button->installEventFilter(this);
        QPointer<QToolButton> guard(button);
        connect(button->menu(), &QMenu::aboutToShow, this, [this, guard] { if (guard) menuShown(guard); });
        connect(button->menu(), &QMenu::aboutToHide, this, [this, guard] { if (guard) menuHidden(guard); });
    }
}

QList<QToolButton*> HoverMenus::buttons() const
{
    QList<QToolButton*> buttons;
    for (const auto& button : m_buttons) {
        if (button) buttons.append(button);
    }
    return buttons;
}

QToolButton* HoverMenus::watchedButton(QObject* object) const
{
    for (const auto& button : m_buttons) {
        if (button && button == object) return button;
    }
    return nullptr;
}

QToolButton* HoverMenus::buttonAt(const QPoint& global) const
{
    for (const auto& button : m_buttons) {
        if (button && button->isVisible() && button->isEnabled() && GlobalRect(button).contains(global)) return button;
    }
    return nullptr;
}

bool HoverMenus::overOpenMenu(const QPoint& global) const
{
    if (!m_open || !m_open->menu()) return false;
    // The open menu and every open submenu below it, however deep.
    std::vector<QMenu*> menus{m_open->menu()};
    for (size_t i = 0; i < menus.size() && i < 64; ++i) {
        QMenu* menu = menus[i];
        if (!menu->isVisible()) continue;
        if (menu->frameGeometry().contains(global)) return true;
        for (QAction* action : menu->actions()) {
            QMenu* sub = action->menu();
            if (sub && sub->isVisible() && std::find(menus.begin(), menus.end(), sub) == menus.end()) menus.push_back(sub);
        }
    }
    return false;
}

bool HoverMenus::eventFilter(QObject* watched, QEvent* event)
{
    // After a menu closed under the pointer, its button opens nothing on hover
    // until the pointer has left it. The button gets no Leave of its own when
    // the popup was the last widget to have the pointer, so the application
    // filter stays installed until a move lands outside the button.
    if (!m_open && m_blocked && event->type() == QEvent::MouseMove &&
        !GlobalRect(m_blocked).contains(static_cast<QMouseEvent*>(event)->globalPos())) {
        m_blocked = nullptr;
        qApp->removeEventFilter(this);
    }
    if (QToolButton* button = watchedButton(watched)) {
        switch (event->type()) {
        case QEvent::Enter:
            if (button != m_blocked && !m_open) {
                m_hover = button;
                m_open_timer.start();
            }
            break;
        case QEvent::Leave:
        case QEvent::Hide:
            if (m_hover == button) {
                m_open_timer.stop();
                m_hover = nullptr;
            }
            if (m_blocked == button) {
                m_blocked = nullptr;
                if (!m_open) qApp->removeEventFilter(this);
            }
            break;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            m_open_timer.stop();
            m_hover = nullptr;
            // QToolButton opens its menu inside this press: a mouse session.
            m_next_is_mouse = true;
            QTimer::singleShot(0, this, [this] { m_next_is_mouse = false; });
            break;
        default:
            break;
        }
        return false;
    }
    // The application filter, while a watched menu is open.
    if (m_open) {
        if (event->type() == QEvent::MouseMove) {
            pointerMoved(static_cast<QMouseEvent*>(event)->globalPos());
        } else if (event->type() == QEvent::KeyPress) {
            m_mouse_session = false;
            m_close_timer.stop();
        }
    }
    return false;
}

void HoverMenus::request(QToolButton* button)
{
    m_open_timer.stop();
    m_hover = nullptr;
    m_pending = button;
    const bool was_open = m_open;
    if (was_open) m_open->menu()->hide(); // its exec() returns; the pending menu follows
    if (m_draining) return;               // the running loop opens it
    if (was_open) {
        // Inside another menu's exec() (opened by a click or the keyboard):
        // open after it has returned, never inside it.
        drainWhenUnwound();
        return;
    }
    drain();
}

void HoverMenus::drainWhenUnwound(int tries)
{
    if (m_drain_queued) return;
    // Any processEvents() before the closed menu's exec() has returned would
    // run a plain queued call inside that exec(); the loop level says whether
    // it has returned (it was shown from m_shown_level, its exec() one deeper).
    // A switch still waiting after about two seconds is dropped.
    const bool inside = QThread::currentThread()->loopLevel() > m_shown_level;
    if (inside && tries >= 200) {
        m_pending = nullptr;
        return;
    }
    m_drain_queued = true;
    QTimer::singleShot(inside ? 10 : 0, this, [this, tries] {
        m_drain_queued = false;
        if (QThread::currentThread()->loopLevel() > m_shown_level) {
            drainWhenUnwound(tries + 1);
            return;
        }
        drain();
    });
}

void HoverMenus::drain()
{
    if (m_draining) return;
    m_draining = true;
    QPointer<HoverMenus> alive(this);
    while (alive && m_pending) {
        QPointer<QToolButton> button = m_pending;
        m_pending = nullptr;
        if (!button || !button->isVisible() || !button->isEnabled() || !button->menu()) continue;
        if (QApplication::activePopupWidget()) continue;
        m_next_is_mouse = true;
        button->showMenu(); // QToolButton runs the menu with exec(): returns once it has closed
    }
    if (alive) m_draining = false;
}

void HoverMenus::menuShown(QToolButton* button)
{
    const bool mouse = m_next_is_mouse;
    m_next_is_mouse = false;
    // The Help menu is the classic one, shared; opened from the classic menu
    // bar (the Abyss button hidden), it is none of ours.
    if (!button->isVisible()) return;
    m_open = button;
    m_shown_level = QThread::currentThread()->loopLevel();
    m_mouse_session = mouse;
    m_open_timer.stop();
    m_hover = nullptr;
    m_close_timer.stop();
    qApp->installEventFilter(this);
}

void HoverMenus::menuHidden(QToolButton* button)
{
    if (button != m_open) return;
    m_close_timer.stop();
    m_open = nullptr;
    m_mouse_session = false;
    // Closed with the pointer on its own button (a click on it, Escape): Qt
    // sends the button an Enter on the next move, which must not open the menu
    // again; it opens on hover once the pointer has left the button.
    if (button->isVisible() && m_pending != button && GlobalRect(button).contains(QCursor::pos())) {
        m_blocked = button; // the application filter stays until the pointer has left it
    } else {
        qApp->removeEventFilter(this);
    }
}

void HoverMenus::pointerMoved(const QPoint& global)
{
    if (!m_open) return;
    // A menu on top of a button (a small screen) is what the pointer is on.
    if (overOpenMenu(global)) {
        m_close_timer.stop();
        return;
    }
    QToolButton* button = buttonAt(global);
    if (button && button != m_open) {
        request(button);
        return;
    }
    if (button == m_open) {
        m_close_timer.stop();
        return;
    }
    if (m_mouse_session && !m_close_timer.isActive()) m_close_timer.start();
}

void HoverMenus::closeIfLeft()
{
    if (!m_open || !m_mouse_session) return;
    const QPoint pointer = QCursor::pos();
    if (overOpenMenu(pointer) || GlobalRect(m_open).contains(pointer)) return;
    m_open->menu()->hide();
}

} // namespace AbyssMenus
