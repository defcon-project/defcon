// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/emptystatewidget.h>

#include <qt/guiutil.h>

#include <QAbstractScrollArea>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <tuple>

EmptyStateWidget::State EmptyStateWidget::stateFor(bool modern, int rows, int unfiltered_rows, bool loading)
{
    if (!modern || rows > 0) return State::Hidden;
    // A filter explains an empty list better than a sync still running.
    if (unfiltered_rows > 0) return State::Filtered;
    return loading ? State::Loading : State::Empty;
}

EmptyStateWidget::EmptyStateWidget(const QString& glyph, const QString& title, const QString& text,
                                   const QString& action, QWidget* parent) :
    QFrame(parent),
    m_glyph(glyph),
    m_title_text(title),
    m_text_text(text),
    m_action_text(action)
{
    setObjectName("emptyState");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(0);

    // The message: a glyph in a ring, a title, one sentence and one action.
    m_message = new QWidget(this);
    m_message->setObjectName("emptyStateMessage");
    auto* message = new QVBoxLayout(m_message);
    message->setContentsMargins(0, 0, 0, 0);
    message->setSpacing(0);
    message->addStretch();
    auto* ring = new QFrame(m_message);
    ring->setObjectName("emptyStateSymbol");
    ring->setFixedSize(64, 64);
    auto* ring_layout = new QHBoxLayout(ring);
    ring_layout->setContentsMargins(0, 0, 0, 0);
    m_symbol = new QLabel(ring);
    m_symbol->setObjectName("emptyStateGlyph");
    m_symbol->setFixedSize(30, 30);
    ring_layout->addWidget(m_symbol, 0, Qt::AlignCenter);
    message->addWidget(ring, 0, Qt::AlignHCenter);
    message->addSpacing(14);
    m_title = new QLabel(m_message);
    m_title->setObjectName("emptyStateTitle");
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setWordWrap(true);
    message->addWidget(m_title);
    message->addSpacing(6);
    m_text = new QLabel(m_message);
    m_text->setObjectName("emptyStateText");
    m_text->setAlignment(Qt::AlignCenter);
    m_text->setWordWrap(true);
    m_text->setMaximumWidth(360);
    m_text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    // Placed with an alignment flag, a word-wrapped label keeps the height of
    // one line. Centred by stretches in a row of its own, it gets the height
    // its lines need at the width it is given.
    auto* text_row = new QHBoxLayout;
    text_row->setSpacing(0);
    text_row->addStretch(0);
    text_row->addWidget(m_text, 1);
    text_row->addStretch(0);
    message->addLayout(text_row);
    message->addSpacing(16);
    m_action = new QPushButton(m_message);
    m_action->setObjectName("emptyStateAction");
    m_action->setMinimumWidth(150);
    message->addWidget(m_action, 0, Qt::AlignHCenter);
    message->addStretch();
    layout->addWidget(m_message);
    connect(m_action, &QPushButton::clicked, this, [this] {
        if (m_state == State::Filtered) {
            Q_EMIT clearFiltersClicked();
        } else {
            Q_EMIT actionClicked();
        }
    });

    // Placeholder rows, as the list will look once its data is in.
    m_skeleton = new QWidget(this);
    m_skeleton->setObjectName("emptyStateSkeleton");
    auto* skeleton = new QVBoxLayout(m_skeleton);
    skeleton->setContentsMargins(12, 12, 12, 0);
    skeleton->setSpacing(24);
    for (int row = 0; row < 3; ++row) {
        auto* line = new QHBoxLayout;
        line->setSpacing(14);
        auto* dot = new QFrame(m_skeleton);
        dot->setObjectName("skeletonDot");
        dot->setFixedSize(36, 36);
        line->addWidget(dot);
        auto* bars = new QVBoxLayout;
        bars->setSpacing(9);
        // A long bar over a short one, about three quarters and two fifths wide.
        for (const auto& [name, width, height] : {std::tuple{"skeletonLine", 76, 11}, std::tuple{"skeletonLineShort", 42, 8}}) {
            auto* bar_row = new QHBoxLayout;
            bar_row->setSpacing(0);
            auto* bar = new QFrame(m_skeleton);
            bar->setObjectName(name);
            bar->setFixedHeight(height);
            bar_row->addWidget(bar, width);
            bar_row->addStretch(100 - width);
            bars->addLayout(bar_row);
        }
        line->addLayout(bars, 1);
        skeleton->addLayout(line);
    }
    auto* loading = new QLabel(tr("Loading…"), m_skeleton);
    loading->setObjectName("emptyStateLoading");
    loading->setAlignment(Qt::AlignCenter);
    skeleton->addSpacing(6);
    skeleton->addWidget(loading);
    skeleton->addStretch();
    layout->addWidget(m_skeleton);

    GUIUtil::setFont({m_title}, GUIUtil::FontWeight::Bold, 16);
    GUIUtil::setFont({m_text, loading}, GUIUtil::FontWeight::Normal, 13);
    GUIUtil::updateFonts();
    paintSymbol();
    hide();
}

