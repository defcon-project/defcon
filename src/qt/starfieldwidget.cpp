// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/starfieldwidget.h>

#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QRegion>
#include <QPaintEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <random>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#elif defined(Q_OS_MAC)
#include <mach/mach_time.h>
#elif defined(Q_OS_LINUX)
#include <time.h>
#endif

namespace {
constexpr qreal SIDEREAL_SECONDS = 86164.09;
constexpr qreal LUNATION_DAYS = 29.530588;
constexpr qreal MOON_SECONDS = 360.0 * 3600.0 / 14.49215;
constexpr qreal MOON_RADIUS = 25.0;
constexpr qreal MOON_EXTENT = MOON_RADIUS * 4.0 + 2.0;
constexpr int PHASE_STEPS = 4096;

//! One star per this many square pixels of sky.
constexpr qreal PIXELS_PER_STAR = 5200.0;

//! Conservative logical-pixel margin for star/halo footprints and rounding.
constexpr qreal STAR_CULL_MARGIN = 32.0;

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

void StarfieldWidget::buildSky()
{
    std::mt19937 rng(SKY_SEED);
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);

    m_sky_radius = std::max(qreal(6000.0), std::hypot(width() / 2.0, height() * 1.32) + 16.0);
    const int count = static_cast<int>((TAU / 2.0 * m_sky_radius * m_sky_radius) / PIXELS_PER_STAR);
    m_stars.clear();
    m_stars.reserve(count);
    m_twinklers.clear();

    for (int i = 0; i < count; ++i) {
        Star star;
        const qreal distance = m_sky_radius * std::sqrt(unit(rng));
        const qreal direction = TAU * unit(rng);
        star.pos = QPointF(distance * std::cos(direction), distance * std::sin(direction));
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
        }
        m_stars.append(star);
    }
    updateStarCandidates();
}

void StarfieldWidget::updateStarCandidates()
{
    // Rotation preserves pole distance. Keep the full deterministic catalogue,
    // but only visit stars that can reach the viewport, even after a long sleep.
    // Rebuild on resize rather than doing this work on every animation tick.
    const qreal radius = std::hypot(width() / 2.0, height() * 1.32) + STAR_CULL_MARGIN;
    const qreal radius_squared = radius * radius;
    m_candidates.clear();
    m_twinklers.clear();
    for (int i = 0; i < m_stars.size(); ++i) {
        const Star& star = m_stars.at(i);
        if (QPointF::dotProduct(star.pos, star.pos) > radius_squared) continue;
        m_candidates.append(i);
        if (star.amplitude > 0) m_twinklers.append(i);
    }
}

StarfieldWidget::StarfieldWidget(QWidget* parent) :
    QWidget(parent),
    m_epoch_utc_ms(QDateTime::currentMSecsSinceEpoch())
{
    // Classic themes do not allocate a star catalogue or image caches.
    m_clock_reference_ms = suspendAwareClockMilliseconds();
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &StarfieldWidget::onTick);
}

qint64 StarfieldWidget::suspendAwareClockMilliseconds()
{
    // Unlike Linux CLOCK_MONOTONIC / mach_absolute_time, these clocks include
    // suspend. They cannot jump when the user or NTP changes the wall clock.
#if defined(Q_OS_WIN)
    return static_cast<qint64>(GetTickCount64());
#elif defined(Q_OS_MAC)
    static const mach_timebase_info_data_t scale = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    return static_cast<qint64>(static_cast<long double>(mach_continuous_time()) * scale.numer / scale.denom / 1000000);
#elif defined(Q_OS_LINUX) && defined(CLOCK_BOOTTIME)
    timespec value{};
    if (clock_gettime(CLOCK_BOOTTIME, &value) == 0) {
        return static_cast<qint64>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
    }
#endif
    // Other platforms retain Qt's monotonic clock; suspend inclusion there is
    // platform dependent. The supported Windows/macOS/Linux paths above do not
    // compare elapsed time with a wall-clock delta to guess whether we slept.
    QElapsedTimer fallback;
    fallback.start();
    return fallback.msecsSinceReference();
}

qint64 StarfieldWidget::clockMilliseconds() const
{
    return suspendAwareClockMilliseconds() - m_clock_reference_ms;
}

qint64 StarfieldWidget::motionMilliseconds() const
{
    return (m_animated ? clockMilliseconds() : m_pause_started_ms) - m_paused_ms;
}

qint64 StarfieldWidget::phaseTimeMilliseconds() const
{
    return m_animated ? m_epoch_utc_ms + clockMilliseconds() : m_frozen_utc_ms;
}

