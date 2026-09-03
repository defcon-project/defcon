// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_WINFRAMETHEME_H
#define BITCOIN_QT_WINFRAMETHEME_H

#include <QtGlobal>

class QWidget;

namespace GUIUtil {

/**
 * Keep the window frame -- the title bar the operating system draws, not the
 * one the stylesheet reaches -- in step with the theme the wallet is using.
 *
 * Only Windows offers this. Elsewhere these are empty inline functions, so a
 * caller never needs a platform guard and nothing is compiled that cannot run.
 * Under WSLg the frame belongs to the WSLg client on the Windows side, and no
 * process inside the Linux VM can reach it; that is not a case these functions
 * can serve, and they do not pretend to.
 */
#if defined(Q_OS_WIN)

//! Apply the current theme to one top-level window. A window with no native
//! handle yet is left alone rather than forced into existence.
void applyWindowFrameTheme(QWidget* window);

//! Re-apply to every window that already exists. Called when the theme changes.
void applyWindowFrameThemeToAll();

//! Apply to every window opened from now on, dialogs included.
void installWindowFrameThemeFilter();

#else

inline void applyWindowFrameTheme(QWidget*) {}
inline void applyWindowFrameThemeToAll() {}
inline void installWindowFrameThemeFilter() {}

#endif // Q_OS_WIN

} // namespace GUIUtil

#endif // BITCOIN_QT_WINFRAMETHEME_H
