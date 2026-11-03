// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/starfieldwidget.h>

#include <qt/skycatalogue.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QImage>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QRegion>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOption>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#elif defined(Q_OS_MAC)
#include <mach/mach_time.h>
#elif defined(Q_OS_LINUX)
#include <time.h>
#endif

namespace SkyTiers {
namespace {
//! Whole coins; the order is the roadmap's accepted ladder.
constexpr std::array<CAmount, COUNT> THRESHOLDS{0, 100000, 200000, 500000, 1000000, 2000000, 4000000, 7000000, 11000000, 17000000, 25000000};
} // namespace

CAmount threshold(int tier)
{
    return THRESHOLDS.at(std::clamp(tier, 0, COUNT - 1)) * COIN;
}

int tierForBalance(CAmount balance, int current)
{
    current = std::clamp(current, 0, COUNT - 1);
    int tier = 0;
    for (int i = 1; i < COUNT; ++i) {
        if (balance >= threshold(i)) tier = i;
    }
    // Hysteresis: a balance that dips just under the threshold it rests on
    // keeps its level; only a real drop takes it down. Integer arithmetic,
    // within range for any coin amount.
    if (tier < current && balance >= 0 && balance * 100 >= threshold(current) * DOWN_PERCENT) return current;
    return tier;
}

QString name(int tier)
{
    switch (std::clamp(tier, 0, COUNT - 1)) {
    case 0: return QCoreApplication::translate("SkyTiers", "Starfield");
    case 1: return QCoreApplication::translate("SkyTiers", "Night Sky");
    case 2: return QCoreApplication::translate("SkyTiers", "Constellations");
    case 3: return QCoreApplication::translate("SkyTiers", "Milky Way");
    case 4: return QCoreApplication::translate("SkyTiers", "Saturn");
    case 5: return QCoreApplication::translate("SkyTiers", "Jupiter");
    case 6: return QCoreApplication::translate("SkyTiers", "Ice Giant");
    case 7: return QCoreApplication::translate("SkyTiers", "Nebula");
    case 8: return QCoreApplication::translate("SkyTiers", "Galaxy");
    case 9: return QCoreApplication::translate("SkyTiers", "Deep Space");
    default: return QCoreApplication::translate("SkyTiers", "DeFCoN Universe");
    }
}
} // namespace SkyTiers

namespace {
constexpr qreal TAU = 6.283185307179586;
constexpr qreal DEG = TAU / 360.0;
constexpr qreal LUNATION_DAYS = 29.530588;
constexpr qreal MOON_RADIUS = 25.0;
constexpr qreal MOON_EXTENT = MOON_RADIUS * 4.0 + 2.0;
constexpr int PHASE_STEPS = 4096;

//! The sky is a window on a fixed plane; resizing reveals more of the same stars.
constexpr qreal PLANE_WIDTH = 7680.0;
constexpr qreal PLANE_HEIGHT = 4320.0;
//! One star per this many square pixels of plane, before the level thins it.
constexpr qreal PIXELS_PER_STAR = 5200.0;
//! Of the plane's stars, the share the levels keep: the plain field is sparse.
constexpr qreal FIELD_SHARE = 0.8;
constexpr qreal SPARSE_SHARE = 0.55;
//! Conservative logical-pixel margin for star/halo footprints and rounding.
constexpr qreal STAR_CULL_MARGIN = 32.0;

/**
 * How the plane is populated. The larger stars are the ones the eye reads as
 * bright, and they hold perfectly steady; only the small ones are allowed to
 * breathe, and very few of them. At a normal window size the shares below work
 * out at one or two visible twinklers at a time, which is the whole point: a
 * sky where many stars pulse reads as decoration rather than as a sky.
 */
constexpr qreal BRIGHTEST_SHARE = 0.015;
constexpr qreal LARGER_SHARE = 0.11;
constexpr qreal TWINKLING_SHARE_OF_SMALL = 0.010;
constexpr qreal TWINKLE_PERIOD_MIN = 4.5;
constexpr qreal TWINKLE_PERIOD_MAX = 9.5;
constexpr qreal TWINKLE_AMPLITUDE = 0.38;
constexpr qreal SMALL_STAR_RADIUS = 0.95;

//! Twelve frames a second is more than a four-second breath can use; without
//! a visible twinkler two a second carry the comet, the shooting star's start
//! and the ten-minute check.
constexpr int FRAME_INTERVAL_MS = 80;
constexpr int IDLE_INTERVAL_MS = 500;
//! The real sky is re-projected this often; a quarter degree of drift is not seen.
constexpr qint64 SKY_BUCKET_MS = 600000;

//! The observer the real sky is drawn for: central Europe. The longitude is a
//! guess from the time zone, so no location is read.
constexpr qreal LATITUDE = 47.0;
//! Zenith distance at the top and bottom edge of the widget.
constexpr qreal EDGE_ZENITH_DISTANCE = 50.0;

//! A shooting star crosses in this long, then at most one a minute follows.
constexpr qint64 SHOOTING_MS = 600;
constexpr qint64 SHOOTING_GAP_MIN_MS = 20000;
constexpr qint64 SHOOTING_GAP_MAX_MS = 60000;
constexpr qreal SHOOTING_LENGTH = 170.0;
constexpr qreal SHOOTING_TAIL = 60.0;
//! The comet of level 6 crosses the header band in four minutes, then returns.
constexpr qint64 COMET_PERIOD_MS = 240000;
constexpr qreal COMET_TAIL = 150.0;

//! Fixed seeds, so the sky is the same one on every start.
constexpr uint32_t SKY_SEED = 0xDEFC0FFE;
constexpr uint32_t MILKY_WAY_SEED = 0x4D494C4B;
constexpr uint32_t GALAXY_SEED = 0xA11;
constexpr uint32_t SMALL_GALAXY_SEED = 0xB22;
constexpr uint32_t DEEP_SPACE_SEED = 0xDEE9;
constexpr uint32_t LOGO_SEED = 77;

//! Galactic pole and the galactic longitude of the celestial pole (J2000).
constexpr qreal GALACTIC_POLE_RA = 192.85948 * DEG;
constexpr qreal GALACTIC_POLE_DEC = 27.12825 * DEG;
constexpr qreal GALACTIC_NCP = 122.93192 * DEG;

enum Tier { Starfield = 0, NightSky, Constellations, MilkyWay, Saturn, Jupiter, IceGiant, Nebula, Galaxy, DeepSpace, Universe };

QColor tintColour(SkyCatalogue::Tint tint)
{
    switch (tint) {
    case SkyCatalogue::Tint::Blue: return QColor(201, 219, 255);
    case SkyCatalogue::Tint::Orange: return QColor(255, 214, 170);
    case SkyCatalogue::Tint::Yellow: return QColor(255, 240, 210);
    default: return QColor(255, 255, 255);
    }
}

QColor withAlpha(QColor colour, qreal alpha)
{
    colour.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return colour;
}

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

void fillGlow(QPainter& painter, const QPointF& at, qreal radius, const QColor& centre, const QColor& edge)
{
    QRadialGradient gradient(at, radius);
    gradient.setColorAt(0.0, centre);
    gradient.setColorAt(1.0, edge);
    painter.setBrush(gradient);
    painter.drawEllipse(at, radius, radius);
}

qreal gaussian(std::mt19937& rng)
{
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);
    return std::sqrt(-2 * std::log(1 - unit(rng))) * std::cos(TAU * unit(rng));
}