void EmptyStateWidget::coverViewport(QAbstractScrollArea* list)
{
    if (parentWidget() != list) setParent(list);
    m_list = list;
    list->viewport()->installEventFilter(this);
    followViewport();
}

QPushButton* EmptyStateWidget::addSecondaryButton(const QString& text)
{
    auto* message = static_cast<QVBoxLayout*>(m_message->layout());
    auto* button = new QPushButton(text, m_message);
    button->setObjectName("emptyStateSecondary");
    button->setMinimumWidth(150);
    // Before the closing stretch, under the action.
    message->insertSpacing(message->count() - 1, 10);
    message->insertWidget(message->count() - 1, button, 0, Qt::AlignHCenter);
    return button;
}

void EmptyStateWidget::setState(State state)
{
    m_state = state;
    if (state == State::Hidden) {
        hide();
        fitList();
        return;
    }
    const bool filtered = state == State::Filtered;
    m_title->setText(filtered ? tr("No matching results") : m_title_text);
    m_text->setText(filtered ? tr("Try a different search or clear your filters.") : m_text_text);
    m_action->setText(filtered ? tr("Clear filters") : m_action_text);
    m_message->setVisible(state != State::Loading);
    m_skeleton->setVisible(state == State::Loading);
    setAccessibleName(state == State::Loading ? tr("Loading…") : m_title->text());
    followViewport();
    show();
    raise();
    fitList();
}

void EmptyStateWidget::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event->type() == QEvent::StyleChange) paintSymbol();
}

bool EmptyStateWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (m_list && watched == m_list->viewport()) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
            followViewport();
            fitList();
            break;
        case QEvent::Show:
            followViewport();
            Q_EMIT listShown();
            fitList();
            break;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void EmptyStateWidget::paintSymbol()
{
    if (!GUIUtil::isModernTheme()) return;
    GUIUtil::setTintedSvgPixmap(m_symbol, QString(":/icons/tabler/%1.svg").arg(m_glyph), m_symbol->size(),
                                GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BLUE));
}

void EmptyStateWidget::followViewport()
{
    // A child of the list rather than of its viewport: the viewport scrolls
    // its children along with the rows, and the header must stay uncovered.
    if (m_list) setGeometry(m_list->viewport()->geometry());
}

void EmptyStateWidget::fitList()
{
    if (!m_list) return;
    if (m_state == State::Hidden) {
        // Give the list its own minimum back, unless its page set another meanwhile.
        if (m_floor > 0 && m_list->minimumHeight() == m_floor) m_list->setMinimumHeight(m_list_minimum);
        m_floor = 0;
        return;
    }
    // A short list (a dialog's table, the payment history under its form)
    // grows until the message fits under the header.
    const int width = m_list->viewport()->width();
    // Until the list is laid out its width says nothing about the wrapping.
    if (!m_list->isVisible() || width <= 0) return;
    const int content = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : layout()->totalMinimumSize().height();
    const int needed = m_list->height() - m_list->viewport()->height() + content;
    if (needed <= m_list->minimumHeight()) return;
    if (m_floor == 0) m_list_minimum = m_list->minimumHeight();
    m_floor = needed;
    m_list->setMinimumHeight(needed);
}
