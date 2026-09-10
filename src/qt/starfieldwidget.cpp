// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/starfieldwidget.h>

#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>

#include <cmath>
#include <random>

namespace {
/**
 * The sky is generated once into a virtual field, and the widget is a window
 * onto the middle of it. Enlarging the wallet therefore reveals more sky rather
 * than stretching what was already there, and every star keeps the place it had
 * -- a starfield that slides around while the window is dragged looks like a
 * screensaver, not like a sky. The field is larger than 4K so that a maximised
 * window on a big screen still lands inside it.
 */
constexpr int SKY_WIDTH = 5120;
constexpr int SKY_HEIGHT = 2880;

//! One star per this many square pixels of sky.
constexpr qreal PIXELS_PER_STAR = 5200.0;

/**
 * How the sky is populated. The larger stars are the ones the eye reads as
 * bright, and they hold perfectly steady; only the small ones are allowed to
 * breathe, and very few of them. At a normal window size the shares below work
 * out at one or two visible twinklers at a time, which is the whole point: a
 * sky where many stars pulse reads as decoration rather than as a sky.
 */
constexpr qreal BRIGHTEST_SHARE = 0.015;
constexpr qreal LARGER_SHARE = 0.11;
constexpr qreal TWINKLING_SHARE_OF_SMALL = 0.010;

//! A breath is slow, and no two are the same length.
constexpr qreal TWINKLE_PERIOD_MIN = 4.5;
constexpr qreal TWINKLE_PERIOD_MAX = 9.5;
//! Swing around the star's own brightness. Small enough to notice only if watched.
constexpr qreal TWINKLE_AMPLITUDE = 0.38;

//! Twelve frames a second is more than a four-second breath can use.
constexpr int FRAME_INTERVAL_MS = 80;

//! A fixed seed, so the sky is the same one on every start.
constexpr uint32_t SKY_SEED = 0xDEFC0FFE;

constexpr qreal TAU = 6.283185307179586;

//! Below this radius a star counts as small, and may be picked to twinkle.
constexpr qreal SMALL_STAR_RADIUS = 0.95;

/** Paint one star, its brightness scaled by `scale` (1.0 = its own). */
void drawStar(QPainter& painter, const QPointF& at, qreal radius, const QColor& colour, qreal scale)
{
    // Only the few brightest carry a halo. Giving every star one turns the sky
    // into fog, and costs a radial gradient per star per frame.
    if (radius > 1.6) {
        QColor halo = colour;
        halo.setAlphaF(qBound(0.0, colour.alphaF() * 0.15 * scale, 1.0));
        QColor edge = halo;
        edge.setAlpha(0);
        QRadialGradient gradient(at, radius * 4.0);
        gradient.setColorAt(0.0, halo);
        gradient.setColorAt(1.0, edge);
        painter.setBrush(gradient);
        painter.drawEllipse(at, radius * 4.0, radius * 4.0);
    }

    QColor core = colour;
    core.setAlphaF(qBound(0.0, colour.alphaF() * scale, 1.0));
    painter.setBrush(core);
    painter.drawEllipse(at, radius, radius);
}
} // namespace

StarfieldWidget::StarfieldWidget(QWidget* parent) :
    QWidget(parent)
{
    buildSky();
    m_clock.start();
    m_timer = new QTimer(this);
    m_timer->setInterval(FRAME_INTERVAL_MS);
    connect(m_timer, &QTimer::timeout, this, &StarfieldWidget::onTick);
}

void StarfieldWidget::buildSky()
{
    std::mt19937 rng(SKY_SEED);
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);

    const int count = static_cast<int>((qreal(SKY_WIDTH) * SKY_HEIGHT) / PIXELS_PER_STAR);
    m_stars.clear();
    m_stars.reserve(count);
    m_twinklers.clear();

    for (int i = 0; i < count; ++i) {
        Star star;
        star.pos = QPointF(unit(rng) * SKY_WIDTH, unit(rng) * SKY_HEIGHT);
        star.amplitude = 0.0;
        star.period = 0.0;
        star.phase = 0.0;

        // Size and brightness travel together: a star that reads as bigger is
        // one that reads as brighter, which is why the large ones need no
        // movement to be noticed.
        const qreal kind = unit(rng);
        qreal alpha;
        if (kind < BRIGHTEST_SHARE) {
            star.radius = 1.70 + unit(rng) * 0.50;
            alpha = 0.85 + unit(rng) * 0.15;
        } else if (kind < BRIGHTEST_SHARE + LARGER_SHARE) {
            star.radius = 1.05 + unit(rng) * 0.45;
            alpha = 0.55 + unit(rng) * 0.30;
        } else {
            star.radius = 0.55 + unit(rng) * 0.35;
            alpha = 0.20 + unit(rng) * 0.40;
        }

        // Starlight is not one colour, and a field of identical white dots
        // reads as sensor noise rather than as a sky.
        const qreal tint = unit(rng);
        if (tint < 0.62) {
            star.colour = QColor(255, 255, 255);
        } else if (tint < 0.88) {
            star.colour = QColor(201, 219, 255);
        } else {
            star.colour = QColor(255, 235, 208);
        }
        star.colour.setAlphaF(alpha);

        if (star.radius < SMALL_STAR_RADIUS && unit(rng) < TWINKLING_SHARE_OF_SMALL) {
            star.amplitude = TWINKLE_AMPLITUDE;
            star.period = TWINKLE_PERIOD_MIN + unit(rng) * (TWINKLE_PERIOD_MAX - TWINKLE_PERIOD_MIN);
            star.phase = unit(rng) * TAU;
            m_twinklers.append(m_stars.size());
        }
        m_stars.append(star);
    }
}