/** Galactic longitude/latitude (degrees) to J2000 right ascension (hours) and declination (degrees). */
std::pair<qreal, qreal> galacticToEquatorial(qreal l_degrees, qreal b_degrees)
{
    const qreal l = l_degrees * DEG, b = b_degrees * DEG;
    const qreal sin_dec = std::sin(b) * std::sin(GALACTIC_POLE_DEC) + std::cos(b) * std::cos(GALACTIC_POLE_DEC) * std::cos(GALACTIC_NCP - l);
    const qreal y = std::cos(b) * std::sin(GALACTIC_NCP - l);
    const qreal x = std::sin(b) * std::cos(GALACTIC_POLE_DEC) - std::cos(b) * std::sin(GALACTIC_POLE_DEC) * std::cos(GALACTIC_NCP - l);
    qreal ra = std::atan2(y, x) + GALACTIC_POLE_RA;
    ra = std::fmod(std::fmod(ra, TAU) + TAU, TAU);
    return {ra / (15 * DEG), std::asin(std::clamp(sin_dec, -1.0, 1.0)) / DEG};
}

/** Low-resolution value noise, three octaves, computed once for the nebula. */
QVector<float> valueNoise(uint32_t seed, int width, int height)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto grid = [&](int n) {
        QVector<float> values(n * n);
        for (float& value : values) value = unit(rng);
        return values;
    };
    const QVector<float> g1 = grid(6), g2 = grid(12), g3 = grid(24);
    const auto sample = [](const QVector<float>& g, int n, float u, float v) {
        const float x = u * (n - 1), y = v * (n - 1);
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(n - 1, x0 + 1), y1 = std::min(n - 1, y0 + 1);
        const float fx = x - x0, fy = y - y0;
        const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
        const float a = g[y0 * n + x0], b = g[y0 * n + x1], c = g[y1 * n + x0], d = g[y1 * n + x1];
        return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy;
    };
    QVector<float> out(width * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / (width - 1), v = static_cast<float>(y) / (height - 1);
            out[y * width + x] = sample(g1, 6, u, v) * 0.55f + sample(g2, 12, u, v) * 0.3f + sample(g3, 24, u, v) * 0.15f;
        }
    }
    return out;
}

QImage renderNebula()
{
    constexpr int width = 64, height = 36;
    const QVector<float> n1 = valueNoise(11, width, height), n2 = valueNoise(23, width, height), n3 = valueNoise(37, width, height);
    QImage image(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            const float ex = (static_cast<float>(x) / width - 0.5f) * 2, ey = (static_cast<float>(y) / height - 0.5f) * 2;
            const float fall = std::max(0.0f, 1 - (ex * ex * 0.9f + ey * ey * 1.1f));
            const float alpha = std::max(0.0f, n1[i] - 0.3f) * 2.6f * fall;
            const float teal = std::max(0.0f, n2[i] - 0.3f), magenta = std::max(0.0f, n3[i] - 0.35f);
            const float warm = std::max(0.0f, n1[i] * n3[i] - 0.25f);
            image.setPixelColor(x, y, QColor(std::min(255, static_cast<int>(40 + teal * 40 + magenta * 170 + warm * 210)),
                                             std::min(255, static_cast<int>(70 + teal * 150 + magenta * 30 + warm * 80)),
                                             std::min(255, static_cast<int>(120 + teal * 140 + magenta * 160 + warm * 20)),
                                             std::min(255, static_cast<int>(alpha * 255))));
        }
    }
    return image;
}

void paintSaturn(QPainter& painter, const QPointF& c)
{
    const qreal R = 20, tilt = -0.32 / DEG, ring_x = 46, ring_y = 13;
    const auto ring = [&](qreal r0, qreal r1, const QColor& colour, qreal alpha) {
        painter.save();
        painter.translate(c);
        painter.rotate(tilt);
        painter.scale(1, ring_y / ring_x);
        QRadialGradient gradient(QPointF(0, 0), r1);
        gradient.setColorAt(r0 / r1, withAlpha(colour, alpha));
        gradient.setColorAt(1.0, withAlpha(colour, alpha * 0.5));
        QPainterPath path;
        path.addEllipse(QPointF(0, 0), r1, r1);
        path.addEllipse(QPointF(0, 0), r0, r0);
        path.setFillRule(Qt::OddEvenFill);
        painter.fillPath(path, gradient);
        painter.restore();
    };
    const auto rings = [&] {
        ring(23, 30, QColor(214, 200, 170), 0.35);
        ring(30, 40, QColor(232, 220, 190), 0.7);
        ring(41.5, 46, QColor(226, 214, 186), 0.5);
    };
    const QRectF back(c.x() - 60, c.y() - 60, 120, 60), front(c.x() - 60, c.y(), 120, 60);
    painter.save();
    painter.setClipRect(back);
    rings();
    // The planet's shadow on the far side of the rings.
    painter.setBrush(QColor(4, 7, 13, 191));
    painter.drawEllipse(c + QPointF(3, -4), R + 2, R + 6);
    painter.restore();

    painter.save();
    QPainterPath disk;
    disk.addEllipse(c, R, R);
    painter.setClipPath(disk);
    QLinearGradient bands(0, c.y() - R, 0, c.y() + R);
    bands.setColorAt(0.0, QColor("#cfc0a0"));
    bands.setColorAt(0.18, QColor("#e7d8b4"));
    bands.setColorAt(0.3, QColor("#d5bf8f"));
    bands.setColorAt(0.42, QColor("#efe2c2"));
    bands.setColorAt(0.55, QColor("#d9c49a"));
    bands.setColorAt(0.68, QColor("#e6d6b3"));
    bands.setColorAt(0.8, QColor("#c8b48b"));
    bands.setColorAt(1.0, QColor("#b39f78"));
    painter.fillRect(QRectF(c.x() - R, c.y() - R, R * 2, R * 2), bands);
    QRadialGradient limb(c, R, c + QPointF(-R * 0.35, -R * 0.3));
    limb.setColorAt(0.0, QColor(255, 250, 235, 31));
    limb.setColorAt(0.75, QColor(0, 0, 0, 0));
    limb.setColorAt(1.0, QColor(0, 0, 0, 140));
    painter.fillRect(QRectF(c.x() - R, c.y() - R, R * 2, R * 2), limb);
    // The rings' shadow across the globe.
    painter.translate(c);
    painter.rotate(tilt);
    painter.setBrush(QColor(40, 30, 15, 115));
    painter.drawEllipse(QPointF(0, -R * 0.52), 52, 2.6);
    painter.restore();

    painter.save();
    painter.setClipRect(front);
    rings();
    painter.restore();
}