qreal StarfieldWidget::rotationAngle(qint64 utc_ms)
{
    return TAU * std::fmod(utc_ms / 1000.0, SIDEREAL_SECONDS) / SIDEREAL_SECONDS;
}

qreal StarfieldWidget::lunarPhase(qint64 utc_ms)
{
    // Mean-cycle anchor: new Moon near 2000-01-06 18:14 UTC. Accuracy is
    // decorative; no location-dependent rise/set or illuminated limb is claimed.
    const qreal cycles = (utc_ms / 1000.0 - 947182440.0) / (86400.0 * LUNATION_DAYS);
    return cycles - std::floor(cycles);
}

QPointF StarfieldWidget::pole() const
{
    return QPointF(width() / 2.0, -height() * 0.32);
}

void StarfieldWidget::setRotationAngle(qreal angle)
{
    m_angle = angle;
    m_cos_angle = std::cos(angle);
    m_sin_angle = std::sin(angle);
}

QPointF StarfieldWidget::widgetPos(const Star& star) const
{
    return pole() + QPointF(star.pos.x() * m_cos_angle + star.pos.y() * m_sin_angle,
                           -star.pos.x() * m_sin_angle + star.pos.y() * m_cos_angle);
}

QRect StarfieldWidget::starRect(const Star& star) const
{
    const QPointF at = widgetPos(star);
    const qreal pad = std::ceil(star.radius * 4.5) + 2;
    return QRectF(at.x() - pad, at.y() - pad, pad * 2 + 1, pad * 2 + 1).toAlignedRect();
}

QPointF StarfieldWidget::moonPosition(qint64 motion_ms) const
{
    // A decorative composition, rather than an observer-specific ephemeris.
    // Keep the initial Moon near the upper right, with its own slower motion.
    const QPointF offset(width() * 0.78 - pole().x(), std::min(height() * 0.09, 70.0) - pole().y());
    const qreal angle = TAU * motion_ms / (1000.0 * MOON_SECONDS);
    return pole() + QPointF(offset.x() * std::cos(angle) + offset.y() * std::sin(angle),
                           -offset.x() * std::sin(angle) + offset.y() * std::cos(angle));
}

QRect StarfieldWidget::moonRect() const
{
    return QRectF(m_moon_position.x() - MOON_EXTENT, m_moon_position.y() - MOON_EXTENT,
                  MOON_EXTENT * 2, MOON_EXTENT * 2).toAlignedRect();
}

QImage StarfieldWidget::renderMoon(qreal phase, qreal dpr)
{
    const qreal radius = MOON_RADIUS * dpr;
    const int side = static_cast<int>(std::ceil(MOON_EXTENT * 2 * dpr));
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const qreal centre = side / 2.0;
    const qreal angle = phase * TAU;
    const qreal light_x = std::sin(angle);
    const qreal light_z = -std::cos(angle);
    const qreal illumination = (1 - std::cos(angle)) / 2;
    {
        QPainter painter(&image);
        QRadialGradient glow(QPointF(centre, centre), radius * 3.8);
        glow.setColorAt(0, QColor(231, 239, 255, static_cast<int>(45 * illumination)));
        glow.setColorAt(0.35, QColor(174, 201, 244, static_cast<int>(18 * illumination)));
        glow.setColorAt(1, Qt::transparent);
        painter.fillRect(image.rect(), glow);
    }
    struct Crater { qreal x, y, radius, depth; };
    const Crater patches[] = {
        {-0.25, -0.20, 0.35, 0.28}, {0.20, 0.15, 0.40, 0.27}, {-0.10, 0.35, 0.28, 0.20},
        {0.35, -0.30, 0.22, 0.25}, {-0.42, 0.38, 0.09, 0.18}, {0.42, 0.53, 0.08, 0.15},
        {0.12, -0.55, 0.13, 0.17}, {-0.36, -0.52, 0.12, 0.13}, {0.30, 0.62, 0.06, 0.22},
    };
    // Only this small image is regenerated, about every ten minutes or on DPR
    // changes. Translating the Moon never regenerates its surface.
    for (int y = std::max(0, static_cast<int>(centre - radius - 1));
         y < std::min(side, static_cast<int>(centre + radius + 1)); ++y) {
        for (int x = std::max(0, static_cast<int>(centre - radius - 1));
             x < std::min(side, static_cast<int>(centre + radius + 1)); ++x) {
            const qreal nx = (x + 0.5 - centre) / radius;
            const qreal ny = (y + 0.5 - centre) / radius;
            const qreal squared = nx * nx + ny * ny;
            if (squared > 1) continue;
            const qreal nz = std::sqrt(1 - squared);
            qreal surface = 0.88;
            for (const auto& crater : patches) {
                const qreal distance = std::hypot(nx - crater.x, ny - crater.y) / crater.radius;
                if (distance < 1) surface -= crater.depth * (1 - distance * distance);
            }
            surface += 0.025 * std::sin(nx * 63 + ny * 19) * std::cos(ny * 71 - nx * 23);
            const qreal light = std::max(qreal(0), nx * light_x + nz * light_z);
            const qreal brightness = 0.035 + std::pow(light, 0.38) * surface * 0.95;
            QColor colour;
            colour.setRgbF(qBound(0.0, brightness * 0.94, 1.0),
                           qBound(0.0, brightness * 0.97, 1.0), qBound(0.0, brightness, 1.0));
            colour.setAlphaF(qBound(0.0, (1 - std::sqrt(squared)) * radius, 1.0));
            image.setPixelColor(x, y, colour);
        }
    }
    image.setDevicePixelRatio(dpr);
    return image;
}

