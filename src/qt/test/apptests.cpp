// Copyright (c) 2018-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/apptests.h>

#include <chainparams.h>
#include <key.h>
#include <qt/bitcoin.h>
#include <qt/bitcoingui.h>
#include <qt/starfieldwidget.h>
#include <qt/guiutil.h>
#include <qt/networkstyle.h>
#include <qt/rpcconsole.h>
#include <shutdown.h>
#include <test/util/setup_common.h>
#include <validation.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QTimer>
#include <QButtonGroup>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QtGlobal>
#include <QtTest/QtTestWidgets>
#include <QtTest/QtTestGui>

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {
//! Regex find a string group inside of the console output
QString FindInConsole(const QString& output, const QString& pattern)
{
    const QRegularExpression re(pattern);
    return re.match(output).captured(1);
}

//! Call getblockchaininfo RPC and check first field of JSON output.
void TestRpcCommand(RPCConsole* console)
{
    QTextEdit* messagesWidget = console->findChild<QTextEdit*>("messagesWidget");
    QLineEdit* lineEdit = console->findChild<QLineEdit*>("lineEdit");
    QSignalSpy mw_spy(messagesWidget, &QTextEdit::textChanged);
    QVERIFY(mw_spy.isValid());
    QTest::keyClicks(lineEdit, "getblockchaininfo");
    QTest::keyClick(lineEdit, Qt::Key_Return);
    QVERIFY(mw_spy.wait(1000));
    QCOMPARE(mw_spy.count(), 4);
    const QString output = messagesWidget->toPlainText();
    const QString pattern = QStringLiteral("\"chain\": \"(\\w+)\"");
    QCOMPARE(FindInConsole(output, pattern), QString("regtest"));
}

#ifdef ENABLE_WALLET
//! Check the Abyss sidebar: the wordmark is centred, and each page's glyph is
//! its artwork, in both button states. An SVG that is missing or cannot be
//! decoded draws nothing without a word, so a glyph merely existing proves
//! little; and two pages once shared one.
void TestAbyssSidebar(BitcoinGUI* window)
{
    struct RestoreTheme {
        QVariant theme{QSettings().value("theme")};
        ~RestoreTheme()
        {
            if (theme.isValid()) QSettings().setValue("theme", theme); else QSettings().remove("theme");
            GUIUtil::loadTheme(true);
        }
    } restore;
    // The wallet's main() turns high-DPI pixmaps on and this binary does not.
    // Without them an icon is handed back at its logical size, scaled down
    // from what was drawn for the screen, which is not what the wallet shows.
    struct HighDpiPixmaps {
        const bool was{QCoreApplication::testAttribute(Qt::AA_UseHighDpiPixmaps)};
        HighDpiPixmaps() { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps); }
        ~HighDpiPixmaps() { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, was); }
    } high_dpi_pixmaps;
    const auto theme = [](const char* name) {
        QSettings().setValue("theme", name);
        GUIUtil::loadTheme(true);
        QApplication::processEvents();
    };

    auto* bar = window->findChild<QToolBar*>("navigationToolBar");
    QVERIFY(bar);
    QButtonGroup* group = nullptr;
    for (QToolButton* button : bar->findChildren<QToolButton*>()) {
        if (button->group()) group = button->group();
    }
    QVERIFY(group);
    const QList<QAbstractButton*> buttons = group->buttons();

    // In the order createToolBars adds the pages; the last three are optional.
    QStringList expected{"home", "send", "qrcode", "history", "arrows-shuffle"};
    QSettings settings;
    if (settings.value("fShowMultisigTab", false).toBool()) expected << "key";
    if (settings.value("fShowMasternodesTab").toBool()) expected << "topology-star-3";
    if (settings.value("fShowGovernanceTab").toBool()) expected << "gavel";
    QCOMPARE(buttons.size(), expected.size());
    // appTests() turns every optional page on for a screenshot run.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) QCOMPARE(expected.size(), 8);

    theme("Abyss");
    if (!output.isEmpty()) {
        // The size AbyssTests captures at. A shorter window folds the last
        // pages into the toolbar's overflow menu, out of the picture.
        window->resize(1280, 850);
        QApplication::processEvents();
        QVERIFY(QDir().mkpath(output));
        QVERIFY(bar->grab().save(QDir(output).filePath("Abyss-navigation.png")));
    }

    // The wordmark sits in the middle of the sidebar. Measured on what the bar
    // draws rather than on the label's geometry: the image is painted inside
    // the label, so where the label sits says little about where the ink is.
    auto* logo = bar->findChild<QLabel*>("lblToolbarLogo");
    QVERIFY(logo);
    const QImage shot = bar->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const qreal dpr = shot.devicePixelRatio();
    // Only the rows the image is drawn in. The whole label reaches down to the
    // checked button below it, whose fill would read as a centred wordmark.
    const QRect canvas = logo->contentsRect().translated(logo->pos());
    const QRect band(QPoint(0, qRound(canvas.top() * dpr)), QSize(shot.width(), qRound(canvas.height() * dpr)));
    // The wordmark is blue on a near-black sidebar; the label's top-left
    // corner is margin, so it shows the sidebar itself.
    const int backdrop = qBlue(shot.pixel(qRound(logo->x() * dpr) + 1, band.top() + 1));
    int left = shot.width();
    int right = -1;
    for (int y = band.top(); y <= band.bottom(); ++y) {
        for (int x = 0; x < shot.width(); ++x) {
            if (qBlue(shot.pixel(x, y)) - backdrop > 60) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    QVERIFY2(right >= left, "the wordmark draws nothing");
    const int gap_left = qRound(left / dpr);
    const int gap_right = bar->width() - qRound((right + 1) / dpr);
    QVERIFY2(std::abs(gap_left - gap_right) <= 2,
             qPrintable(QString("wordmark off centre: %1 px left, %2 px right").arg(gap_left).arg(gap_right)));

    // Each glyph is drawn for the screen's pixel ratio and shown one to one in
    // the 22 px the button lays out. So it is compared with its artwork drawn
    // at that same pixel size, not with a copy scaled down to the layout size.
    QVERIFY(window->windowHandle());
    const QSize logical(22, 22);
    const qreal ratio = qApp->devicePixelRatio();
    const QSize physical = logical * ratio;
    for (int i = 0; i < buttons.size(); ++i) {
        QCOMPARE(buttons[i]->iconSize(), logical);
        QImage art(physical, QImage::Format_ARGB32_Premultiplied);
        art.fill(Qt::transparent);
        QPainter painter(&art);
        QIcon(":/icons/tabler/" + expected[i] + ".svg").paint(&painter, art.rect());
        painter.end();

        for (const QIcon::State state : {QIcon::Off, QIcon::On}) {
            const QString what = expected[i] + (state == QIcon::On ? " (checked)" : "");
            const QColor tint = state == QIcon::On ? QColor(Qt::white) : GUIUtil::getThemedQColor(GUIUtil::ThemedColor::BLUE);
            // What the button paints, asked for the way its style asks.
            const QPixmap shown = buttons[i]->icon().pixmap(window->windowHandle(), logical, QIcon::Normal, state);
            QCOMPARE(shown.size(), physical);
            QCOMPARE(shown.devicePixelRatio(), ratio);
            const QImage drawn = shown.toImage().convertToFormat(QImage::Format_ARGB32);
            int ink = 0;
            int worst = 0;
            QRgb densest = 0;
            for (int y = 0; y < physical.height(); ++y) {
                for (int x = 0; x < physical.width(); ++x) {
                    const QRgb px = drawn.pixel(x, y);
                    worst = std::max(worst, std::abs(qAlpha(px) - qAlpha(art.pixel(x, y))));
                    if (qAlpha(px) > 0) ++ink;
                    if (qAlpha(px) > qAlpha(densest)) densest = px;
                }
            }
            QVERIFY2(ink > 20, qPrintable("navigation glyph is empty: " + what));
            QVERIFY2(worst <= 2, qPrintable("navigation glyph is not its artwork: " + what));
            QVERIFY2(std::abs(qRed(densest) - tint.red()) <= 3 && std::abs(qGreen(densest) - tint.green()) <= 3 &&
                         std::abs(qBlue(densest) - tint.blue()) <= 3,
                     qPrintable("navigation glyph has the wrong tint: " + what));
        }
    }

    // Exercise the actual BitcoinGUI theme route, not only the sky setter.
    auto* sky = qobject_cast<StarfieldWidget*>(window->centralWidget());
    QVERIFY(sky);
    const auto sky_timers = sky->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
    QCOMPARE(sky_timers.size(), 1);
    QApplication::setActiveWindow(window);
    QApplication::processEvents();
    QVERIFY(sky_timers.front()->isActive());
    if (!output.isEmpty()) QVERIFY(sky->grab().save(QDir(output).filePath("Abyss-central-sky.png")));
    if (!output.isEmpty()) QVERIFY(window->grab().save(QDir(output).filePath("Abyss-window.png")));
    for (const char* classic : {"Light", "Dark", "Traditional"}) {
        theme(classic);
        QVERIFY(!sky_timers.front()->isActive());
        for (QAbstractButton* button : buttons) QVERIFY(button->icon().isNull());
        theme("Abyss");
        QApplication::setActiveWindow(window);
        QApplication::processEvents();
        QVERIFY(sky_timers.front()->isActive());
    }

    // Leaving Abyss takes the glyphs away again: the classic bars are text only.
    theme("Light");
    for (QAbstractButton* button : buttons) {
        QVERIFY2(button->icon().isNull(), qPrintable(button->text()));
    }
}
#endif // ENABLE_WALLET
} // namespace

