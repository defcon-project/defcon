// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.

#ifndef BITCOIN_QT_ABYSSUNITS_H
#define BITCOIN_QT_ABYSSUNITS_H

#include <consensus/amount.h>

#include <QString>

/** The Abyss theme's amounts (frontend roadmap U3-C): the display unit's value
 *  without the trailing zeros of its fraction ("0.5 DFCN", "935 000 000 DFCN"),
 *  thousands grouped with a thin space. Display only: wherever a value leaves
 *  the screen (copy, export, details) the exact BitcoinUnits text stays. */
namespace AbyssUnits {

/** The number alone. With `floor`, the fraction is first cut to the user's
 *  "digits" setting, as BitcoinUnits::floorWithUnit does; a non-zero amount
 *  that this would leave at zero is shown exactly instead. */
QString number(int unit, const CAmount& amount, bool plussign = false, bool floor = false);

/** number() followed by a space and the unit's name. */
QString format(int unit, const CAmount& amount, bool plussign = false, bool floor = false);

/** Every decimal, with the unit's name: what a tooltip gives. */
QString exact(int unit, const CAmount& amount, bool plussign = false);

} // namespace AbyssUnits

#endif // BITCOIN_QT_ABYSSUNITS_H
