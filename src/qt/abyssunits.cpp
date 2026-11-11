// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.

#include <qt/abyssunits.h>

#include <qt/bitcoinunits.h>

#include <QSettings>

#include <algorithm>

namespace AbyssUnits {

QString number(int unit, const CAmount& amount, bool plussign, bool floor)
{
    QString text = BitcoinUnits::format(unit, amount, plussign, BitcoinUnits::SeparatorStyle::ALWAYS);
    const int decimals = BitcoinUnits::decimals(unit);
    const int point = text.indexOf(QLatin1Char('.'));
    if (decimals <= 0 || point < 0) return text; // a whole unit: its zeros are digits
    if (floor) {
        // As BitcoinUnits::floorWithUnit cuts: the setting's digits, towards zero.
        const int digits = std::clamp(QSettings().value("digits").toInt(), 0, decimals);
        const QString floored = text.left(text.size() - (decimals - digits));
        // A non-zero amount is never shown as zero: "0.00" for 0.003 reads as
        // nothing, so that amount keeps all of its digits.
        const bool nothing_left = std::none_of(floored.cbegin(), floored.cend(),
                                               [](QChar c) { return c.isDigit() && c != QLatin1Char('0'); });
        if (amount == 0 || !nothing_left) text = floored;
    }
    int end = text.size();
    while (end > point + 1 && text.at(end - 1) == QLatin1Char('0')) --end;
    if (end == point + 1) end = point; // nothing left after the point
    text.truncate(end);
    return text;
}

QString format(int unit, const CAmount& amount, bool plussign, bool floor)
{
    return number(unit, amount, plussign, floor) + QLatin1Char(' ') + BitcoinUnits::name(unit);
}

QString exact(int unit, const CAmount& amount, bool plussign)
{
    return BitcoinUnits::formatWithUnit(unit, amount, plussign, BitcoinUnits::SeparatorStyle::ALWAYS);
}

} // namespace AbyssUnits