QPointF StarfieldWidget::widgetPos(const Star& star) const
{
    const qreal dx = (SKY_WIDTH - width()) / 2.0;
    const qreal dy = (SKY_HEIGHT - height()) / 2.0;
    return QPointF(star.pos.x() - dx, star.pos.y() - dy);
}

QRect StarfieldWidget::starRect(const Star& star) const
{
    const QPointF at = widgetPos(star);
    // Wide enough for the halo of even the largest star, so a repaint of this
    // rectangle leaves nothing of the previous frame behind.
    const int pad = static_cast<int>(std::ceil(star.radius * 4.5)) + 2;
    return QRect(static_cast<int>(at.x()) - pad, static_cast<int>(at.y()) - pad, pad * 2 + 1, pad * 2 + 1);
}

void StarfieldWidget::redrawSteadySky()
{
    const qreal dpr = devicePixelRatioF();
    m_steady = QPixmap(static_cast<int>(std::ceil(width() * dpr)), static_cast<int>(std::ceil(height() * dpr)));
    m_steady.setDevicePixelRatio(dpr);
    m_steady.fill(Qt::transparent);

    QPainter painter(&m_steady);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    const QRectF view(0, 0, width(), height());
    for (const Star& star : m_stars) {
        if (star.amplitude > 0.0) continue; // these are drawn live, every frame
        const QPointF at = widgetPos(star);
        if (!view.contains(at)) continue;
        drawStar(painter, at, star.radius, star.colour, 1.0);
    }
}

void StarfieldWidget::paintEvent(QPaintEvent* event)
{
    // A stylesheet does not paint a QWidget subclass on its own, and the theme
    // still owns the colour behind the stars, so ask the style for it first.
    QStyleOption option;
    option.initFrom(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

    if (!m_skyVisible || width() <= 0 || height() <= 0) return;

    if (m_steady.isNull()) redrawSteadySky();
    painter.drawPixmap(0, 0, m_steady);

    if (m_twinklers.isEmpty()) return;
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    const qreal now = m_clock.elapsed() / 1000.0;
    const QRectF view(0, 0, width(), height());
    const QRect damaged = event->rect();
    for (const int index : m_twinklers) {
        const Star& star = m_stars.at(index);
        const QPointF at = widgetPos(star);
        if (!view.contains(at)) continue;
        if (!damaged.intersects(starRect(star))) continue;
        // With the animation off every star sits at its own brightness, so
        // switching it off settles the sky instead of freezing it mid-breath.
        const qreal scale = m_animated ? 1.0 + star.amplitude * std::sin(TAU * now / star.period + star.phase) : 1.0;
        drawStar(painter, at, star.radius, star.colour, scale);
    }
}

void StarfieldWidget::onTick()
{
    // Repaint the few small squares the twinkling stars occupy, never the whole
    // window: this runs for as long as the wallet is open.
    const QRect view = rect();
    for (const int index : m_twinklers) {
        const QRect box = starRect(m_stars.at(index));
        if (box.intersects(view)) update(box);
    }
}

bool StarfieldWidget::wantsAnimation() const
{
    if (!m_skyVisible || !m_animated || !isVisible() || m_twinklers.isEmpty()) return false;
    const QWidget* top = window();
    if (top == nullptr || top->isMinimized()) return false;
    // A wallet is left open for days. Nothing moves while it is not the window
    // being looked at.
    return top->isActiveWindow();
}

void StarfieldWidget::updateTimerState()
{
    if (m_timer == nullptr) return;
    if (wantsAnimation()) {
        if (!m_timer->isActive()) m_timer->start();
    } else if (m_timer->isActive()) {
        m_timer->stop();
    }
}

void StarfieldWidget::setSkyVisible(bool visible)
{
    if (m_skyVisible == visible) return;
    m_skyVisible = visible;
    m_steady = QPixmap();
    updateTimerState();
    update();
}

void StarfieldWidget::setAnimated(bool animated)
{
    if (m_animated == animated) return;
    m_animated = animated;
    updateTimerState();
    for (const int index : m_twinklers) update(starRect(m_stars.at(index)));
}

void StarfieldWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_steady = QPixmap(); // a different window shows a different piece of sky
}

void StarfieldWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (QWidget* top = window()) top->installEventFilter(this);
    updateTimerState();
}

void StarfieldWidget::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    updateTimerState();
}

bool StarfieldWidget::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::ActivationChange:
    case QEvent::WindowStateChange:
    case QEvent::Show:
    case QEvent::Hide:
        updateTimerState();
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}