void StarfieldWidget::redrawSteadySky()
{
    const qreal dpr = devicePixelRatioF();
    const bool full = m_steady.isNull();
    if (full) {
        setRotationAngle(rotationAngle(m_epoch_utc_ms + motionMilliseconds()));
        m_moon_position = moonPosition(motionMilliseconds());
        m_steady = QPixmap(static_cast<int>(std::ceil(width() * dpr)), static_cast<int>(std::ceil(height() * dpr)));
        m_steady.setDevicePixelRatio(dpr);
        m_steady.fill(Qt::transparent);
    }
    QPainter painter(&m_steady);
    if (!full) {
        painter.setClipRegion(m_sky_dirty);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QRectF view = QRectF(rect()).adjusted(-10, -10, 10, 10);
    for (const int index : m_candidates) {
        const Star& star = m_stars.at(index);
        if (star.amplitude > 0) continue;
        if (!full && !m_sky_dirty.intersects(starRect(star))) continue;
        const QPointF at = widgetPos(star);
        if (view.contains(at)) drawStar(painter, at, star.radius, star.colour, 1.0);
    }
    m_sky_dirty = QRegion();
    updateTimerState();
}

void StarfieldWidget::paintEvent(QPaintEvent* event)
{
    // A stylesheet does not paint a QWidget subclass automatically. Ask the
    // active style for the background first, then add only the visible sky.
    QStyleOption option;
    option.initFrom(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
    if (!m_skyVisible || width() <= 0 || height() <= 0) return;
    if (!m_steady.isNull() && !qFuzzyCompare(m_steady.devicePixelRatioF(), devicePixelRatioF())) invalidateImages();
    if (m_steady.isNull() || !m_sky_dirty.isEmpty()) redrawSteadySky();
    painter.drawPixmap(0, 0, m_steady);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const qreal now = motionMilliseconds() / 1000.0;
    for (const int index : m_twinklers) {
        const Star& star = m_stars.at(index);
        if (!event->region().intersects(starRect(star))) continue;
        const qreal scale = m_animated ? 1 + star.amplitude * std::sin(TAU * now / star.period + star.phase) : 1;
        drawStar(painter, widgetPos(star), star.radius, star.colour, scale);
    }
    if (event->region().intersects(moonRect())) {
        if (m_moon.isNull()) {
            const qreal phase = lunarPhase(phaseTimeMilliseconds());
            m_moon_phase_bucket = static_cast<int>(phase * PHASE_STEPS);
            m_moon = QPixmap::fromImage(renderMoon(phase, devicePixelRatioF()));
        }
        const qreal side = m_moon.width() / m_moon.devicePixelRatioF();
        painter.drawPixmap(m_moon_position - QPointF(side / 2, side / 2), m_moon);
    }
}

void StarfieldWidget::onTick()
{
    if (!wantsAnimation()) {
        updateTimerState();
        return;
    }
    const qreal angle = rotationAngle(m_epoch_utc_ms + motionMilliseconds());
    const qreal max_radius = std::hypot(width() / 2.0, height() * 1.32) + 10;
    if (m_steady.isNull()) {
        update();
        return;
    }
    QRegion dirty;
    if (std::abs(std::remainder(angle - m_angle, TAU)) * max_radius * devicePixelRatioF() >= 0.25) {
        // Keep the large backing image. Clear and rebuild only the old/new
        // star footprints instead of reallocating a full high-DPI canvas.
        QVector<QRegion> footprints;
        const auto collect = [&] {
            for (const int index : m_candidates) {
                const Star& star = m_stars.at(index);
                const QRect box = starRect(star).intersected(rect());
                if (!box.isEmpty()) footprints.append(QRegion(box));
            }
        };
        collect();
        dirty += moonRect();
        setRotationAngle(angle);
        m_moon_position = moonPosition(motionMilliseconds());
        collect();
        // A balanced union avoids repeatedly copying a growing, fragmented
        // region when hundreds of old/new footprints are scattered over 4K.
        for (int span = 1; span < footprints.size(); span *= 2) {
            for (int i = 0; i + span < footprints.size(); i += span * 2) footprints[i] += footprints[i + span];
        }
        if (!footprints.isEmpty()) m_sky_dirty += footprints.front();
        dirty += m_sky_dirty;
        dirty += moonRect();
    }
    for (const int index : m_twinklers) dirty += starRect(m_stars.at(index)).intersected(rect());
    const QPointF next = moonPosition(motionMilliseconds());
    const int phase = static_cast<int>(lunarPhase(phaseTimeMilliseconds()) * PHASE_STEPS);
    if (std::hypot(next.x() - m_moon_position.x(), next.y() - m_moon_position.y()) * devicePixelRatioF() >= 0.25 ||
        phase != m_moon_phase_bucket) {
        dirty += moonRect();
        m_moon_position = next;
        if (phase != m_moon_phase_bucket) m_moon = QPixmap();
        dirty += moonRect();
    }
    dirty &= rect();
    if (!dirty.isEmpty()) update(dirty);
}

bool StarfieldWidget::wantsAnimation() const
{
    // A wallet may stay open for days: stop waking the GUI when it is hidden,
    // minimized or inactive, even if the decorative clock keeps advancing.
    const QWidget* top = window();
    return m_skyVisible && m_animated && isVisible() && top && !top->isMinimized() && top->isActiveWindow();
}

void StarfieldWidget::updateTimerState()
{
    if (!wantsAnimation()) {
        m_timer->stop();
        return;
    }
    bool visible_twinkle = false;
    for (const int index : m_twinklers) {
        if (starRect(m_stars.at(index)).intersects(rect())) {
            visible_twinkle = true;
            break;
        }
    }
    const int interval = visible_twinkle ? FRAME_INTERVAL_MS : 500;
    if (m_timer->interval() != interval) m_timer->setInterval(interval);
    if (!m_timer->isActive()) m_timer->start();
}

void StarfieldWidget::invalidateImages()
{
    m_steady = QPixmap();
    m_sky_dirty = QRegion();
    m_moon = QPixmap();
    m_moon_phase_bucket = -1;
}

void StarfieldWidget::setSkyVisible(bool visible)
{
    if (m_skyVisible == visible) return;
    m_skyVisible = visible;
    invalidateImages();
    if (visible) {
        buildSky();
    } else {
        m_stars.clear();
        m_stars.squeeze();
        m_candidates.clear();
        m_candidates.squeeze();
        m_twinklers.clear();
        m_twinklers.squeeze();
    }
    updateTimerState();
    update();
}

void StarfieldWidget::setAnimated(bool animated)
{
    if (m_animated == animated) return;
    if (!animated) {
        m_pause_started_ms = clockMilliseconds();
        m_frozen_utc_ms = m_epoch_utc_ms + m_pause_started_ms;
    } else {
        m_paused_ms += clockMilliseconds() - m_pause_started_ms;
        m_moon = QPixmap(); // Phase catches up to the current date, motion remains continuous.
    }
    m_animated = animated;
    updateTimerState();
    QRegion dirty(moonRect());
    for (const int index : m_twinklers) dirty += starRect(m_stars.at(index));
    update(dirty.intersected(rect()));
}

void StarfieldWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_skyVisible) {
        if (std::hypot(width() / 2.0, height() * 1.32) + 16 > m_sky_radius) {
            buildSky();
        } else {
            updateStarCandidates();
        }
    }
    invalidateImages();
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
    if (watched == window()) {
        switch (event->type()) {
        case QEvent::ActivationChange:
        case QEvent::WindowStateChange:
        case QEvent::Show:
        case QEvent::Hide:
            updateTimerState();
            break;
        case QEvent::ScreenChangeInternal:
            invalidateImages();
            update();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}