void paintJupiter(QPainter& painter, const QPointF& c)
{
    const qreal R = 45;
    painter.save();
    QPainterPath disk;
    disk.addEllipse(c, R, R);
    painter.setClipPath(disk);
    const std::array<std::pair<const char*, qreal>, 11> stripes{{{"#d8c6ad", 10}, {"#b9977a", 7}, {"#ead9bf", 8}, {"#a98367", 9}, {"#efe3cf", 9}, {"#c4a081", 6},
                                                                 {"#e3d1b6", 10}, {"#b08a6b", 8}, {"#dcc9ad", 9}, {"#c9ad8c", 8}, {"#d6c3a8", 6}}};
    qreal y = c.y() - R;
    for (const auto& [colour, height] : stripes) {
        QPainterPath band;
        band.moveTo(c.x() - R, y);
        for (qreal x = -R; x <= R; x += 4) band.lineTo(c.x() + x, y + std::sin(x / 9 + y) * 0.8);
        band.lineTo(c.x() + R, y + height + 2);
        band.lineTo(c.x() - R, y + height + 2);
        band.closeSubpath();
        painter.fillPath(band, QColor(colour));
        y += height;
    }
    painter.setBrush(QColor("#c75d45"));
    painter.drawEllipse(c + QPointF(12, 12), 9, 5);
    painter.setBrush(QColor(255, 220, 200, 89));
    painter.drawEllipse(c + QPointF(12, 12), 5, 2.5);
    QRadialGradient limb(c, R, c + QPointF(-R * 0.3, -R * 0.3));
    limb.setColorAt(0.0, QColor(255, 250, 240, 26));
    limb.setColorAt(0.7, QColor(0, 0, 0, 0));
    limb.setColorAt(1.0, QColor(0, 0, 0, 153));
    painter.fillRect(QRectF(c.x() - R, c.y() - R, R * 2, R * 2), limb);
    // Kept under the brightness of the page's own text.
    painter.fillRect(QRectF(c.x() - R, c.y() - R, R * 2, R * 2), QColor(10, 8, 20, 36));
    painter.restore();
    drawStar(painter, c + QPointF(-72, 6), 1.4, withAlpha(QColor(255, 255, 255), 0.95), 1.0);
    drawStar(painter, c + QPointF(66, -9), 1.1, withAlpha(QColor(255, 240, 210), 0.85), 1.0);
}

void paintIceGiant(QPainter& painter, const QPointF& c)
{
    const qreal R = 30, tilt = 0.9 / DEG;
    const QRectF ring(-50, -9, 100, 18);
    painter.save();
    painter.translate(c);
    painter.rotate(tilt);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(180, 210, 255, 89), 1.2));
    painter.drawEllipse(ring);
    painter.restore();

    QRadialGradient body(c, R, c + QPointF(-R * 0.35, -R * 0.35));
    body.setColorAt(0.0, QColor("#6aa8ff"));
    body.setColorAt(0.5, QColor("#2f6fd6"));
    body.setColorAt(1.0, QColor("#0f2d6a"));
    painter.setPen(Qt::NoPen);
    painter.setBrush(body);
    painter.drawEllipse(c, R, R);
    painter.save();
    QPainterPath disk;
    disk.addEllipse(c, R, R);
    painter.setClipPath(disk);
    for (int i = -2; i <= 2; ++i) painter.fillRect(QRectF(c.x() - R, c.y() + i * 9 - 1, R * 2, 2), QColor(255, 255, 255, 20));
    painter.restore();

    // The near half of the ring crosses the globe.
    painter.save();
    QPainterPath front;
    front.addEllipse(c, R + 1, R + 1);
    painter.setClipPath(front);
    painter.translate(c);
    painter.rotate(tilt);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(200, 225, 255, 128), 1.2));
    painter.drawArc(ring, 0, -180 * 16);
    painter.restore();
    painter.setPen(Qt::NoPen);
}

