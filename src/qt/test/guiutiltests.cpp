// Copyright (c) 2024 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/guiutiltests.h>

#include <qt/guiutil.h>

#include <QFont>

#include <optional>

void GUIUtilTests::effectivePointSizeTests()
{
    // A point-sized font reports its own size and needs no conversion. The DPI
    // is irrelevant on this path, so it is passed as something absurd to prove
    // the point size wins.
    {
        QFont font;
        font.setPointSizeF(11.5);
        const std::optional<double> size{GUIUtil::internal::EffectivePointSize(font, 1)};
        QVERIFY(size.has_value());
        QCOMPARE(*size, 11.5);
    }

    // The case F-2026-030 is about: a stylesheet rule `font-size: 17px` leaves
    // the font pixel-sized, and at 96 DPI that is 17 * 72 / 96 = 12.75 points.
    // Exactly 12.75, not 13 -- rounding here is the defect.
    {
        QFont font;
        font.setPixelSize(17);
        const std::optional<double> size{GUIUtil::internal::EffectivePointSize(font, 96)};
        QVERIFY(size.has_value());
        QCOMPARE(*size, 12.75);
    }

    // A whole-point conversion still lands on a whole point.
    {
        QFont font;
        font.setPixelSize(16);
        const std::optional<double> size{GUIUtil::internal::EffectivePointSize(font, 96)};
        QVERIFY(size.has_value());
        QCOMPARE(*size, 12.0);
    }

    // A non-positive DPI cannot convert, and must report that rather than
    // dividing by zero or returning a nonsense size. This is the branch that
    // keeps updateFonts() skipping a widget instead of aborting the GUI, and it
    // is the only way to reach nullopt from a test.
    //
    // The other nullopt case -- a font whose pointSizeF() is negative, which a
    // box-engine fallback can produce as something like -0.72 -- cannot be
    // built through the public API: QFont::setPointSizeF refuses a value <= 0
    // and leaves the font unchanged, so the call still converts from the pixel
    // size. An earlier version of this test asserted that case and failed for
    // exactly that reason. The production guard compares against 0 rather than
    // the -1 sentinel and is covered by inspection, not here.
    {
        QFont font;
        font.setPixelSize(17);
        QVERIFY(!GUIUtil::internal::EffectivePointSize(font, 0).has_value());
        QVERIFY(!GUIUtil::internal::EffectivePointSize(font, -96).has_value());
    }
}

void GUIUtilTests::scaledFontSizeTests()
{
    // At the default scale the scaler is the identity on quarter points, so it
    // must carry a fractional input through instead of flattening it. These two
    // differing is the whole of F-2026-030: before the fix the second value
    // could only ever be reached by way of 13.0.
    const int scale_before{GUIUtil::getFontScale()};
    GUIUtil::setFontScale(GUIUtil::getFontScaleDefault());

    QCOMPARE(GUIUtil::getScaledFontSize(12.75), 12.75);
    QCOMPARE(GUIUtil::getScaledFontSize(13.0), 13.0);
    QVERIFY(GUIUtil::getScaledFontSize(12.75) != GUIUtil::getScaledFontSize(13.0));

    // Integer callers are unchanged: a whole number in, the same whole number
    // out, which is what every existing call site relied on.
    QCOMPARE(GUIUtil::getScaledFontSize(12), 12.0);

    // The result is still snapped to quarter points -- that rounding is the
    // fork's scaling rule and this change does not touch it.
    QCOMPARE(GUIUtil::getScaledFontSize(12.8), 12.75);

    GUIUtil::setFontScale(scale_before);
}
