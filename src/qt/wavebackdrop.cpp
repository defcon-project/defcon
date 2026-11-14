// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/wavebackdrop.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace WaveBackdrop {
namespace {
constexpr float TAU = 6.28318530717958647692f;

/** One sheet of light. Its upper edge runs along
 *      v(u) = c + s*u + q*u*u + a*sin(TAU*(f*u + p))
 *  with u across and v down the area, both 0 to 1. The edge glows (`crest`,
 *  `width` of the area's height wide), the sheet hangs below it and fades
 *  over `sheet`, and both are brightest around `env_centre`, fading over
 *  `env_width` to either side. */
struct Ribbon {
    float c, s, q, a, f, p;
    float width, sheet, crest, fill;
    float env_centre, env_width;
    float red, green, blue;
};

constexpr Ribbon RIBBONS[] = {
    // The bright crest: a valley on the left, a rise through the middle, a hump on the right.
    {0.670f, -0.050f, 0.000f, 0.120f, 1.25f, -0.200f, 0.0030f, 0.22f, 1.00f, 0.20f, 0.60f, 0.21f, 0.13f, 0.42f, 1.00f},
    // The sheet coming in from the left edge.
    {0.645f, 0.000f, 0.000f, 0.065f, 0.90f, -0.128f, 0.0055f, 0.20f, 0.30f, 0.13f, 0.40f, 0.42f, 0.07f, 0.27f, 0.80f},
    // The low, nearly level one.
    {0.800f, 0.020f, 0.000f, 0.030f, 1.10f, 0.150f, 0.0070f, 0.16f, 0.22f, 0.10f, 0.62f, 0.40f, 0.06f, 0.24f, 0.74f},
    // The fold climbing to the right edge.
    {1.060f, -0.730f, 0.000f, 0.020f, 1.20f, 0.100f, 0.0060f, 0.26f, 0.24f, 0.12f, 0.86f, 0.24f, 0.07f, 0.27f, 0.80f},
    // The wide fold in the upper right.
    {0.620f, -0.750f, 0.150f, 0.000f, 1.00f, 0.000f, 0.0080f, 0.34f, 0.18f, 0.12f, 0.84f, 0.30f, 0.05f, 0.20f, 0.66f},
};

//! The ground, and the wide glow where the sheets gather.
constexpr float GROUND[3] = {0.016f, 0.036f, 0.070f};
constexpr float GLOW[3] = {0.004f, 0.030f, 0.085f};
//! What the crest adds at its very brightest, towards white.
constexpr float SHINE[3] = {0.20f, 0.24f, 0.20f};

//! The distance from an edge the tables cover, in area heights, either way.
constexpr float REACH = 1.6f;

/** A sheet's brightness by the distance below its edge (negative above),
 *  tabulated: the picture is then a few table reads for each pixel. */
struct Profile {
    std::vector<float> light; //!< The edge's glow and the sheet below it.
    std::vector<float> core;  //!< The edge alone, for the shine.
    float scale;              //!< Table steps for one area height.
    float core_reach;         //!< Beyond this distance the edge adds nothing.

    Profile(const Ribbon& ribbon, float width)
    {
        // An eighth of the edge's width for a step: the edge is drawn smooth.
        scale = 8.0f / width;
        core_reach = 4.0f * width;
        const int count = static_cast<int>(std::ceil(2 * REACH * scale)) + 2;
        light.resize(count);
        core.resize(count);
        for (int i = 0; i < count; ++i) {
            const float d = i / scale - REACH;
            const float edge = std::exp(-(d / width) * (d / width));
            const float halo = std::exp(-std::abs(d) / (width * 7));
            const float below = d > 0 ? std::exp(-d / ribbon.sheet) * (1 - std::exp(-d / (width * 2))) : 0.0f;
            light[i] = ribbon.crest * (0.70f * edge + 0.30f * halo) + ribbon.fill * below;
            core[i] = ribbon.crest * edge;
        }
    }

