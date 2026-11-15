// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/wavebackdrop.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace WaveBackdrop {
namespace {
struct Colour {
    float red, green, blue;
};

constexpr Colour hex(unsigned value)
{
    return {((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f};
}

//! A near-black navy ground, two layer colours (night blue, a restrained
//! sapphire) and the colour of the thin highlight on some edges.
constexpr Colour GROUND = hex(0x060E18);
constexpr Colour NIGHT = hex(0x0A1C32);
constexpr Colour SAPPHIRE = hex(0x103356);
constexpr Colour LIGHT = hex(0x3D78B8);

/** One wave, a layer laid over what is under it. Its upper edge is the cubic
 *  Bezier through `x` and `y` (x grows along it), in the area's own 0 to 1
 *  coordinates, u across and v down. Below the edge the layer covers what is
 *  under it by `opacity`, fading over `depth` of the area's height; when
 *  `fade_to` is above `fade_from` it also fades in along u between the two.
 *  A thin highlight runs on the edge, `light` strong at u = `light_at`,
 *  falling off over `light_over` to either side. */
struct Wave {
    float x[4];
    float y[4];
    Colour colour;
    float opacity, depth;
    float light, light_at, light_over;
    float fade_from, fade_to;
};

//! Back to front: each wave covers part of the ones before it.
constexpr Wave WAVES[] = {
    // The faint wide arc in the upper right; it fades in from the left.
    {{0.30f, 0.62f, 0.84f, 1.06f}, {0.52f, 0.46f, 0.20f, 0.04f}, NIGHT, 0.55f, 0.50f, 0.10f, 0.82f, 0.20f, 0.40f, 0.78f},
    // The main waves, lower middle, rising gently to the right.
    {{-0.06f, 0.36f, 0.60f, 1.06f}, {0.56f, 0.86f, 0.44f, 0.50f}, NIGHT, 0.80f, 0.42f, 0.16f, 0.30f, 0.28f, 0.0f, 0.0f},
    {{-0.06f, 0.40f, 0.56f, 1.06f}, {0.80f, 0.96f, 0.52f, 0.60f}, SAPPHIRE, 0.62f, 0.30f, 0.34f, 0.64f, 0.22f, 0.0f, 0.0f},
    {{-0.06f, 0.34f, 0.66f, 1.06f}, {0.92f, 0.86f, 0.98f, 0.72f}, SAPPHIRE, 0.45f, 0.26f, 0.14f, 0.70f, 0.30f, 0.0f, 0.0f},
};
constexpr int COUNT = sizeof(WAVES) / sizeof(WAVES[0]);

//! The highlight's width, of the area's height; above FULL_HEIGHT logical
//! pixels a taller area does not thicken it.
constexpr float EDGE_WIDTH = 0.0017f;
constexpr float FULL_HEIGHT = 900.0f;

//! How far below its edge a layer's fading is tabulated, in area heights,
//! and in how many steps for one area height. Further down it is held.
constexpr float FALL_REACH = 1.25f;
constexpr int FALL_STEPS = 1024;

/** Where a wave's edge runs in each column: the curve sampled finely and read
 *  where it passes the column's centre; beyond its ends it is held. */
std::vector<float> edgeLine(const Wave& wave, int w)
{
    const int n = 4 * w + 1;
    std::vector<float> bx(n), by(n);
    for (int k = 0; k < n; ++k) {
        const float t = static_cast<float>(k) / (n - 1);
        const float s = 1.0f - t;
        const float a = s * s * s, b = 3 * s * s * t, c = 3 * s * t * t, d = t * t * t;
        bx[k] = a * wave.x[0] + b * wave.x[1] + c * wave.x[2] + d * wave.x[3];
        by[k] = a * wave.y[0] + b * wave.y[1] + c * wave.y[2] + d * wave.y[3];
    }
    std::vector<float> line(w);
    int k = 0;
    for (int x = 0; x < w; ++x) {
        const float u = (x + 0.5f) / w;
        if (u <= bx[0]) {
            line[x] = by[0];
        } else if (u >= bx[n - 1]) {
            line[x] = by[n - 1];
        } else {
            // x grows strictly along the curve (its control points do), so
            // the walk only goes forward and never divides by zero.
            while (bx[k + 1] < u) ++k;
            line[x] = by[k] + (by[k + 1] - by[k]) * (u - bx[k]) / (bx[k + 1] - bx[k]);
        }
    }
    return line;
}

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
    return std::clamp(static_cast<int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + dither), 0, 255);
}
} // namespace

