// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/abysstoast.h>

#include <qt/guiutil.h>

#include <QAccessible>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QSizePolicy>

AbyssToast::AbyssToast(QWidget* parent) :
    QFrame(parent),
    m_text(new QLabel(this)),
    m_opacity(new QGraphicsOpacityEffect(this)),
    m_fade(new QPropertyAnimation(m_opacity, "opacity", this))
{
    setObjectName("abyssToast");
    setFocusPolicy(Qt::NoFocus);
    // Neither a click target nor something that could swallow one.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    m_text->setObjectName("abyssToastText");
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 4, 10, 4);
    layout->addWidget(m_text);
    GUIUtil::setFont({m_text}, GUIUtil::FontWeight::Normal, 12);
    QSizePolicy policy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    policy.setRetainSizeWhenHidden(true);
    setSizePolicy(policy);
    m_opacity->setOpacity(0);
    setGraphicsEffect(m_opacity);
    m_fade->setDuration(DURATION_MS);
    m_fade->setKeyValueAt(0, 0.0);
    m_fade->setKeyValueAt(0.1, 1.0);
    m_fade->setKeyValueAt(0.8, 1.0);
    m_fade->setKeyValueAt(1, 0.0);
    connect(m_fade, &QPropertyAnimation::finished, this, &AbyssToast::finish);
    QFrame::hide();
}

void AbyssToast::showMessage(const QString& text)
{
    m_text->setText(text);
    setAccessibleName(text);
    m_fade->stop();
    m_opacity->setOpacity(0);
    show();
    // On a page that is not on screen nobody would see it: no animation then.
    if (!isVisible()) {
        finish();
        return;
    }
    m_fade->start();
    QAccessibleEvent alert(this, QAccessible::Alert);
    QAccessible::updateAccessibility(&alert);
}

bool AbyssToast::isShowing() const
{
    return m_fade->state() == QAbstractAnimation::Running;
}

QString AbyssToast::text() const
{
    return m_text->text();
}

bool AbyssToast::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::Hide:
    case QEvent::WindowDeactivate:
        // A window that goes away, or another one taking the focus, ends the
        // message now rather than leaving an animation running unseen.
        if (m_fade->state() != QAbstractAnimation::Stopped) {
            m_fade->stop();
            finish();
        }
        break;
    default:
        break;
    }
    return QFrame::event(event);
}

void AbyssToast::finish()
{
    m_opacity->setOpacity(0);
    QFrame::hide();
}
