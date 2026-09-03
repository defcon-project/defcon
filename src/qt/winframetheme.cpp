// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/winframetheme.h>

#if defined(Q_OS_WIN)

#include <qt/guiutil.h>

#include <windows.h>

#include <QApplication>
#include <QEvent>
#include <QObject>
#include <QWidget>
#include <QWindow>

namespace GUIUtil {
namespace {

using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

//! Resolved at run time instead of linked. It keeps dwmapi out of the build
//! system for the sake of one call, and the attribute this needs is numbered
//! differently on different releases anyway, so a link-time dependency would
//! buy no certainty about whether the call works.
DwmSetWindowAttributeFn DwmSetWindowAttributeOrNull()
{
    static DwmSetWindowAttributeFn fn = []() -> DwmSetWindowAttributeFn {
        HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
        if (dwm == nullptr) {
            return nullptr;
        }
        // Through void*, because GetProcAddress returns FARPROC and a direct
        // cast to a specific signature is what -Wcast-function-type warns
        // about. Verified against real mingw headers: this form is clean where
        // the one-step cast is not.
        return reinterpret_cast<DwmSetWindowAttributeFn>(
            reinterpret_cast<void*>(::GetProcAddress(dwm, "DwmSetWindowAttribute")));
    }();
    return fn;
}

//! DWMWA_USE_IMMERSIVE_DARK_MODE. Windows 10 1809 numbered it 19 and rejects
//! 20; every release after it numbers it 20 and rejects 19. Trying the current
//! one first costs a single failed call on exactly one old release.
constexpr DWORD kImmersiveDarkMode{20};
constexpr DWORD kImmersiveDarkMode1809{19};

//! Themes a window the moment it is shown, so a dialog opened long after
//! startup is framed like the window that opened it.
class FrameThemeFilter final : public QObject
{
public:
    explicit FrameThemeFilter(QObject* parent) : QObject(parent) {}

    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::Show && object->isWidgetType()) {
            auto* widget = static_cast<QWidget*>(object);
            if (widget->isWindow()) {
                applyWindowFrameTheme(widget);
            }
        }
        return QObject::eventFilter(object, event);
    }
};

} // namespace

void applyWindowFrameTheme(QWidget* window)
{
    if (window == nullptr || !window->isWindow()) {
        return;
    }
    // windowHandle() rather than winId(): the latter creates the native window
    // as a side effect, and a window that has not been created yet has no frame
    // to theme. It passes through here again when it is shown.
    if (window->windowHandle() == nullptr) {
        return;
    }
    const auto set_attribute = DwmSetWindowAttributeOrNull();
    if (set_attribute == nullptr) {
        return;
    }
    auto* const handle = reinterpret_cast<HWND>(window->winId());
    if (handle == nullptr) {
        return;
    }
    const BOOL dark = themeIsDark() ? TRUE : FALSE;
    if (FAILED(set_attribute(handle, kImmersiveDarkMode, &dark, sizeof(dark)))) {
        set_attribute(handle, kImmersiveDarkMode1809, &dark, sizeof(dark));
    }
}

void applyWindowFrameThemeToAll()
{
    for (QWidget* window : QApplication::topLevelWidgets()) {
        applyWindowFrameTheme(window);
    }
}

void installWindowFrameThemeFilter()
{
    qApp->installEventFilter(new FrameThemeFilter(qApp));
}

} // namespace GUIUtil

#endif // Q_OS_WIN
