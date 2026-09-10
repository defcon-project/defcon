// Copyright (c) 2014-2019 The Bitcoin Core developers
// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/networkstyle.h>

#include <qt/guiconstants.h>
#include <qt/guiutil.h>

#include <chainparams.h>
#include <tinyformat.h>
#include <util/system.h>

#include <chainparamsbase.h>

#include <QApplication>

static const struct {
    const char *networkId;
    const char *appName;
    const int iconColorHueShift;
    const int iconColorSaturationReduction;
    const std::string titleAddText;
} network_styles[] = {
    {"main", QAPP_APP_NAME_DEFAULT, 0, 0, ""},
    {"test", QAPP_APP_NAME_TESTNET, 190, 20},
    {"devnet", QAPP_APP_NAME_DEVNET, 190, 20, "[devnet: %s]"},
    {"regtest", QAPP_APP_NAME_REGTEST, 160, 30}
};

void NetworkStyle::rotateColor(QColor& col, const int iconColorHueShift, const int iconColorSaturationReduction)
{
    int h, s, l, a;
    col.getHsl(&h, &s, &l, &a);

    // rotate color on RGB color circle
    h += iconColorHueShift;
    // change saturation value
    s -= iconColorSaturationReduction;
    s = std::max(s, 0);

    col.setHsl(h,s,l,a);
}

void NetworkStyle::rotateColors(QImage& img, const int iconColorHueShift, const int iconColorSaturationReduction)
{
    // traverse though lines
    for(int y=0;y<img.height();y++)
    {
        QRgb *scL = reinterpret_cast< QRgb *>( img.scanLine( y ) );

        // loop through pixels
        for(int x=0;x<img.width();x++)
        {
            QColor col;
            col.setRgba(scL[x]);
            rotateColor(col, iconColorHueShift, iconColorSaturationReduction);
            scL[x] = col.rgba();
        }
    }
}

namespace {
//! The sizes a window manager, a task bar and a tray are likely to ask for.
//! Above the largest, Qt scales the largest down, which is what it does for any
//! icon.
const QList<int> usualIconSizes{16, 24, 32, 48, 64, 128, 256};

//! Build an icon that renders from vector art but still declares its sizes.
//!
//! An icon constructed straight from an SVG draws correctly at any size and
//! reports **no available sizes at all** -- being scalable, it has none to
//! report. X11 builds a window's _NET_WM_ICON property out of exactly that
//! list, so a scalable icon leaves a Linux window with no icon in its corner
//! and none in the task bar, while the same binary shows one on Windows, which
//! takes its icon from the executable instead. Measured: availableSizes() is
//! empty for the SVG and holds one entry for the PNG that preceded it.
//!
//! Rendering the usual sizes from the SVG gives the list something to contain
//! and costs nothing in quality: each pixmap comes from the vector, not from
//! resampling a neighbour.
template <typename Source>
QIcon iconAtUsualSizes(const Source& source)
{
    const QIcon scalable(source);
    QIcon icon;
    for (int size : usualIconSizes) {
        icon.addPixmap(scalable.pixmap(QSize(size, size)));
    }
    return icon;
}
} // namespace

// titleAddText needs to be const char* for tr()
NetworkStyle::NetworkStyle(const QString &_appName, const int iconColorHueShift, const int iconColorSaturationReduction, const char *_titleAddText):
    appName(_appName),
    titleAddText(qApp->translate("SplashScreen", _titleAddText)),
    badgeColor(QColor(0, 141, 228)) // default badge color is the original Dash's blue, regardless of the current theme
{
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(appName);

    // The logo is vector artwork, so nothing here is tied to one pixel size:
    // every pixmap below is rendered from the SVG rather than resampled from a
    // raster made for some other size. It is loaded through the resource
    // system, not the QtSvg API, so this compiles the same whether Qt is the
    // static one from depends or a shared system Qt -- only the plugins
    // differ, and those are imported in bitcoin.cpp.
    const QString logoResource(":/images/defcon_logo");

    if (iconColorHueShift != 0 && iconColorSaturationReduction != 0) {
        // A test network's icon is the same artwork with its colours rotated,
        // and rotating colours needs pixels. Render once, large enough that
        // every size the window and tray ask for is a reduction.
        QPixmap appIconPixmap = QIcon(logoResource).pixmap(QSize(1024, 1024));
        QImage appIconImg = appIconPixmap.toImage();
        rotateColors(appIconImg, iconColorHueShift, iconColorSaturationReduction);
        appIconPixmap.convertFromImage(appIconImg);
        // tweak badge color
        rotateColor(badgeColor, iconColorHueShift, iconColorSaturationReduction);

        appIcon           = iconAtUsualSizes(appIconPixmap);
        trayAndWindowIcon = appIcon;
    } else {
        appIcon           = iconAtUsualSizes(logoResource);
        trayAndWindowIcon = appIcon;
    }

    splashImage = QPixmap(logoResource);
}

const NetworkStyle* NetworkStyle::instantiate(const std::string& networkId)
{
    std::string titleAddText = networkId == CBaseChainParams::MAIN ? "" : strprintf("[%s]", networkId);
    for (const auto& network_style : network_styles)
    {
        if (networkId == network_style.networkId)
        {
            std::string appName = network_style.appName;
            std::string titleAddText = network_style.titleAddText;

            if (networkId == CBaseChainParams::DEVNET.c_str()) {
                appName = strprintf(appName, gArgs.GetDevNetName());
                titleAddText = strprintf(titleAddText, gArgs.GetDevNetName());
            }

            return new NetworkStyle(
                    appName.c_str(),
                    network_style.iconColorHueShift,
                    network_style.iconColorSaturationReduction,
                    titleAddText.c_str());
        }
    }
    return nullptr;
}