QImage render(const QSize& pixels, qreal dpr)
{
    const int w = pixels.width(), h = pixels.height();
    if (w <= 0 || h <= 0) return QImage();
    QImage image(w, h, QImage::Format_RGB32);
    image.setDevicePixelRatio(dpr);

    const float ratio = static_cast<float>(dpr);
    // The highlight is never thinner than about a logical pixel; a layer's
    // own edge is antialiased over a little more than one.
    const float width = std::max(EDGE_WIDTH * std::min(1.0f, FULL_HEIGHT * ratio / h), 0.9f * ratio / h);
    const float soft = 1.2f * ratio / h;
    // Beyond four widths the highlight adds less than a millionth.
    const float shine_reach = 4.0f * width;

    // Everything that depends on the column alone: where each edge runs, how
    // much of the layer its fade-in lets through, how strong the highlight is
    // there, and the column's share of the corners' dimming.
    const size_t cells = static_cast<size_t>(COUNT) * w;
    std::vector<float> line(cells), cover(cells), shine(cells), dark_x(w);
    for (int i = 0; i < COUNT; ++i) {
        const Wave& wave = WAVES[i];
        const std::vector<float> edge = edgeLine(wave, w);
        for (int x = 0; x < w; ++x) {
            const float u = (x + 0.5f) / w;
            const size_t at = static_cast<size_t>(i) * w + x;
            line[at] = edge[x];
            float fade = 1.0f;
            if (wave.fade_to > wave.fade_from) {
                const float f = std::clamp((u - wave.fade_from) / (wave.fade_to - wave.fade_from), 0.0f, 1.0f);
                fade = f * f * (3 - 2 * f);
            }
            cover[at] = wave.opacity * fade;
            const float l = (u - wave.light_at) / wave.light_over;
            shine[at] = wave.light * std::exp(-l * l);
        }
    }
    for (int x = 0; x < w; ++x) {
        const float k = ((x + 0.5f) / w - 0.55f) / 0.85f;
        dark_x[x] = k * k;
    }
    // Each layer's fading below its edge, tabulated: one table read a pixel.
    const size_t fall_count = static_cast<size_t>(FALL_STEPS * FALL_REACH) + 2;
    std::vector<float> fall(static_cast<size_t>(COUNT) * fall_count);
    for (int i = 0; i < COUNT; ++i) {
        for (size_t s = 0; s < fall_count; ++s) {
            fall[i * fall_count + s] = std::exp(-(static_cast<float>(s) / FALL_STEPS) / WAVES[i].depth);
        }
    }

    for (int y = 0; y < h; ++y) {
        const float v = (y + 0.5f) / h;
        const float k = (v - 0.55f) / 0.95f;
        const float dark_y = k * k;
        QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            float red = GROUND.red, green = GROUND.green, blue = GROUND.blue;
            for (int i = 0; i < COUNT; ++i) {
                const Wave& wave = WAVES[i];
                const size_t at = static_cast<size_t>(i) * w + x;
                const float d = v - line[at]; // positive below the edge
                if (d > -0.5f * soft) {
                    const float inside = std::min(d / soft + 0.5f, 1.0f);
                    const float position = std::clamp(d, 0.0f, FALL_REACH) * FALL_STEPS;
                    const size_t index = std::min(static_cast<size_t>(position), fall_count - 2);
                    const float* table = &fall[i * fall_count + index];
                    const float falling = table[0] + (table[1] - table[0]) * (position - index);
                    const float alpha = cover[at] * inside * falling;
                    red += (wave.colour.red - red) * alpha;
                    green += (wave.colour.green - green) * alpha;
                    blue += (wave.colour.blue - blue) * alpha;
                }
                if (std::abs(d) < shine_reach) {
                    const float q = d / width;
                    const float added = shine[at] * std::exp(-q * q);
                    red += added * LIGHT.red;
                    green += added * LIGHT.green;
                    blue += added * LIGHT.blue;
                }
            }
            // The corners fall back towards the ground.
            const float keep = 1.0f - 0.30f * std::min(dark_x[x] + dark_y, 1.0f);
            const float dither = (BAYER[y & 7][x & 7] + 0.5f) / 64.0f;
            row[x] = qRgb(channel(GROUND.red * 0.55f + (red - GROUND.red * 0.55f) * keep, dither),
                          channel(GROUND.green * 0.55f + (green - GROUND.green * 0.55f) * keep, dither),
                          channel(GROUND.blue * 0.55f + (blue - GROUND.blue * 0.55f) * keep, dither));
        }
    }
    return image;
}
} // namespace WaveBackdrop
