// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_WAVEBACKDROP_H
#define BITCOIN_QT_WAVEBACKDROP_H

#include <QImage>
#include <QSize>

/**
 * The Abyss backdrop: a near-black navy ground with a few wide waves in night
 * blue and a restrained sapphire laid over each other, a faint arc in the
 * upper right, and a thin, faint highlight on some edges; no glow. Computed,
 * not shipped as a picture, so it is sharp at any size and pixel ratio and the
 * same for the same size. Nothing moves and nothing is read: no node, wallet,
 * clock or setting.
 */
namespace WaveBackdrop {
/** The picture for an area of `pixels` device pixels shown at `dpr`. The
 *  composition follows the area's proportions; a highlight is never thinner
 *  than about a logical pixel. Opaque; null for an empty size. */
QImage render(const QSize& pixels, qreal dpr);
} // namespace WaveBackdrop

#endif // BITCOIN_QT_WAVEBACKDROP_H
