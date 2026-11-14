// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_WAVEBACKDROP_H
#define BITCOIN_QT_WAVEBACKDROP_H

#include <QImage>
#include <QSize>

/**
 * The Abyss backdrop: a few sheets of blue light folding over a dark ground,
 * one bright crest among them. Computed, not shipped as a picture, so it is
 * sharp at any size and pixel ratio and the same for the same size. Nothing
 * moves and nothing is read: no node, wallet, clock or setting.
 */
namespace WaveBackdrop {
/** The picture for an area of `pixels` device pixels shown at `dpr`. The
 *  composition follows the area's proportions; the crest keeps a visible
 *  width on a small area. Opaque; null for an empty size. */
QImage render(const QSize& pixels, qreal dpr);
} // namespace WaveBackdrop

#endif // BITCOIN_QT_WAVEBACKDROP_H