void paintGalaxy(QPainter& painter, const QPointF& c, qreal scale, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);
    painter.save();
    painter.translate(c);
    painter.rotate(-0.42 / DEG);
    painter.scale(1, 0.46);
    fillGlow(painter, QPointF(0, 0), 110 * scale, QColor(200, 210, 255, 41), QColor(200, 210, 255, 0));
    QRadialGradient core(QPointF(0, 0), 44 * scale);
    core.setColorAt(0.0, QColor(255, 245, 220, 242));
    core.setColorAt(0.25, QColor(255, 235, 200, 140));
    core.setColorAt(1.0, QColor(230, 220, 255, 0));
    painter.setBrush(core);
    painter.drawEllipse(QPointF(0, 0), 44 * scale, 44 * scale);
    for (int arm = 0; arm < 2; ++arm) {
        for (int i = 0; i < 1400; ++i) {
            const qreal t = unit(rng) * 3.3 * (TAU / 2);
            const qreal r = 5 * std::exp(0.21 * t) * scale;
            if (r > 108 * scale) continue;
            const qreal angle = t + arm * (TAU / 2) + gaussian(rng) * 0.11;
            const qreal rr = r + gaussian(rng) * (4 + r * 0.08);
            const qreal k = unit(rng);
            const QColor colour = k < 0.08 ? QColor(255, 180, 200, static_cast<int>((0.5 + unit(rng) * 0.4) * 255))
                                : k < 0.5  ? QColor(200, 215, 255, static_cast<int>((0.25 + unit(rng) * 0.45) * 255))
                                           : QColor(235, 235, 250, static_cast<int>((0.15 + unit(rng) * 0.35) * 255));
            const qreal size = k < 0.08 ? 1.6 : 1.0;
            painter.fillRect(QRectF(rr * std::cos(angle), rr * std::sin(angle), size, size), colour);
        }
    }
    painter.restore();
}

void paintLogoConstellation(QPainter& painter, const QPointF& c, qreal size)
{
    const qreal s = size / 1024.0;
    std::mt19937 rng(LOGO_SEED);
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);
    const auto outline = [&](const SkyCatalogue::Point* points, size_t count) {
        QVector<QPointF> nodes;
        for (size_t i = 0; i < count; ++i) nodes.append(c + QPointF((points[i].x - 512) * s, (points[i].y - 512) * s));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(18, 151, 255, 115), 1.0));
        painter.drawPolygon(nodes.constData(), nodes.size());
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < nodes.size(); ++i) {
            const bool big = i % 3 == 0;
            const qreal radius = big ? 2.0 + unit(rng) * 0.6 : 1.1 + unit(rng) * 0.4;
            fillGlow(painter, nodes[i], radius * 5, big ? QColor(0, 255, 251, 89) : QColor(0, 157, 247, 64), QColor(0, 157, 247, 0));
            painter.setBrush(big ? QColor("#dffbff") : QColor("#bfe4ff"));
            painter.drawEllipse(nodes[i], radius, radius);
        }
    };
    outline(SkyCatalogue::LOGO_CRESCENT, sizeof(SkyCatalogue::LOGO_CRESCENT) / sizeof(SkyCatalogue::LOGO_CRESCENT[0]));
    outline(SkyCatalogue::LOGO_WING, sizeof(SkyCatalogue::LOGO_WING) / sizeof(SkyCatalogue::LOGO_WING[0]));
}

void paintDeepSpace(QPainter& painter, const QRectF& area, const QPointF& cluster)
{
    std::mt19937 rng(DEEP_SPACE_SEED);
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);
    const auto one = [&](const QPointF& at, qreal size, qreal alpha, qreal tilt, bool warm) {
        painter.save();
        painter.translate(at);
        painter.rotate(tilt / DEG);
        painter.scale(1, 0.3 + unit(rng) * 0.6);
        QRadialGradient gradient(QPointF(0, 0), size);
        gradient.setColorAt(0.0, warm ? withAlpha(QColor(255, 236, 210), alpha) : withAlpha(QColor(205, 215, 255), alpha));
        gradient.setColorAt(0.4, warm ? withAlpha(QColor(240, 215, 190), alpha * 0.5) : withAlpha(QColor(190, 200, 245), alpha * 0.5));
        gradient.setColorAt(1.0, QColor(200, 200, 240, 0));
        painter.setBrush(gradient);
        painter.drawEllipse(QPointF(0, 0), size, size);
        painter.restore();
    };
    for (int i = 0; i < 90; ++i) {
        one(QPointF(area.left() + unit(rng) * area.width(), area.top() + unit(rng) * area.height()), 3 + unit(rng) * 9, 0.35 + unit(rng) * 0.4, unit(rng) * (TAU / 2), unit(rng) < 0.45);
    }
    for (int i = 0; i < 22; ++i) {
        const qreal r = std::abs(gaussian(rng)) * 55, a = unit(rng) * TAU;
        one(cluster + QPointF(r * std::cos(a), r * std::sin(a) * 0.6), 2.5 + unit(rng) * 6, 0.3 + unit(rng) * 0.35, unit(rng) * (TAU / 2), unit(rng) < 0.6);
    }
    painter.save();
    painter.translate(cluster);
    painter.scale(1, 70.0 / 120.0);
    fillGlow(painter, QPointF(0, 0), 120, QColor(220, 210, 235, 18), QColor(220, 210, 235, 0));
    painter.restore();
}
} // namespace

StarfieldWidget::Projection::Projection(const QSize& size, qreal lst_hours) :
    centre_x(size.width() / 2.0), centre_y(size.height() / 2.0),
    radius((size.height() / 2.0) / (2 * std::tan(EDGE_ZENITH_DISTANCE / 2 * DEG))),
    sidereal_hours(lst_hours), sin_lat(std::sin(LATITUDE * DEG)), cos_lat(std::cos(LATITUDE * DEG))
{
}

bool StarfieldWidget::Projection::place(qreal ra_hours, qreal dec_degrees, QPointF& at, qreal& altitude) const
{
    const qreal hour_angle = (sidereal_hours - ra_hours) * 15 * DEG, dec = dec_degrees * DEG;
    const qreal sin_alt = std::sin(dec) * sin_lat + std::cos(dec) * cos_lat * std::cos(hour_angle);
    if (sin_alt <= 0.05) return false;
    altitude = std::asin(std::clamp(sin_alt, -1.0, 1.0));
    const qreal cos_alt = std::cos(altitude);
    const qreal azimuth = std::atan2(-std::sin(hour_angle) * std::cos(dec) / cos_alt, (std::sin(dec) - sin_alt * sin_lat) / (cos_alt * cos_lat));
    const qreal r = 2 * radius * std::tan((TAU / 4 - altitude) / 2);
    at = QPointF(centre_x - r * std::sin(azimuth), centre_y - r * std::cos(azimuth));
    return true;
}