    static float read(const std::vector<float>& table, float position)
    {
        const int last = static_cast<int>(table.size()) - 2;
        const int index = std::clamp(static_cast<int>(position), 0, last);
        const float part = std::clamp(position - index, 0.0f, 1.0f);
        return table[index] + (table[index + 1] - table[index]) * part;
    }
};

//! Ordered dither, 8x8: the dark, slow gradients would band in eight bits.
constexpr unsigned char BAYER[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

inline int channel(float value, float dither)
{
    return std::clamp(static_cast<int>(value * 255.0f + dither), 0, 255);
}
} // namespace

QImage render(const QSize& pixels, qreal dpr)
{
    const int w = pixels.width(), h = pixels.height();
    if (w <= 0 || h <= 0) return QImage();
    QImage image(w, h, QImage::Format_RGB32);
    image.setDevicePixelRatio(dpr);

    constexpr int COUNT = sizeof(RIBBONS) / sizeof(RIBBONS[0]);
    // An edge is never thinner than about a logical pixel, however low the area.
    const float thinnest = 1.1f * static_cast<float>(dpr) / h;
    std::vector<Profile> profiles;
    profiles.reserve(COUNT);
    for (const Ribbon& ribbon : RIBBONS) profiles.emplace_back(ribbon, std::max(ribbon.width, thinnest));

    // Everything that depends on the column alone: where each edge runs and
    // how bright the sheet is there, the glow's and the vignette's share.
    std::vector<float> line(static_cast<size_t>(COUNT) * w), envelope(static_cast<size_t>(COUNT) * w);
    std::vector<float> glow_x(w), dark_x(w);
    for (int x = 0; x < w; ++x) {
        const float u = (x + 0.5f) / w;
        for (int i = 0; i < COUNT; ++i) {
            const Ribbon& r = RIBBONS[i];
            line[static_cast<size_t>(i) * w + x] = r.c + r.s * u + r.q * u * u + r.a * std::sin(TAU * (r.f * u + r.p));
            const float e = (u - r.env_centre) / r.env_width;
            envelope[static_cast<size_t>(i) * w + x] = std::exp(-e * e);
        }
        const float g = (u - 0.68f) / 0.50f;
        glow_x[x] = std::exp(-g * g);
        const float k = (u - 0.55f) / 0.80f;
        dark_x[x] = k * k;
    }

    for (int y = 0; y < h; ++y) {
        const float v = (y + 0.5f) / h;
        const float g = (v - 0.70f) / 0.34f;
        const float glow_y = std::exp(-g * g);
        const float k = (v - 0.55f) / 0.90f;
        const float dark_y = k * k;
        QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const float glow = glow_x[x] * glow_y;
            float red = GROUND[0] + glow * GLOW[0];
            float green = GROUND[1] + glow * GLOW[1];
            float blue = GROUND[2] + glow * GLOW[2];
            for (int i = 0; i < COUNT; ++i) {
                const Ribbon& r = RIBBONS[i];
                const Profile& profile = profiles[i];
                const float d = v - line[static_cast<size_t>(i) * w + x];
                const float e = envelope[static_cast<size_t>(i) * w + x];
                const float position = (d + REACH) * profile.scale;
                const float light = e * Profile::read(profile.light, position);
                red += light * r.red;
                green += light * r.green;
                blue += light * r.blue;
                if (std::abs(d) < profile.core_reach) {
                    const float core = e * Profile::read(profile.core, position);
                    const float shine = core * core;
                    red += shine * SHINE[0];
                    green += shine * SHINE[1];
                    blue += shine * SHINE[2];
                }
            }
            // The corners fall back to the dark.
            const float keep = 1.0f - 0.45f * std::min(dark_x[x] + dark_y, 1.0f);
            const float dither = (BAYER[y & 7][x & 7] + 0.5f) / 64.0f;
            row[x] = qRgb(channel(red * keep, dither), channel(green * keep, dither), channel(blue * keep, dither));
        }
    }
    return image;
}
} // namespace WaveBackdrop