//! Entry point for BitcoinApplication tests.
void AppTests::appTests()
{
    QVERIFY(GUIUtil::isValidTheme("Abyss"));
    // Nebula and Arctic were removed, along with the former names of all three
    // modern themes. None of them may be selectable any more.
    QVERIFY(!GUIUtil::isValidTheme("Nebula"));
    QVERIFY(!GUIUtil::isValidTheme("Arctic"));
    QVERIFY(!GUIUtil::isValidTheme("DeFCon Dark"));
    QVERIFY(!GUIUtil::isValidTheme("DeFCon Galaxy"));
    QVERIFY(!GUIUtil::isValidTheme("DeFCon Light"));

    // What matters more than the names is where a wallet that still has one
    // stored ends up. Falling back to the default would hand a white wallet to
    // someone who chose a dark one, silently, on the next start -- so each
    // removed theme resolves to a survivor of its own kind instead.
    {
        QSettings settings;
        const QVariant saved = settings.value("theme");
        const auto resolves = [&settings](const char* stored) {
            settings.setValue("theme", QString(stored));
            return GUIUtil::getActiveTheme();
        };
        QCOMPARE(resolves("Nebula"), QString("Abyss"));
        QCOMPARE(resolves("DeFCon Galaxy"), QString("Abyss"));
        QCOMPARE(resolves("DeFCon Dark"), QString("Abyss"));
        QCOMPARE(resolves("Arctic"), QString("Light"));
        QCOMPARE(resolves("DeFCon Light"), QString("Light"));
        // A name nobody ever shipped still falls back to the default, as before.
        QCOMPARE(resolves("NoSuchTheme"), GUIUtil::getDefaultTheme());
        if (saved.isValid()) {
            settings.setValue("theme", saved);
        } else {
            settings.remove("theme");
        }
    }

    // The stylesheets follow the themes: the survivors are in the resources and
    // the removed ones are gone. The two assertions here previously named
    // ":/css/DeFCon Galaxy" and ":/css/DeFCon Light", aliases the qrc stopped
    // carrying when the themes were renamed -- so this check had been failing
    // on every build since, unnoticed, which is how a resource can go missing
    // and a test still be described as covering it.
    // Which themes are dark. The window frame follows this and not the desktop,
    // so a wallet and the bar above it can never contradict each other. The
    // platform call that acts on the answer is Windows-only and is not even
    // compiled here -- which is the reason the answer lives apart from it, and
    // the reason this much can still be measured.
    {
        QSettings settings;
        const QVariant saved = settings.value("theme");
        const auto isDark = [&settings](const char* theme) {
            settings.setValue("theme", QString(theme));
            return GUIUtil::themeIsDark();
        };
        QVERIFY(isDark("Abyss"));
        QVERIFY(isDark("Dark"));
        QVERIFY(!isDark("Light"));
        QVERIFY(!isDark("Traditional"));
        // A removed theme is resolved before it is judged, so it inherits the
        // brightness of the survivor it maps to rather than the default.
        QVERIFY(isDark("Nebula"));
        QVERIFY(!isDark("Arctic"));
        // And a name nobody shipped follows the default, which is light.
        QVERIFY(!isDark("NoSuchTheme"));
        if (saved.isValid()) {
            settings.setValue("theme", saved);
        } else {
            settings.remove("theme");
        }
    }

    QVERIFY(QFile(":/css/Abyss").exists());
    QVERIFY(QFile(":/css/Light").exists());
    QVERIFY(!QFile(":/css/Nebula").exists());
    QVERIFY(!QFile(":/css/Arctic").exists());
    // Properties Qt Style Sheets do not implement. Qt drops them in silence, so
    // a rule written with one reads correctly in the file and does nothing on
    // screen. Abyss shipped text-transform and letter-spacing on the devnet
    // badge for exactly that reason, and a change before it shipped opacity on
    // a button. Neither was noticed by reading; a list is cheaper.
    {
        const QStringList unsupported{"text-transform", "letter-spacing", "box-shadow",
                                      "text-shadow", "transition", "z-index",
                                      "opacity", "float", "overflow"};
        QStringList sheets{"general"};
        for (const QString& theme : GUIUtil::listThemes()) {
            sheets << theme;
        }
        for (const QString& sheet : sheets) {
            QFile file(":/css/" + sheet);
            QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                     qPrintable("stylesheet resource missing: " + sheet));
            const QString css = QString::fromUtf8(file.readAll());
            for (const QString& property : unsupported) {
                // Anchored where a property may begin, so the word inside
                // "subcontrol-position" or a sentence in a comment is not a hit.
                const QRegularExpression re("(^|[{;])\\s*" + property + "\\s*:",
                                            QRegularExpression::MultilineOption);
                QVERIFY2(!re.match(css).hasMatch(),
                         qPrintable(sheet + " uses " + property + ", which Qt ignores"));
            }
        }
    }

    QCOMPARE(GUIUtil::getDefaultTheme(), QString("Light"));