void StarfieldWidget::buildField()
{
    std::mt19937 rng(SKY_SEED);
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);

    const int count = static_cast<int>(PLANE_WIDTH * PLANE_HEIGHT / PIXELS_PER_STAR * FIELD_SHARE);
    m_stars.clear();
    m_stars.reserve(count);
    for (int i = 0; i < count; ++i) {
        Star star;
        star.pos = QPointF(unit(rng) * PLANE_WIDTH, unit(rng) * PLANE_HEIGHT);
        star.rank = unit(rng);
        star.amplitude = 0.0;
        star.period = 0.0;
        star.phase = 0.0;

        // Size and brightness travel together: a star that reads as bigger is
        // one that reads as brighter, which is why the large ones need no
        // movement to be noticed.
        const qreal kind = unit(rng);
        qreal alpha;
        star.bright = kind < BRIGHTEST_SHARE;
        if (star.bright) {
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
    updateVisibleStars();
}

void StarfieldWidget::updateVisibleStars()
{
    // The plane never moves, so the stars in frame change only on resize; the
    // paint and tick paths then visit only these.
    const QRectF frame = QRectF(rect()).adjusted(-STAR_CULL_MARGIN, -STAR_CULL_MARGIN, STAR_CULL_MARGIN, STAR_CULL_MARGIN);
    m_visible.clear();
    m_twinklers.clear();
    for (int i = 0; i < m_stars.size(); ++i) {
        const Star& star = m_stars.at(i);
        if (!frame.contains(star.pos)) continue;
        m_visible.append(i);
        if (star.amplitude > 0) m_twinklers.append(i);
    }
}

StarfieldWidget::StarfieldWidget(QWidget* parent) :
    QWidget(parent)
{
    // Classic themes do not allocate a star catalogue or image caches.
    m_clock_reference_ms = suspendAwareClockMilliseconds();
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &StarfieldWidget::onTick);
    m_refresh = new QTimer(this);
    m_refresh->setSingleShot(true);
    connect(m_refresh, &QTimer::timeout, this, &StarfieldWidget::onRefresh);
    m_next_shooting_ms = SHOOTING_GAP_MIN_MS;
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

qint64 StarfieldWidget::utcMilliseconds() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 StarfieldWidget::phaseTimeMilliseconds() const
{
    // The real sky and the Moon follow the system clock, so a corrected clock
    // corrects them at the next tick's ten-minute check; motion keeps the
    // suspend-aware clock above and cannot jump with it.
    return m_animated ? utcMilliseconds() : m_frozen_utc_ms;
}

qint64 StarfieldWidget::skyBucket() const
{
    return phaseTimeMilliseconds() / SKY_BUCKET_MS;
}

qreal StarfieldWidget::lunarPhase(qint64 utc_ms)
{
    // Mean-cycle anchor: new Moon near 2000-01-06 18:14 UTC. Accuracy is
    // decorative; no location-dependent rise/set or illuminated limb is claimed.
    const qreal cycles = (utc_ms / 1000.0 - 947182440.0) / (86400.0 * LUNATION_DAYS);
    return cycles - std::floor(cycles);
}

qreal StarfieldWidget::siderealHours(qint64 utc_ms, qreal longitude_degrees)
{
    // Greenwich mean sidereal time from the J2000 epoch, then the local offset.
    const qreal days = utc_ms / 86400000.0 + 2440587.5 - 2451545.0;
    const qreal greenwich = std::fmod(std::fmod(18.697374558 + 24.06570982441908 * days, 24.0) + 24.0, 24.0);
    return std::fmod(std::fmod(greenwich + longitude_degrees / 15.0, 24.0) + 24.0, 24.0);
}

qreal StarfieldWidget::longitudeGuess(qint64 utc_ms)
{
    // Fifteen degrees per hour of the zone's standard offset: within an hour
    // of sidereal time for most of a time zone, and no location is read.
    return QTimeZone::systemTimeZone().standardTimeOffset(QDateTime::fromMSecsSinceEpoch(utc_ms, Qt::UTC)) / 3600.0 * 15.0;
}

QPointF StarfieldWidget::moonPosition() const
{
    // A decorative composition, rather than an observer-specific ephemeris:
    // the Moon sits in the header, near the upper right.
    return QPointF(width() * 0.78, std::min(height() * 0.09, 70.0));
}

QRect StarfieldWidget::moonRect() const
{
    return QRectF(m_moon_position.x() - MOON_EXTENT, m_moon_position.y() - MOON_EXTENT,
                  MOON_EXTENT * 2, MOON_EXTENT * 2).toAlignedRect();
}

QPointF StarfieldWidget::heroPosition() const
{
    // The level's own scene takes the Moon's place; from Saturn up the Moon
    // is not drawn (user decision, 2026-11-03).
    return QPointF(width() * 0.78, 58.0);
}

QRect StarfieldWidget::starRect(const Star& star) const
{
    const qreal pad = std::ceil(star.radius * 4.5) + 2;
    return QRectF(star.pos.x() - pad, star.pos.y() - pad, pad * 2 + 1, pad * 2 + 1).toAlignedRect();
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
    // changes.
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

bool StarfieldWidget::hasRealSky() const
{
    return m_tier >= NightSky;
}

bool StarfieldWidget::hasShootingStars() const
{
    // Deep space looks past the atmosphere.
    return hasRealSky() && m_tier != DeepSpace;
}

void StarfieldWidget::paintField(QPainter& painter) const
{
    const bool sparse = m_tier == Starfield;
    const qreal dim = sparse ? 0.6 : (m_tier == DeepSpace ? 0.55 : 0.75);
    for (const int index : m_visible) {
        const Star& star = m_stars.at(index);
        if (star.amplitude > 0) continue;
        if (sparse ? star.rank >= SPARSE_SHARE / FIELD_SHARE : star.bright) continue;
        drawStar(painter, star.pos, star.radius, star.colour, dim);
    }
}

void StarfieldWidget::paintCatalogue(QPainter& painter, const Projection& projection, qreal dim) const
{
    for (const SkyCatalogue::Star& star : SkyCatalogue::STARS) {
        QPointF at;
        qreal altitude;
        if (!projection.place(star.ra_hours, star.dec_degrees, at, altitude)) continue;
        // Extinction near the horizon.
        const qreal fade = std::min(1.0, (altitude - 3 * DEG) / (12 * DEG));
        const qreal radius = std::max(0.65, 3.0 - 0.5 * star.magnitude);
        const qreal alpha = std::clamp(1.12 - 0.16 * star.magnitude, 0.38, 1.0) * fade * dim;
        drawStar(painter, at, radius, withAlpha(tintColour(star.tint), alpha), 1.0);
    }
}

void StarfieldWidget::paintLines(QPainter& painter, const Projection& projection, qreal alpha) const
{
    painter.save();
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(withAlpha(QColor(120, 160, 210), alpha), 0.8));
    for (const SkyCatalogue::Line& line : SkyCatalogue::LINES) {
        const SkyCatalogue::Star& from = SkyCatalogue::STARS[line.from];
        const SkyCatalogue::Star& to = SkyCatalogue::STARS[line.to];
        QPointF a, b;
        qreal altitude;
        if (!projection.place(from.ra_hours, from.dec_degrees, a, altitude)) continue;
        if (!projection.place(to.ra_hours, to.dec_degrees, b, altitude)) continue;
        painter.drawLine(a, b);
    }
    painter.restore();
}

void StarfieldWidget::paintMilkyWay(QPainter& painter, const Projection& projection, qreal strength) const
{
    std::mt19937 rng(MILKY_WAY_SEED);
    std::uniform_real_distribution<qreal> unit(0.0, 1.0);
    const auto weight = [](qreal l) {
        const qreal wrapped = std::fmod(l + 180.0, 360.0) - 180.0;
        return 0.3 + 0.7 * std::exp(-(wrapped / 80) * (wrapped / 80));
    };
    const auto rift = [](qreal l, qreal b) { return l > 15 && l < 60 && b > -2 && b < 7; };
    const QRectF frame = QRectF(rect()).adjusted(-80, -80, 80, 80);
    // A soft band first, then the grain of distant stars along it.
    for (int i = 0; i < 700; ++i) {
        const qreal l = unit(rng) * 360, b = gaussian(rng) * 7;
        const qreal rad = 26 + unit(rng) * 40, alpha = 0.05 * weight(l) * strength;
        const bool dark = rift(l, b) && unit(rng) < 0.7;
        if (dark) continue;
        const auto [ra, dec] = galacticToEquatorial(l, b);
        QPointF at;
        qreal altitude;
        if (!projection.place(ra, dec, at, altitude) || !frame.contains(at)) continue;
        fillGlow(painter, at, rad, withAlpha(QColor(190, 205, 235), alpha), QColor(190, 205, 235, 0));
    }
    for (int i = 0; i < 9000; ++i) {
        const qreal l = unit(rng) * 360, b = gaussian(rng) * (unit(rng) < 0.75 ? 4.5 : 11);
        const qreal w = weight(l);
        const qreal keep = unit(rng);
        const qreal grain = 0.2 + unit(rng) * 0.45;
        const bool dark = rift(l, b) && unit(rng) < 0.75;
        if (keep > w || dark) continue;
        const auto [ra, dec] = galacticToEquatorial(l, b);
        QPointF at;
        qreal altitude;
        if (!projection.place(ra, dec, at, altitude) || !frame.contains(at)) continue;
        painter.fillRect(QRectF(at.x(), at.y(), 0.9, 0.9), withAlpha(QColor(215, 225, 245), grain * strength));
    }
}

void StarfieldWidget::paintMoon(QPainter& painter)
{
    const qreal phase = lunarPhase(m_sky_bucket * SKY_BUCKET_MS);
    const int bucket = static_cast<int>(phase * PHASE_STEPS);
    if (m_moon.isNull() || bucket != m_moon_phase_bucket || !qFuzzyCompare(m_moon.devicePixelRatioF(), devicePixelRatioF())) {
        m_moon_phase_bucket = bucket;
        m_moon = QPixmap::fromImage(renderMoon(phase, devicePixelRatioF()));
    }
    const qreal side = m_moon.width() / m_moon.devicePixelRatioF();
    painter.drawPixmap(m_moon_position - QPointF(side / 2, side / 2), m_moon);
}

void StarfieldWidget::paintScene(QPainter& painter)
{
    const QPointF hero = heroPosition();
    switch (m_tier) {
    case Saturn: paintSaturn(painter, hero); break;
    case Jupiter: paintJupiter(painter, hero); break;
    case IceGiant: paintIceGiant(painter, hero); break;
    case Nebula: {
        if (m_nebula.isNull()) m_nebula = renderNebula();
        const QPointF centre(width() * 0.72, 60);
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRectF(centre.x() - 300, centre.y() - 110, 600, 300), m_nebula);
        painter.restore();
        // Young stars inside the cloud.
        drawStar(painter, centre + QPointF(-40, 2), 1.9, withAlpha(QColor(201, 219, 255), 0.95), 1.0);
        drawStar(painter, centre + QPointF(35, -14), 1.5, withAlpha(QColor(201, 219, 255), 0.9), 1.0);
        drawStar(painter, centre + QPointF(80, 18), 1.2, withAlpha(QColor(255, 255, 255), 0.8), 1.0);
        drawStar(painter, centre + QPointF(-110, -20), 1.0, withAlpha(QColor(201, 219, 255), 0.7), 1.0);
        break;
    }
    case Galaxy: paintGalaxy(painter, hero + QPointF(-10, 0), 1.0, GALAXY_SEED); break;
    case DeepSpace: paintDeepSpace(painter, QRectF(rect()), QPointF(width() * 0.74, 58)); break;
    case Universe:
        paintGalaxy(painter, hero + QPointF(-180, 4), 0.5, SMALL_GALAXY_SEED);
        paintLogoConstellation(painter, hero + QPointF(0, -4), 94);
        break;
    default: break;
    }
}

