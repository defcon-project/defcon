// Copyright (c) 2024 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_GUIUTILTESTS_H
#define BITCOIN_QT_TEST_GUIUTILTESTS_H

#include <QObject>
#include <QTest>

/**
 * Font-size arithmetic in GUIUtil.
 *
 * These are pure arithmetic cases on purpose. Upstream's widget-level
 * regression QSKIPs wherever fonts cannot initialise, and `minimal` is the only
 * Qt platform plugin built here, so a widget test would skip and prove nothing.
 * What must not skip is the conversion itself: a stylesheet `font-size: 17px`
 * at 96 DPI is 12.75 pt, and rounding that to 13 pt before scaling was
 * F-2026-030.
 */
class GUIUtilTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void effectivePointSizeTests();
    void scaledFontSizeTests();
};

#endif // BITCOIN_QT_TEST_GUIUTILTESTS_H