#ifdef Q_OS_MAC
    if (QApplication::platformName() == "minimal") {
        // Disable for mac on "minimal" platform to avoid crashes inside the Qt
        // framework when it tries to look up unimplemented cocoa functions,
        // and fails to handle returned nulls
        // (https://bugreports.qt.io/browse/QTBUG-49686).
        QWARN("Skipping AppTests on mac build with 'minimal' platform set due to Qt bugs. To run AppTests, invoke "
              "with 'QT_QPA_PLATFORM=cocoa test_dash-qt' on mac, or else use a linux or windows build.");
        return;
    }
#endif

    fs::create_directories([] {
        BasicTestingSetup test{CBaseChainParams::REGTEST}; // Create a temp data directory to backup the gui settings to
        return gArgs.GetDataDirNet() / "blocks";
    }());

    qRegisterMetaType<interfaces::BlockAndHeaderTipInfo>("interfaces::BlockAndHeaderTipInfo");
    m_app.parameterSetup();
    GUIUtil::loadFonts();
    m_app.createOptionsModel(true /* reset settings */);
    QScopedPointer<const NetworkStyle> style(
        NetworkStyle::instantiate(Params().NetworkIDString()));
    // A screenshot run shows every page the sidebar can carry, so the optional
    // glyphs are checked and captured too. After the network style, which
    // renames the application and with it the settings the window reads. Put
    // back afterwards: later suites capture the options dialog, which shows
    // these switches.
    std::vector<std::pair<QString, QVariant>> optional_tabs;
    if (!qEnvironmentVariable("DEFCON_QT_SCREENSHOTS").isEmpty()) {
        QSettings settings;
        for (const char* tab : {"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab"}) {
            optional_tabs.emplace_back(tab, settings.value(tab));
            settings.setValue(tab, true);
        }
    }
    m_app.createWindow(style.data());
    connect(&m_app, &BitcoinApplication::windowShown, this, &AppTests::guiTests);
    expectCallback("guiTests");
    QSettings().setValue("fAppearanceSetupDone", true); // skip appearance setup
    m_app.baseInitialize();
    m_app.requestInitialize();
    m_app.exec();
    m_app.requestShutdown();
    m_app.exec();

    for (const auto& [tab, value] : optional_tabs) {
        if (value.isValid()) QSettings().setValue(tab, value); else QSettings().remove(tab);
    }
    // Reset global state to avoid interfering with later tests.
    LogInstance().DisconnectTestLogger();
    AbortShutdown();
}

//! Entry point for BitcoinGUI tests.
void AppTests::guiTests(BitcoinGUI* window)
{
    HandleCallback callback{"guiTests", *this};
#ifdef ENABLE_WALLET
    TestAbyssSidebar(window);
#endif
    connect(window, &BitcoinGUI::consoleShown, this, &AppTests::consoleTests);
    expectCallback("consoleTests");
    QAction* action = window->findChild<QAction*>("openRPCConsoleAction");
    action->activate(QAction::Trigger);
}

//! Entry point for RPCConsole tests.
void AppTests::consoleTests(RPCConsole* console)
{
    HandleCallback callback{"consoleTests", *this};
    TestRpcCommand(console);
}

//! Destructor to shut down after the last expected callback completes.
AppTests::HandleCallback::~HandleCallback()
{
    auto& callbacks = m_app_tests.m_callbacks;
    auto it = callbacks.find(m_callback);
    assert(it != callbacks.end());
    callbacks.erase(it);
    if (callbacks.empty()) {
        m_app_tests.m_app.quit();
    }
}