void StarfieldWidget::redrawSteadySky()
{
    const qreal dpr = devicePixelRatioF();
    m_steady = QPixmap(static_cast<int>(std::ceil(width() * dpr)), static_cast<int>(std::ceil(height() * dpr)));
    m_steady.setDevicePixelRatio(dpr);
    m_steady.fill(Qt::transparent);
    ++m_steady_redraws;
    m_moon_position = moonPosition();
    m_sky_bucket = skyBucket();

    {
        QPainter painter(&m_steady);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        paintField(painter);
        if (hasRealSky()) {
            // The start of the ten-minute slot, not this moment: any rebuild within
            // the slot gives the same picture.
            const qint64 now = m_sky_bucket * SKY_BUCKET_MS;
            const Projection projection(size(), siderealHours(now, longitudeGuess(now)));
            const bool scene = m_tier >= Saturn;
            // From Saturn up the real sky steps back so the scene carries the band.
            if (m_tier >= MilkyWay && m_tier != DeepSpace) paintMilkyWay(painter, projection, m_tier == MilkyWay ? 1.0 : 0.55);
            paintCatalogue(painter, projection, m_tier == DeepSpace ? 0.6 : 1.0);
            if (m_tier >= Constellations && m_tier != DeepSpace) paintLines(painter, projection, scene ? 0.16 : 0.3);
            if (m_tier == Constellations || m_tier == MilkyWay) paintMoon(painter);
            paintScene(painter);
        }
    }
    // The picture is finished before the timers look at it; none of them
    // drops it here, while the paint event that asked for it still uses it.
    updateTimerState();
}

QRect StarfieldWidget::shootingRect(qint64 motion_ms) const
{
    if (m_shooting_started_ms < 0) return QRect();
    const qreal progress = static_cast<qreal>(motion_ms - m_shooting_started_ms) / SHOOTING_MS;
    if (progress < 0 || progress >= 1) return QRect();
    const QPointF head = m_shooting_from + m_shooting_direction * (progress * SHOOTING_LENGTH);
    const QPointF tail = head - m_shooting_direction * SHOOTING_TAIL;
    return QRectF(QPointF(std::min(head.x(), tail.x()), std::min(head.y(), tail.y())),
                  QPointF(std::max(head.x(), tail.x()), std::max(head.y(), tail.y()))).adjusted(-3, -3, 3, 3).toAlignedRect();
}

void StarfieldWidget::startShootingStar(qint64 motion_ms)
{
    QRandomGenerator* random = QRandomGenerator::global();
    m_shooting_started_ms = motion_ms;
    m_shooting_from = QPointF(width() * (0.35 + random->bounded(0.5)), 8 + random->bounded(60.0));
    const qreal dx = 0.8 + random->bounded(0.2), dy = 0.35 + random->bounded(0.3);
    const qreal length = std::hypot(dx, dy);
    m_shooting_direction = QPointF(dx / length, dy / length);
    m_next_shooting_ms = motion_ms + SHOOTING_MS + SHOOTING_GAP_MIN_MS + random->bounded(static_cast<int>(SHOOTING_GAP_MAX_MS - SHOOTING_GAP_MIN_MS));
}

void StarfieldWidget::paintShootingStar(QPainter& painter, qint64 motion_ms) const
{
    if (m_shooting_started_ms < 0) return;
    const qreal progress = static_cast<qreal>(motion_ms - m_shooting_started_ms) / SHOOTING_MS;
    if (progress < 0 || progress >= 1) return;
    const QPointF head = m_shooting_from + m_shooting_direction * (progress * SHOOTING_LENGTH);
    const QPointF tail = head - m_shooting_direction * SHOOTING_TAIL;
    const qreal alpha = std::sin(progress * TAU / 2);
    QLinearGradient gradient(tail, head);
    gradient.setColorAt(0.0, QColor(255, 255, 255, 0));
    gradient.setColorAt(1.0, withAlpha(QColor(255, 255, 255), 0.9 * alpha));
    painter.save();
    painter.setPen(QPen(QBrush(gradient), 1.4));
    painter.drawLine(tail, head);
    painter.restore();
}

QRect StarfieldWidget::cometRect(qint64 motion_ms) const
{
    const qreal t = static_cast<qreal>(motion_ms % COMET_PERIOD_MS) / COMET_PERIOD_MS;
    const QPointF head(width() * 0.33 + (width() * 0.98 - width() * 0.33) * t, 30 + std::sin(t * TAU / 2) * 40);
    return QRectF(head.x() - COMET_TAIL - 30, head.y() - COMET_TAIL - 30, COMET_TAIL + 45, COMET_TAIL + 45).toAlignedRect();
}

void StarfieldWidget::paintComet(QPainter& painter, qint64 motion_ms) const
{
    const qreal t = static_cast<qreal>(motion_ms % COMET_PERIOD_MS) / COMET_PERIOD_MS;
    const QPointF head(width() * 0.33 + (width() * 0.98 - width() * 0.33) * t, 30 + std::sin(t * TAU / 2) * 40);
    painter.save();
    painter.translate(head);
    painter.rotate(std::atan2(-0.5, -0.86) / DEG); // The tail points away from a sun below the lower left.
    QLinearGradient tail(0, 0, COMET_TAIL, 0);
    tail.setColorAt(0.0, QColor(200, 230, 255, 128));
    tail.setColorAt(0.3, QColor(160, 200, 255, 41));
    tail.setColorAt(1.0, QColor(120, 170, 255, 0));
    QPainterPath path;
    path.moveTo(0, -3);
    path.quadTo(COMET_TAIL * 0.5, -16, COMET_TAIL, -24);
    path.lineTo(COMET_TAIL, 18);
    path.quadTo(COMET_TAIL * 0.5, 10, 0, 3);
    path.closeSubpath();
    painter.fillPath(path, tail);
    painter.restore();
    QRadialGradient glow(head, 9);
    glow.setColorAt(0.0, QColor(240, 250, 255, 255));
    glow.setColorAt(0.3, QColor(200, 230, 255, 153));
    glow.setColorAt(1.0, QColor(200, 230, 255, 0));
    painter.setBrush(glow);
    painter.drawEllipse(head, 9, 9);
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
    if (m_steady.isNull()) redrawSteadySky();
    painter.drawPixmap(0, 0, m_steady);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const qreal now = motionMilliseconds() / 1000.0;
    for (const int index : m_twinklers) {
        const Star& star = m_stars.at(index);
        if (!event->region().intersects(starRect(star))) continue;
        const qreal scale = m_animated ? 1 + star.amplitude * std::sin(TAU * now / star.period + star.phase) : 1;
        drawStar(painter, star.pos, star.radius, star.colour, scale);
    }
    // The moving elements are painted at the tick's time, which is the time
    // the tick asked to repaint them at; where they land is kept so the next
    // tick erases it, even after a pause or a missed frame.
    const qint64 live = m_live_motion_ms;
    if (hasShootingStars()) {
        const QRect streak = shootingRect(live);
        if (!streak.isEmpty() && event->region().intersects(streak)) {
            paintShootingStar(painter, live);
            m_live_drawn += streak;
        }
    }
    if (m_tier == IceGiant) {
        const QRect comet = cometRect(live);
        if (event->region().intersects(comet)) {
            paintComet(painter, live);
            m_live_drawn += comet;
        }
    }
}

void StarfieldWidget::onTick()
{
    if (!wantsAnimation()) {
        updateTimerState();
        return;
    }
    if (m_steady.isNull()) {
        m_live_motion_ms = motionMilliseconds();
        update();
        return;
    }
    // The real sky drifts a quarter degree in a minute: a quiet redraw every
    // ten minutes keeps it honest without anyone seeing it move.
    if (hasRealSky() && skyBucket() != m_sky_bucket) {
        m_live_motion_ms = motionMilliseconds();
        invalidateImages();
        update();
        return;
    }
    const QRegion dirty = tickRegion(motionMilliseconds());
    if (!dirty.isEmpty()) update(dirty);
}

QRegion StarfieldWidget::tickRegion(qint64 motion_ms)
{
    m_live_motion_ms = motion_ms;
    QRegion dirty = m_live_drawn;
    m_live_drawn = QRegion();
    for (const int index : m_twinklers) dirty += starRect(m_stars.at(index));
    if (hasShootingStars()) {
        // A streak in flight needs the fast frames; when it lands, a sky with
        // no visible twinkler goes back to the slow timer.
        const bool was_flying = m_shooting_started_ms >= 0;
        if (was_flying && motion_ms - m_shooting_started_ms >= SHOOTING_MS) m_shooting_started_ms = -1;
        if (m_shooting_started_ms < 0 && motion_ms >= m_next_shooting_ms) startShootingStar(motion_ms);
        if (was_flying != (m_shooting_started_ms >= 0)) updateTimerState();
        dirty += shootingRect(motion_ms);
    }
    if (m_tier == IceGiant) dirty += cometRect(motion_ms);
    return dirty & rect();
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
        // A streak caught mid-flight would stay on a still window as a scratch.
        if (m_shooting_started_ms >= 0) {
            m_shooting_started_ms = -1;
            update(m_live_drawn & rect());
            m_live_drawn = QRegion();
        }
        updateRefreshTimer();
        return;
    }
    m_refresh->stop();
    bool fast = m_shooting_started_ms >= 0;
    for (const int index : m_twinklers) {
        if (starRect(m_stars.at(index)).intersects(rect())) {
            fast = true;
            break;
        }
    }
    const int interval = fast ? FRAME_INTERVAL_MS : IDLE_INTERVAL_MS;
    if (m_timer->interval() != interval) m_timer->setInterval(interval);
    if (!m_timer->isActive()) m_timer->start();
}

void StarfieldWidget::updateRefreshTimer()
{
    // Hidden, minimized or still windows, and the plain field, need nothing:
    // they catch up on the first tick once they animate again.
    const QWidget* top = window();
    if (!m_skyVisible || !m_animated || !hasRealSky() || !isVisible() || top == nullptr || top->isMinimized()) {
        m_refresh->stop();
        return;
    }
    // Only armed here: this also runs at the end of a cache rebuild, inside a
    // paint event, where nothing may be dropped. A cache already behind (a
    // slot turned while it was drawn, or the window was away) is redrawn from
    // the event loop at once; otherwise a second past the turn of the slot.
    // One look at the clock decides both, so a turn between two looks cannot
    // leave the old sky up for a whole slot.
    const qint64 now = phaseTimeMilliseconds();
    const bool behind = !m_steady.isNull() && now / SKY_BUCKET_MS != m_sky_bucket;
    m_refresh->start(behind ? 0 : static_cast<int>(SKY_BUCKET_MS - now % SKY_BUCKET_MS + 1000));
}

void StarfieldWidget::onRefresh()
{
    if (!m_steady.isNull() && hasRealSky() && skyBucket() != m_sky_bucket) {
        invalidateImages();
        update();
    }
    updateRefreshTimer();
}

void StarfieldWidget::invalidateImages()
{
    // Every caller repaints the whole widget, which also covers what moved.
    m_live_drawn = QRegion();
    m_steady = QPixmap();
    m_moon = QPixmap();
    m_moon_phase_bucket = -1;
}

void StarfieldWidget::setSkyVisible(bool visible)
{
    if (m_skyVisible == visible) return;
    m_skyVisible = visible;
    invalidateImages();
    if (visible) {
        buildField();
    } else {
        m_stars.clear();
        m_stars.squeeze();
        m_visible.clear();
        m_visible.squeeze();
        m_twinklers.clear();
        m_twinklers.squeeze();
        m_nebula = QImage();
        m_shooting_started_ms = -1;
    }
    updateTimerState();
    update();
}

void StarfieldWidget::setAnimated(bool animated)
{
    if (m_animated == animated) return;
    if (!animated) {
        m_pause_started_ms = clockMilliseconds();
        m_frozen_utc_ms = utcMilliseconds();
        m_shooting_started_ms = -1;
    } else {
        m_paused_ms += clockMilliseconds() - m_pause_started_ms;
    }
    m_animated = animated;
    // A still sky shows the time it stopped at. A cache from an older
    // ten-minute slot (after a corrected clock, or a long inactive spell
    // without ticks) is drawn once more for it.
    if (!animated && !m_steady.isNull() && hasRealSky() && skyBucket() != m_sky_bucket) {
        invalidateImages();
        update();
    }
    updateTimerState();
    // The sky and the Moon catch up to the date on the next tick; the stars
    // that breathe settle or resume now, and a streak in flight is erased.
    QRegion dirty = m_live_drawn;
    m_live_drawn = QRegion();
    for (const int index : m_twinklers) dirty += starRect(m_stars.at(index));
    if (m_tier == IceGiant) dirty += cometRect(m_live_motion_ms);
    update(dirty.intersected(rect()));
}

void StarfieldWidget::setSkyTier(int tier)
{
    tier = std::clamp(tier, 0, SkyTiers::COUNT - 1);
    if (m_tier == tier) return;
    m_tier = tier;
    m_shooting_started_ms = -1;
    if (!m_skyVisible) return; // Classic themes keep nothing but the number.
    invalidateImages();
    updateTimerState();
    update();
}

void StarfieldWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_skyVisible) updateVisibleStars();
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
