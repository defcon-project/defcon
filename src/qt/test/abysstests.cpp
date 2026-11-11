// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.
#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif
#include <qt/test/abysstests.h>
#include <qt/abyssmenus.h>
#include <qt/abysstoast.h>
#include <qt/abyssunits.h>
#include <qt/editaddressdialog.h>
#include <qt/receiverequestdialog.h>
#include <qt/transactiontablemodel.h>
#include <interfaces/node.h>
#include <script/descriptor.h>
#include <util/strencodings.h>
#include <interfaces/wallet.h>
#include <node/interface_ui.h>
#include <chainparams.h>
#include <key_io.h>
#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/appearancewidget.h>
#include <qt/askpassphrasedialog.h>
#include <qt/bitcoinunits.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoingui.h>
#include <qt/guiconstants.h>
#include <qt/modaloverlay.h>
#include <qt/networkstyle.h>
#include <qt/sendcoinsentry.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/coincontroltreewidget.h>
#include <qt/emptystatewidget.h>
#include <qt/guiutil.h>
#include <qt/openuridialog.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/skycatalogue.h>
#include <qt/starfieldwidget.h>
#include <qt/qrimagewidget.h>
#include <qt/qrdialog.h>
#include <qt/transactionview.h>
#include <qt/masternodelist.h>
#include <qt/receivecoinsdialog.h>
#include <qt/recentrequeststablemodel.h>
#include <qt/rpcconsole.h>
#include <qt/sendcoinsdialog.h>
#include <qt/signverifymessagedialog.h>
#include <qt/utilitydialog.h>
#include <qt/walletmodel.h>
#include <qt/walletframe.h>
#include <qt/walletcontroller.h>
#include <qt/walletview.h>
#include <pos/minter.h>
#include <pos/multiwallet.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <util/settings.h>
#include <util/system.h>
#include <univalue.h>
#include <validation.h>
#include <util/check.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>

#ifndef WIN32
#include <sys/resource.h>
#endif
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSet>
#include <QStringList>
#include <QAction>
#include <QSignalSpy>
#include <QToolButton>
#include <QToolBar>
#include <QTranslator>
#include <QCursor>
#include <QThread>
#include <QWidgetAction>
#include <QWindow>
#if defined(Q_OS_LINUX)
#include <time.h>
#endif
#include <QCalendarWidget>
#include <QDateTimeEdit>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QLineEdit>
#include <QSettings>
#include <QShortcut>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QStyleOptionButton>
#include <QStatusBar>
#include <QPainter>
#include <QPointer>
#include <QTableView>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTest>
#include <QTextStream>
#include <QTextDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolTip>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>
#ifdef USE_QRCODE
#include <qrencode.h>
#endif

namespace {
// All model state is disposable; the original node context is restored even
// when a QVERIFY returns early. No real wallet or on-disk chain is opened.
struct WalletFixture {
    TestChain100Setup chain;
    interfaces::Node& node;
    NodeContext* previous;
    std::shared_ptr<CWallet> wallet;
    std::unique_ptr<OptionsModel> options;
    std::unique_ptr<ClientModel> client;
    std::unique_ptr<WalletModel> model;
    //! The mock Berkeley database cannot be encrypted (its rewrite reloads an
    //! environment with no path); a wallet that will be, takes the dummy one.
    explicit WalletFixture(interfaces::Node& n, bool dummy_database = false, uint64_t wallet_flags = 0) : node(n), previous(n.context())
    {
        node.setContext(&chain.m_node);
        wallet = std::make_shared<CWallet>(node.context()->chain.get(), node.context()->coinjoin_loader.get(), "",
                                           dummy_database ? CreateDummyWalletDatabase() : CreateMockWalletDatabase());
        AddWallet(wallet);
        // The dummy database's cursor never reports its end: LoadWallet() would
        // not return. There is nothing in it to load.
        if (!dummy_database) wallet->LoadWallet();
        if (wallet_flags) wallet->SetWalletFlag(wallet_flags);
        wallet->GetOrCreateLegacyScriptPubKeyMan();
        options = std::make_unique<OptionsModel>();
        options->setNode(node);
        client = std::make_unique<ClientModel>(node, options.get());
        model = std::make_unique<WalletModel>(interfaces::MakeWallet(wallet), *client);
    }
    ~WalletFixture()
    {
        model.reset(); client.reset(); options.reset();
        RemoveWallet(wallet, std::nullopt); wallet.reset();
        node.setContext(previous);
    }
};
struct RestoreAppearance {
    QVariant theme{QSettings().value("theme")};
    GUIUtil::FontFamily family{GUIUtil::getFontFamily()};
    bool explicit_family{GUIUtil::hasExplicitFontFamily()};
    int scale{GUIUtil::getFontScale()};
    ~RestoreAppearance()
    {
        if (theme.isValid()) QSettings().setValue("theme", theme); else QSettings().remove("theme");
        GUIUtil::setFontFamily(family, explicit_family);
        GUIUtil::setFontScale(scale);
        GUIUtil::loadTheme(true);
    }
};
void Theme(const char* theme)
{
    QSettings().setValue("theme", theme);
    GUIUtil::loadTheme(true);
    QApplication::processEvents();
}
void Settle()
{
    // Deliver coalesced font/layout callbacks and the resulting layout requests.
    for (int i = 0; i < 8; ++i) QApplication::processEvents();
}
QRect OnPage(QWidget& page, QWidget* child)
{
    return {child->mapTo(&page, {}), child->size()};
}
//! The text with thin spaces where the test writes plain ones, as amounts group
//! their thousands.
QString Thin(const char* text) { return QString::fromUtf8(text).replace(QLatin1Char(' '), QChar(THIN_SP_CP)); }
// A translation with three plural forms, as Russian has them: 1 and 21 take
// the first, 2 to 4 the second, 5 to 20 the third. Only the network bar's age
// strings are translated; everything else falls through untranslated.
class RussianPlurals : public QTranslator
{
public:
    using QTranslator::QTranslator;
    static int form(int n)
    {
        if (n % 10 == 1 && n % 100 != 11) return 0;
        if (n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 10 || n % 100 >= 20)) return 1;
        return 2;
    }
    QString translate(const char* context, const char* source, const char* disambiguation, int n) const override
    {
        Q_UNUSED(disambiguation);
        if (qstrcmp(context, "OverviewPage") != 0) return QString();
        static const std::map<QByteArray, std::array<const char*, 3>> plurals{
            {"%n minutes", {"%n минута", "%n минуты", "%n минут"}},
            {"%n hours", {"%n час", "%n часа", "%n часов"}},
            {"%n days", {"%n день", "%n дня", "%n дней"}},
            {"masternodes", {"мастернода", "мастерноды", "мастернод"}},
        };
        if (qstrcmp(source, "masternode") == 0) return QString::fromUtf8("мастернода");
        if (qstrcmp(source, "1 minute") == 0) return QString::fromUtf8("1 минута");
        if (qstrcmp(source, "%1 ago") == 0) return QString::fromUtf8("%1 назад");
        const auto it = plurals.find(source);
        if (it == plurals.end() || n < 0) return QString();
        return QString::fromUtf8(it->second[form(n)]);
    }
    bool isEmpty() const override { return false; }
};
// Minimal has no window-system mouse. Exercise the actual QSS paint path with
// explicit QStyle states instead of pretending a synthetic enter moved a cursor.
class StateButton : public QPushButton {
public:
    using QPushButton::QPushButton;
    QImage paintedState(int state)
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.state.setFlag(QStyle::State_MouseOver, state == 1);
        option.state.setFlag(QStyle::State_Sunken, state == 2);
        option.state.setFlag(QStyle::State_Enabled, state != 3);
        QImage image(size(), QImage::Format_RGB32);
        image.fill(QColor("#0b1727"));
        QPainter painter(&image);
        style()->drawControl(QStyle::CE_PushButton, &option, &painter, this);
        return image;
    }
};
bool AmountFits(QLabel* label)
{
    const int padding = 2 * label->margin() + std::max(0, label->indent());
    return QFontMetricsF(label->font()).horizontalAdvance(label->text()) <= label->contentsRect().width() - padding;
}
//! Closes every open popup, the innermost first.
void CloseMenus()
{
    for (int i = 0; i < 16 && QApplication::activePopupWidget(); ++i) QApplication::activePopupWidget()->close();
}
QPoint Centre(const QWidget* widget) { return widget->mapToGlobal(widget->rect().center()); }
//! Moves the pointer the way the window system does (QTest's QWindow path):
//! enter and leave between widgets, every move to the open popup while one is.
//! It arrives with a one-pixel step, as a real pointer does: a menu ignores the
//! first move after its Enter (Qt 5.15 QMenu::enterEvent sets motions to -1).
void PointAt(QWidget* widget, const QPoint& global)
{
    QWindow* handle = widget->window()->windowHandle();
    QTest::mouseMove(handle, handle->mapFromGlobal(global + QPoint(1, 0)));
    QTest::mouseMove(handle, handle->mapFromGlobal(global));
}
//! Steps a test through the event loops of open menus. A menu opened by
//! QToolButton::showMenu() runs its own exec(), so the test cannot wait in its
//! own code for what happens while that menu is open. Each step waits until
//! it is ready, then acts, from a ticking timer that runs in whatever loop is
//! current; a step's action must not block (one that opens a menu with exec()
//! goes through post()). A step not ready within its time fails the run, and
//! every popup is closed then, so a failure never leaves an exec() running.
class MenuScript
{
public:
    using Ready = std::function<bool(qint64 ms_in_step)>;
    void step(const QString& name, Ready ready, std::function<void()> act = {}, int timeout_ms = 5000)
    {
        m_steps.push_back({name, std::move(ready), std::move(act), timeout_ms});
    }
    void then(const QString& name, std::function<void()> act) { step(name, [](qint64) { return true; }, std::move(act)); }
    void until(const QString& name, std::function<bool()> condition, int timeout_ms = 5000)
    {
        step(name, [condition = std::move(condition)](qint64) { return condition(); }, {}, timeout_ms);
    }
    void pause(const QString& name, int ms) { step(name, [ms](qint64 elapsed) { return elapsed >= ms; }, {}, ms + 5000); }
    static void post(std::function<void()> act) { QTimer::singleShot(0, std::move(act)); }
    //! What a failed step reports besides its name (the state it was waiting on).
    void describe(std::function<QString()> state) { m_describe = std::move(state); }
    //! Runs the steps; returns the failure, empty when every step was reached.
    QString run()
    {
        QEventLoop loop;
        QTimer ticker;
        ticker.setInterval(10);
        size_t index = 0;
        QElapsedTimer clock;
        clock.start();
        QString failure;
        bool done = false;
        const auto finish = [&](const QString& why) {
            if (done) return;
            done = true;
            failure = why;
            ticker.stop();
            CloseMenus();
            loop.quit();
        };
        // A step's action blocked in a nested loop (a menu's exec() it should
        // not have run) stalls the ticker, whose slot cannot run again while it
        // is on the stack; the watchdog, another timer, still fires there.
        bool in_action = false;
        const auto tick = [&] {
            if (done) return;
            if (in_action) {
                if (clock.elapsed() > 10000) finish(QString("step %1 \"%2\": its action did not return").arg(index - 1).arg(m_steps[index - 1].name));
                return;
            }
            if (index >= m_steps.size()) return finish(QString());
            Step& current = m_steps[index];
            if (current.ready(clock.elapsed())) {
                ++index;
                clock.restart();
                if (current.act) {
                    in_action = true;
                    current.act();
                    in_action = false;
                }
            } else if (clock.elapsed() > current.timeout_ms) {
                finish(QString("step %1 \"%2\" not reached in %3 ms%4").arg(index).arg(current.name).arg(current.timeout_ms)
                           .arg(m_describe ? "; " + m_describe() : QString()));
            }
        };
        QTimer watchdog;
        watchdog.setInterval(100);
        QObject::connect(&ticker, &QTimer::timeout, tick);
        QObject::connect(&watchdog, &QTimer::timeout, tick);
        ticker.start();
        watchdog.start();
        loop.exec();
        watchdog.stop();
        m_steps.clear();
        return failure;
    }

private:
    struct Step {
        QString name;
        Ready ready;
        std::function<void()> act;
        int timeout_ms;
    };
    std::vector<Step> m_steps;
    std::function<QString()> m_describe;
};
//! The pointer's state as Qt sees it, for a failed step's report.
QString PointerState(QWidget* window)
{
    const QPoint pointer = QCursor::pos();
    QWidget* under = QApplication::widgetAt(pointer);
    QWidget* popup = QApplication::activePopupWidget();
    QWidget* active = QApplication::activeWindow();
    const auto name = [](QWidget* w) -> QString { return w ? w->metaObject()->className() + QString("#") + w->objectName() : QString("none"); };
    return QString("pointer %1,%2 under %3 popup %4 active %5 (%6) tracking %7")
        .arg(pointer.x()).arg(pointer.y())
        .arg(name(under), name(popup))
        .arg(window->isActiveWindow() ? 1 : 0)
        .arg(name(active))
        .arg(popup && popup->hasMouseTracking() ? 1 : 0);
}
} // namespace

void AbyssTests::initTestCase()
{
    if (!GUIUtil::fontsLoaded()) GUIUtil::loadFonts();
}

void AbyssTests::init()
{
    // OptionsModel soft arguments and GUI preferences survive earlier cases
    // and suites. Isolate each case and retain all original assertions.
    util::Settings old_args;
    gArgs.LockSettings([&](util::Settings& args) { old_args = args; });
    const QString app_name = QCoreApplication::applicationName();
    auto settings = std::make_shared<QSettings>();
    settings->setFallbacksEnabled(false);
    std::map<QString, QVariant> old_preferences;
    for (const QString& key : settings->allKeys()) {
        old_preferences.emplace(key, settings->value(key));
    }
    m_restore_test_settings = [old_args = std::move(old_args), old_preferences, settings, app_name]() mutable {
        QCoreApplication::setApplicationName(app_name);
        gArgs.LockSettings([&](util::Settings& args) { args = std::move(old_args); });
        settings->clear();
        for (const auto& entry : old_preferences) settings->setValue(entry.first, entry.second);
    };
    // This suite does not test weight CLI overrides. Let each OptionsModel
    // normalize the stored weights for the family's actual supported weights.
    gArgs.ForceRemoveArg("font-weight-normal");
    gArgs.ForceRemoveArg("font-weight-bold");
}

void AbyssTests::cleanup()
{
    // QTest calls cleanup even after a QVERIFY/QSKIP returns from a case.
    auto restore = std::move(m_restore_test_settings);
    if (restore) restore();
}

void AbyssTests::qrPixels_data()
{
    QTest::addColumn<QString>("theme");
    QTest::addColumn<QString>("payload");
    for (const char* theme : {"Abyss", "Light", "Dark", "Traditional"}) {
        QTest::newRow((QString(theme) + "-address").toUtf8()) << QString(theme) << QString("defcon:XtestAddress123456789");
        QTest::newRow((QString(theme) + "-payment").toUtf8()) << QString(theme) << QString("defcon:XtestAddress123456789?amount=1234.56789&label=Regression");
        QTest::newRow((QString(theme) + "-long").toUtf8()) << QString(theme) << QString(900, 'A');
    }
}

void AbyssTests::qrPixels()
{
#ifdef USE_QRCODE
    RestoreAppearance restore;
    QFETCH(QString, theme);
    QFETCH(QString, payload);
    Theme(theme.toUtf8().constData());
    QRImageWidget widget;
    QVERIFY(widget.setQR(payload, "Black caption on white", 1200));
    const QImage image = widget.exportImage();
    QVERIFY(!image.isNull());
    QRcode* code = QRcode_encodeString(payload.toUtf8().constData(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    QVERIFY(code);
    const auto release = interfaces::MakeHandler([code] { QRcode_free(code); });
    qInfo() << "QR physical pixels / DPR" << image.size() << image.devicePixelRatio();
    const int side = image.width();
    const int module = side / (code->width + 8);
    QVERIFY(module >= 1);
    const int offset = (side - code->width * module) / 2;
    QVERIFY(offset >= 4 * module);
    QVERIFY(side - offset - code->width * module >= 4 * module);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const int col = (x - offset) / module, row = (y - offset) / module;
            const bool inside = x >= offset && y >= offset && col < code->width && row < code->width;
            const QRgb expected = inside && (code->data[row * code->width + col] & 1) ? qRgb(0, 0, 0) : qRgb(255, 255, 255);
            if (image.pixel(x, y) != expected) QFAIL(qPrintable(QString("Nonuniform/tinted QR pixel at %1,%2").arg(x).arg(y)));
        }
    }
    widget.copyImage();
    QCOMPARE(QApplication::clipboard()->image().convertToFormat(QImage::Format_RGB32), image.convertToFormat(QImage::Format_RGB32));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filename = directory.filePath("qr.png");
    QVERIFY(image.save(filename));
    // PNG retains physical pixels, not Qt's device-pixel-ratio metadata.
    QImage pixels = image.convertToFormat(QImage::Format_RGB32);
    pixels.setDevicePixelRatio(1);
    QCOMPARE(QImage(filename).convertToFormat(QImage::Format_RGB32), pixels);
#else
    QSKIP("Built without USE_QRCODE");
#endif
}

void AbyssTests::networkStyleLifetime()
{
    // NetworkStyle::instantiate changes the application/settings name. Keep
    // this test in the same isolated namespace as the surrounding suite.
    const QString app_name = QCoreApplication::applicationName();
    const auto restore_name = interfaces::MakeHandler([app_name] { QCoreApplication::setApplicationName(app_name); });
    std::unique_ptr<const NetworkStyle> caller_style(NetworkStyle::instantiate("main"));
    QCoreApplication::setApplicationName(app_name);
    RestoreAppearance restore;
    Theme("Abyss");
    BitcoinGUI window(m_node, caller_style.get());
    const QString title = window.windowTitle();
    const qint64 icon_key = window.windowIcon().cacheKey();

    // AppTests releases its local style while BitcoinApplication still owns
    // the window. A later stylesheet change previously read freed style data.
    caller_style.reset();
    Theme("Light");
    Theme("Abyss");
    QCOMPARE(window.windowTitle(), title);
    QCOMPARE(window.windowIcon().cacheKey(), icon_key);
    QVERIFY(!window.findChild<QLabel*>("devnetBadge"));
}



void AbyssTests::overviewTotalHeight_data()
{
    QTest::addColumn<int>("height");
    QTest::addColumn<bool>("coinjoin_visible");
    QTest::newRow("ample-with-coinjoin") << 850 << true;
    QTest::newRow("compact-without-coinjoin") << 550 << false;
    QTest::newRow("compact-with-coinjoin") << 550 << true;
}

void AbyssTests::overviewTotalHeight()
{
    QFETCH(int, height);
    QFETCH(bool, coinjoin_visible);
    if (!GUIUtil::fontsLoaded()) QSKIP("Vertical text geometry requires a font-capable platform");
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    // Match the default Abyss family seen in the native zero-balance trial.
    GUIUtil::setFontFamily(GUIUtil::FontFamily::Roboto);
    GUIUtil::setFontScale(0);
    OverviewPage page;
    page.setClientModel(fixture.client.get());
    page.setWalletModel(fixture.model.get());
    interfaces::WalletBalances balances{};
    page.setBalance(balances);
    auto* total = page.findChild<QLabel*>("labelTotal");
    auto* coinjoin = page.findChild<QFrame*>("frameCoinJoin");
    QVERIFY(total && coinjoin);
    // Abyss keeps the CoinJoin card away until mixing; the row's link pins it open past the status timer.
    if (coinjoin_visible) page.findChild<QPushButton*>("coinJoinRowShow")->click();
    coinjoin->setVisible(coinjoin_visible);
    page.resize(1100, height);
    page.show(); Settle();
    QVERIFY(total->isVisible());
    QCOMPARE(coinjoin->isVisible(), coinjoin_visible);
    const QFontMetricsF metrics(total->font());
    const QString geometry = QString("page=%1x%2 CoinJoin=%3 total=%4x%5 contents=%6x%7 minimumHint=%8x%9 lineHeight=%10 DPR=%11")
        .arg(page.width()).arg(page.height()).arg(coinjoin_visible)
        .arg(total->width()).arg(total->height())
        .arg(total->contentsRect().width()).arg(total->contentsRect().height())
        .arg(total->minimumSizeHint().width()).arg(total->minimumSizeHint().height())
        .arg(metrics.height()).arg(page.devicePixelRatioF());
    qInfo().noquote() << "OVERVIEW_TOTAL_HEIGHT" << QTest::currentDataTag() << geometry;
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        const QString name = QString("overview-total-height-%1").arg(QTest::currentDataTag());
        QVERIFY(page.grab().save(output + "/" + name + ".png"));
        QVERIFY(total->grab().save(output + "/" + name + "-label.png"));
    }
    // Existing AmountFits checks only width; all three rows pass that check.
    QVERIFY2(AmountFits(total), qPrintable("Unexpected horizontal clipping: " + geometry));
    // Ask the rendered QLabel for the vertical floor of its current text/font
    // and style, rather than hardcoding one platform's point/pixel conversion.
    QVERIFY2(total->height() >= total->minimumSizeHint().height(),
             qPrintable("Total text is vertically clipped: " + geometry));
    auto* normal = page.findChild<QLabel*>("labelBalance");
    QVERIFY(normal);
    QVERIFY(total->font().pointSizeF() >= normal->font().pointSizeF());
    // A shared font refresh must refit a compact total without changing its
    // balance; growing the page must also restore room for the larger font.
    const qreal fitted = total->font().pointSizeF();
    GUIUtil::updateFonts(); Settle();
    QCOMPARE(total->font().pointSizeF(), fitted);
    page.resize(1100, 850); Settle();
    QVERIFY(total->height() >= total->minimumSizeHint().height());
    QVERIFY(total->font().pointSizeF() >= fitted);
    page.resize(1100, height); Settle();
    QVERIFY(total->height() >= total->minimumSizeHint().height());
    QVERIFY(total->font().pointSizeF() >= normal->font().pointSizeF());
}

void AbyssTests::overviewResponsive()
{
    if (!GUIUtil::fontsLoaded()) QSKIP("Font metrics require a font-capable platform; run minimal:enable_fonts");
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setClientModel(fixture.client.get());
    page.setWalletModel(fixture.model.get());
    auto* total = page.findChild<QLabel*>("labelTotal");
    auto* normal = page.findChild<QLabel*>("labelBalance");
    auto* left = page.findChild<QFrame*>("frame");
    auto* right = page.findChild<QFrame*>("frame_2");
    auto* network = page.findChild<QFrame*>("networkCard");
    QVERIFY(total && normal && left && right && network);
    interfaces::WalletBalances balances{};
    balances.balance = 5082097599LL * COIN / 100;
    balances.unconfirmed_balance = 123456789;
    balances.immature_balance = 234567890;
    balances.watch_only_balance = balances.balance;
    for (int unit : BitcoinUnits::availableUnits()) {
        QVERIFY(fixture.options->setData(fixture.options->index(OptionsModel::DisplayUnit), unit));
        for (bool watch : {false, true}) {
            QVERIFY(QMetaObject::invokeMethod(&page, "updateWatchOnlyLabels", Q_ARG(bool, watch)));
            for (bool privacy : {false, true}) {
                page.setBalance(balances);
                page.setPrivacy(privacy);
                page.show();
                for (int width : {0, 1280, 1920, 2560}) {
                    page.resize(width ? width : page.minimumWidth(), 850);
                    Settle();
                    QVERIFY2(AmountFits(total), qPrintable(QString("Total clipped unit=%1 width=%2 watch=%3 privacy=%4").arg(unit).arg(page.width()).arg(watch).arg(privacy)));
                    QVERIFY(total->font().pointSizeF() >= normal->font().pointSizeF());
                    QCOMPARE(OnPage(page, left).left(), OnPage(page, network).left());
                    QCOMPARE(OnPage(page, right).right(), OnPage(page, network).right());
                    QCOMPARE(OnPage(page, right).left() - OnPage(page, left).right() - 1, 12);
                }
            }
        }
    }
    // The staking card sits under the balances one with the same gap the
    // network card leaves above them, and with CoinJoin opened, its card under
    // the staking card with that gap again.
    auto* staking = page.findChild<QFrame*>("frameStaking");
    auto* coinjoin = page.findChild<QFrame*>("frameCoinJoin");
    auto* open_coinjoin = page.findChild<QPushButton*>("coinJoinRowShow");
    QVERIFY(staking && coinjoin && open_coinjoin);
    open_coinjoin->click();
    Settle();
    QVERIFY(coinjoin->isVisible());
    const int gap = OnPage(page, left).top() - OnPage(page, network).bottom() - 1;
    QCOMPARE(OnPage(page, staking).top() - OnPage(page, left).bottom() - 1, gap);
    QCOMPARE(OnPage(page, coinjoin).top() - OnPage(page, staking).bottom() - 1, gap);
    // The balance never changes here: scale, family and bold-weight changes
    // must remeasure/refit the total without another setBalance() call.
    page.setPrivacy(false);
    const QString text = total->text();
    for (auto family : {GUIUtil::FontFamily::SystemDefault, GUIUtil::FontFamily::Montserrat, GUIUtil::FontFamily::Roboto}) {
        GUIUtil::setFontFamily(family);
        const auto previous_weight = GUIUtil::getFontWeightBold();
        const auto restore_weight = interfaces::MakeHandler([previous_weight] {
            GUIUtil::setFontWeightBold(previous_weight);
        });
        const auto weights = GUIUtil::getSupportedWeights();
        QVERIFY(!weights.empty());
        for (auto weight : {weights.front(), weights.back()}) {
            GUIUtil::setFontWeightBold(weight);
            for (int scale : {25, 50, 0}) {
                GUIUtil::setFontScale(scale);
                page.resize(page.minimumWidth(), 850);
                Settle();
                QCOMPARE(total->text(), text);
                QVERIFY(AmountFits(total));
                QCOMPARE(total->font().family(), GUIUtil::getFont(GUIUtil::FontWeight::Bold, false, 26).family());
                QCOMPARE(total->font().weight(), int(GUIUtil::getFontWeightBold()));
                const qreal fitted = total->font().pointSizeF();
                GUIUtil::updateFonts();
                Settle();
                QCOMPARE(total->font().pointSizeF(), fitted);
                // At 2560px a large scaled duffs amount and watch-only column
                // can still require shrinking. Give the cap test enough room
                // and assert that premise, independently of the no-clip cases.
                page.resize(4096, 850);
                Settle();
                const QFont cap = GUIUtil::getFont(GUIUtil::FontWeight::Bold, false, 26);
                QVERIFY(QFontMetricsF(cap).horizontalAdvance(total->text()) <= total->contentsRect().width() - 2 * total->margin());
                QVERIFY(total->height() >= total->minimumSizeHint().height());
                QCOMPARE(total->font().pointSizeF(), cap.pointSizeF());
            }
        }
    }
    // A pixel-sized normal font must yield a usable positive floor as well.
    QFont pixel_font = normal->font();
    pixel_font.setPixelSize(16);
    normal->setFont(pixel_font);
    page.resize(page.minimumWidth(), 850);
    Settle();
    QVERIFY(total->font().pointSizeF() > 0);
    QVERIFY(AmountFits(total));
}

void AbyssTests::themeRestoresLayout()
{
    RestoreAppearance restore;
    Theme("Light");
    OverviewPage page;
    page.show(); Settle();
    auto* row = page.findChild<QHBoxLayout*>("horizontalLayout");
    auto* column = page.findChild<QVBoxLayout*>("verticalLayout_2");
    auto* grid = page.findChild<QGridLayout*>("gridLayout");
    auto* total = page.findChild<QLabel*>("labelTotal");
    QVERIFY(row && column && grid && total);
    const int columnSpacing = column->spacing();
    std::vector<int> stretches;
    std::vector<std::pair<QSize, QSizePolicy>> spacers;
    for (int i = 0; i < row->count(); ++i) {
        stretches.push_back(row->stretch(i));
        if (auto* spacer = row->itemAt(i)->spacerItem()) spacers.emplace_back(spacer->sizeHint(), spacer->sizePolicy());
    }
    const auto policy = total->sizePolicy();
    const int gridStretch = grid->columnStretch(1);
    std::map<QWidget*, int> minimums;
    auto* balances = page.findChild<QFrame*>("frame");
    QVERIFY(balances);
    for (QLabel* child : balances->findChildren<QLabel*>()) minimums.emplace(child, child->minimumWidth());
    minimums.emplace(balances, balances->minimumWidth());
    minimums.emplace(&page, page.minimumWidth());
    for (const char* classic : {"Light", "Dark", "Traditional"}) {
        Theme("Abyss"); Settle();
        QCOMPARE(row->stretch(0), 0);
        QCOMPARE(row->stretch(1), 1);
        Theme(classic); Settle();
        size_t index = 0;
        for (int i = 0; i < row->count(); ++i) {
            QCOMPARE(row->stretch(i), stretches[i]);
            if (auto* spacer = row->itemAt(i)->spacerItem()) {
                QCOMPARE(spacer->sizeHint(), spacers[index].first);
                QCOMPARE(spacer->sizePolicy(), spacers[index++].second);
            }
        }
        QCOMPARE(total->sizePolicy(), policy);
        QCOMPARE(column->spacing(), columnSpacing);
        QCOMPARE(grid->columnStretch(1), gridStretch);
        // Traditional has no general.css floors. Its own fresh-page baseline
        // is compared in themeSwitchFromUnpolished rather than to Light's CSS.
        if (QString(classic) != "Traditional") {
            for (const auto& entry : minimums) QVERIFY2(entry.first->minimumWidth() == entry.second, qPrintable(entry.first->objectName()));
        }
    }
}

void AbyssTests::themeSwitchFromUnpolished_data()
{
    QTest::addColumn<QString>("classic");
    QTest::addColumn<bool>("hidden");
    QTest::addColumn<bool>("without_stylesheet");
    for (const QString& theme : {QString("Light"), QString("Dark"), QString("Traditional")}) {
        QTest::newRow(qPrintable(theme + "-visible")) << theme << false << false;
        QTest::newRow(qPrintable(theme + "-hidden")) << theme << true << false;
        QTest::newRow(qPrintable(theme + "-before-stylesheet")) << theme << true << true;
    }
}

void AbyssTests::themeSwitchFromUnpolished()
{
    if (!GUIUtil::fontsLoaded()) QSKIP("Theme geometry requires minimal:enable_fonts or a native platform");
    QFETCH(QString, classic);
    QFETCH(bool, hidden);
    QFETCH(bool, without_stylesheet);
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    if (without_stylesheet) qApp->setStyleSheet({});
    OverviewPage page;
    page.setClientModel(fixture.client.get());
    page.setWalletModel(fixture.model.get());
    interfaces::WalletBalances balances{};
    balances.balance = 5082097599LL * COIN / 100;
    page.setBalance(balances);
    page.resize(1280, 850);
    if (!hidden) page.show();
    Settle(); // Also drain queued work for pages hidden by a multiwallet stack.
    Theme(classic.toUtf8().constData());
    page.show();
    Settle();

    OverviewPage fresh;
    fresh.setClientModel(fixture.client.get());
    fresh.setWalletModel(fixture.model.get());
    fresh.setBalance(balances);
    fresh.resize(1280, 850);
    fresh.show();
    Settle();
    QCOMPARE(page.minimumWidth(), fresh.minimumWidth());
    for (const char* name : {"frame", "frame_2", "labelBalance", "labelBalanceText", "labelTotal", "labelTotalText"}) {
        auto* actual = page.findChild<QWidget*>(name);
        auto* expected = fresh.findChild<QWidget*>(name);
        QVERIFY(actual && expected);
        QCOMPARE(actual->minimumSize(), expected->minimumSize());
        QCOMPARE(actual->sizePolicy(), expected->sizePolicy());
        QCOMPARE(actual->font(), expected->font());
        if (OnPage(page, actual) != OnPage(fresh, expected)) {
            qInfo() << "CLASSIC_LAYOUT_DIAGNOSTIC" << name << actual->sizeHint() << expected->sizeHint()
                    << actual->contentsMargins() << expected->contentsMargins();
            for (QLabel* label : page.findChild<QFrame*>("frame")->findChildren<QLabel*>()) {
                auto* reference = fresh.findChild<QLabel*>(label->objectName());
                if (reference) qInfo() << label->objectName() << label->font().toString() << reference->font().toString()
                                      << label->sizeHint() << reference->sizeHint() << label->margin() << reference->margin()
                                      << label->contentsMargins() << reference->contentsMargins()
                                      << label->minimumSize() << reference->minimumSize()
                                      << label->maximumSize() << reference->maximumSize()
                                      << label->frameWidth() << reference->frameWidth()
                                      << label->indent() << reference->indent()
                                      << label->textFormat() << reference->textFormat();
            }
        }
        QCOMPARE(OnPage(page, actual), OnPage(fresh, expected));
    }
    if (classic != "Traditional") {
        QCOMPARE(page.findChild<QFrame*>("frame")->minimumWidth(), 490);
        QCOMPARE(page.findChild<QLabel*>("labelBalance")->minimumWidth(), 60);
    }
    auto* row = page.findChild<QHBoxLayout*>("horizontalLayout");
    auto* expected_row = fresh.findChild<QHBoxLayout*>("horizontalLayout");
    auto* grid = page.findChild<QGridLayout*>("gridLayout");
    auto* expected_grid = fresh.findChild<QGridLayout*>("gridLayout");
    QVERIFY(row && expected_row && grid && expected_grid);
    QCOMPARE(row->contentsMargins(), expected_row->contentsMargins());
    QCOMPARE(row->spacing(), expected_row->spacing());
    for (int i = 0; i < row->count(); ++i) {
        QCOMPARE(row->stretch(i), expected_row->stretch(i));
        if (auto* spacer = row->itemAt(i)->spacerItem()) {
            auto* expected = expected_row->itemAt(i)->spacerItem();
            QVERIFY(expected);
            QCOMPARE(spacer->sizeHint(), expected->sizeHint());
            QCOMPARE(spacer->sizePolicy(), expected->sizePolicy());
        }
    }
    QCOMPARE(grid->contentsMargins(), expected_grid->contentsMargins());
    QCOMPARE(grid->spacing(), expected_grid->spacing());
    for (int col = 0; col < grid->columnCount(); ++col) {
        QCOMPARE(grid->columnStretch(col), expected_grid->columnStretch(col));
    }
    auto* spacer = grid->itemAtPosition(2, 3)->spacerItem();
    auto* expected_spacer = expected_grid->itemAtPosition(2, 3)->spacerItem();
    QVERIFY(spacer && expected_spacer);
    QCOMPARE(spacer->sizeHint(), expected_spacer->sizeHint());
    QCOMPARE(spacer->sizePolicy(), expected_spacer->sizePolicy());
    // Resizing a classic page must not reapply a minimum captured in Abyss.
    for (int width : {960, 1920, 1280}) {
        page.resize(width, 850);
        fresh.resize(width, 850);
        Settle();
        QCOMPARE(page.minimumWidth(), fresh.minimumWidth());
        QCOMPARE(OnPage(page, page.findChild<QFrame*>("frame")), OnPage(fresh, fresh.findChild<QFrame*>("frame")));
        QCOMPARE(OnPage(page, page.findChild<QLabel*>("labelTotal")), OnPage(fresh, fresh.findChild<QLabel*>("labelTotal")));
    }
}

void AbyssTests::cliFontDoesNotPersist_data()
{
    QTest::addColumn<QString>("family");
    QTest::addColumn<int>("marker");
    for (const QString& family : {QString("SystemDefault"), QString("Roboto")}) {
        for (int marker : {-1, 0, 1}) {
            QTest::newRow(qPrintable(family + QString("-marker-%1").arg(marker))) << family << marker;
        }
    }
}

void AbyssTests::cliFontDoesNotPersist()
{
    if (!GUIUtil::fontsLoaded()) QSKIP("Font preferences require minimal:enable_fonts or a native platform");
    QFETCH(QString, family);
    QFETCH(int, marker);
    RestoreAppearance restore;
    const bool had_arg = gArgs.IsArgSet("-font-family");
    const std::string old_arg = gArgs.GetArg("-font-family", "");
    QSettings settings;
    const QVariant old_family = settings.value("fontFamily");
    const QVariant old_marker = settings.value("fontFamilyExplicit");
    const auto restore_preferences = interfaces::MakeHandler([&] {
        if (had_arg) gArgs.ForceSetArg("-font-family", old_arg); else gArgs.ForceRemoveArg("font-family");
        for (const auto& entry : {std::make_pair(QString("fontFamily"), old_family), std::make_pair(QString("fontFamilyExplicit"), old_marker)}) {
            if (entry.second.isValid()) settings.setValue(entry.first, entry.second); else settings.remove(entry.first);
        }
    });
    settings.setValue("fontFamily", "SystemDefault");
    if (marker < 0) settings.remove("fontFamilyExplicit"); else settings.setValue("fontFamilyExplicit", bool(marker));
    const QVariant expected_marker = settings.value("fontFamilyExplicit");
    gArgs.ForceSetArg("-font-family", family.toStdString());
    GUIUtil::setFontFamily(GUIUtil::fontFamilyFromString(family));
    OptionsModel options;
    QVERIFY(options.getOverriddenByCommandLine().contains("-font-family"));
    QVERIFY(options.setData(options.index(OptionsModel::FontFamily), "SystemDefault"));
    QCOMPARE(settings.value("fontFamilyExplicit"), expected_marker);
    {
        AppearanceWidget appearance;
        appearance.setModel(&options);
        appearance.accept(); // The mapper submits every field, even untouched ones.
    }
    QCOMPARE(settings.value("fontFamilyExplicit"), expected_marker);
    QCOMPARE(settings.value("fontFamily").toString(), QString("SystemDefault"));
    // A later launch without CLI must retain the original persisted intent.
    gArgs.ForceRemoveArg("font-family");
    OptionsModel next_launch;
    QCOMPARE(GUIUtil::hasExplicitFontFamily(), marker == 1);
}

void AbyssTests::appearanceFonts()
{
    if (!GUIUtil::fontsLoaded()) QSKIP("Font preferences require minimal:enable_fonts or a native platform");
    RestoreAppearance restore;
    Theme("Abyss");
    GUIUtil::setFontFamily(GUIUtil::FontFamily::SystemDefault, false);
    QVERIFY(GUIUtil::getFontNormal().family().startsWith("Roboto"));
    Theme("Light");
    QCOMPARE(GUIUtil::getFontNormal().family(), GUIUtil::getFont(GUIUtil::FontFamily::SystemDefault, QFont::Normal).family());
    Theme("Abyss");
    OptionsModel options;
    GUIUtil::setFontFamily(GUIUtil::FontFamily::SystemDefault, false);
    {
        AppearanceWidget appearance;
        appearance.setModel(&options);
        QVERIFY(!GUIUtil::hasExplicitFontFamily());
        auto* family = appearance.findChild<QComboBox*>("fontFamily");
        QVERIFY(family);
        QVERIFY(QMetaObject::invokeMethod(family, "activated", Q_ARG(int, 0)));
        QVERIFY(GUIUtil::hasExplicitFontFamily());
    }
    QVERIFY(!GUIUtil::hasExplicitFontFamily()); // Cancel restores even same-family intent.
    QLabel label("Appearance preference");
    GUIUtil::setFont({&label}, GUIUtil::FontWeight::Bold, 18);
    for (auto family : {GUIUtil::FontFamily::SystemDefault, GUIUtil::FontFamily::Montserrat, GUIUtil::FontFamily::Roboto}) {
        GUIUtil::setFontFamily(family);
        GUIUtil::setFontScale(25);
        GUIUtil::updateFonts();
        QCOMPARE(label.font().family(), GUIUtil::getFont(GUIUtil::FontWeight::Bold, false, 18).family());
        QCOMPARE(label.font().pointSizeF(), double(GUIUtil::getScaledFontSize(18)));
        QCOMPARE(label.font().weight(), int(GUIUtil::getFontWeightBold()));
    }
}

void AbyssTests::cssStates()
{
    RestoreAppearance restore;
    Theme("Abyss");
    struct Button { const char* page; const char* frame; const char* name; };
    for (const Button spec : {
        Button{"AddressBookPage", "", "newAddress"}, {"AddressBookPage", "", "copyAddress"},
        {"AddressBookPage", "", "showAddressQRCode"}, {"AddressBookPage", "", "deleteAddress"},
        {"OpenURIDialog", "", "selectFileButton"}, {"OptionsDialog", "", "resetButton"},
        {"SendCoinsDialog", "", "addButton"}, {"SendCoinsDialog", "", "clearButton"},
        {"ReceiveCoinsDialog", "frame", "removeRequestButton"}, {"ReceiveCoinsDialog", "frame", "showRequestButton"},
        {"ReceiveCoinsDialog", "frame2", "clearButton"}, {"ReceiveCoinsDialog", "frame2", "receiveButton"},
        {"ReceiveCoinsDialog", "abyssReceiveCard", "abyssReceiveNewAddress"}, {"SignVerifyMessageDialog", "", "clearButton_SM"},
        {"SignVerifyMessageDialog", "", "clearButton_VM"}}) {
        QDialog page;
        page.setObjectName(spec.page);
        auto* layout = new QVBoxLayout(&page);
        auto* frame = new QFrame(&page); frame->setObjectName(spec.frame);
        auto* inner = new QVBoxLayout(frame);
        auto* button = new StateButton("Action", frame); button->setObjectName(spec.name);
        inner->addWidget(button); layout->addWidget(frame);
        page.resize(300, 180); page.show(); Settle();
        for (int state = 0; state < 4; ++state) {
            button->setEnabled(state != 3);
            button->setAttribute(Qt::WA_UnderMouse, state == 1);
            QEvent hover(state == 1 ? QEvent::Enter : QEvent::Leave);
            QApplication::sendEvent(button, &hover);
            button->setDown(state == 2);
            button->update(); Settle();
            const QColor expected = state == 1 || state == 2 ? QColor("#10263e") : QColor("#0b1727");
            const QImage image = button->paintedState(state);
            const QColor actual = image.pixelColor(image.width() - 12, image.height() / 2);
            QVERIFY2(actual == expected, qPrintable(QString("%1/%2 state=%3 actual=%4 expected=%5")
                .arg(spec.page).arg(spec.name).arg(state).arg(actual.name()).arg(expected.name())));
            const QColor border = state == 1 ? QColor("#3a5e82") :
                                  state == 2 ? QColor("#1297ff") :
                                  state == 3 ? QColor("#1b2d43") : QColor("#29435f");
            const QColor actual_border = image.pixelColor(image.width() / 2, 0);
            if (actual_border != border) {
                qInfo() << "BUTTON_BORDER_DIAGNOSTIC" << spec.page << spec.name << state
                        << image.size() << button->font().toString() << button->styleSheet();
                const QString diagnostic = qEnvironmentVariable("DEFCON_QT_DIAGNOSTICS");
                if (!diagnostic.isEmpty()) {
                    QDir().mkpath(diagnostic);
                    image.save(QDir(diagnostic).filePath(QString("%1-%2-%3.png").arg(spec.page).arg(spec.name).arg(state)));
                }
            }
            QVERIFY2(actual_border == border, qPrintable(QString("%1/%2 state=%3 border=%4 expected=%5")
                .arg(spec.page).arg(spec.name).arg(state).arg(actual_border.name()).arg(border.name())));
            if (GUIUtil::fontsLoaded()) {
                const QColor text = state == 2 ? QColor("#ffffff") :
                                    state == 3 ? QColor("#526780") : QColor("#e6edf7");
                bool has_text_color = false;
                // A font-capable platform must render the declared foreground,
                // not merely the right background behind an inherited grey label.
                for (int y = 2; y < image.height() - 2; ++y) {
                    for (int x = 2; x < image.width() - 2; ++x) {
                        if (image.pixelColor(x, y) == text) has_text_color = true;
                    }
                }
                QVERIFY2(has_text_color, qPrintable(QString("%1/%2 state=%3 missing text color %4")
                    .arg(spec.page).arg(spec.name).arg(state).arg(text.name())));
            }
        }
    }
    // OptionsDialog's former scoped primary-button rule outranked these ID
    // rules and filled inactive tabs blue. Check every tab in its real parent.
    for (const auto& group : std::vector<std::pair<const char*, QStringList>>{
        {"OptionsDialog", {"Main", "Wallet", "CoinJoin", "Network", "Display", "Appearance"}},
        {"RPCConsole", {"Info", "Console", "Commands", "NetTraffic", "Peers", "Repair"}},
        {"SignVerifyMessageDialog", {"SignMessage", "VerifyMessage"}}}) {
        QDialog page; page.setObjectName(group.first);
        auto* layout = new QVBoxLayout(&page);
        for (const QString& name : group.second) {
            StateButton button("Tab", &page);
            button.setObjectName("btn" + name); button.setCheckable(true);
            layout->addWidget(&button); page.resize(300, 180); page.show(); Settle();
            for (bool checked : {false, true}) {
                button.setChecked(checked);
                for (int state = 0; state < 4; ++state) {
                    button.setEnabled(state != 3);
                    const QImage image = button.paintedState(state);
                    const QColor expected = state == 1 || state == 2 ? QColor("#10263e") : QColor("#0b1727");
                    const QColor actual = image.pixelColor(image.width() - 12, image.height() / 2);
                    QVERIFY2(actual == expected, qPrintable(QString("%1/btn%2 state=%3 checked=%4 actual=%5")
                        .arg(group.first).arg(name).arg(state).arg(checked).arg(actual.name())));
                }
            }
            layout->removeWidget(&button);
        }
    }
    // Calendar headers are painted using AlternateBase, not QHeaderView.
    // Its scoped Dark rule used to survive an ID-only calendar override.
    QCalendarWidget calendar;
    calendar.setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
    calendar.resize(420, 280); calendar.show(); Settle();
    auto* calendarView = calendar.findChild<QTableView*>("qt_calendar_calendarview");
    QVERIFY(calendarView);
    QCOMPARE(calendarView->palette().color(QPalette::AlternateBase), QColor("#10263e"));
    QCOMPARE(calendarView->palette().color(QPalette::Highlight), QColor("#1297ff"));
    const QRect headerCell = calendarView->visualRect(calendarView->model()->index(0, 1));
    QVERIFY(headerCell.isValid());
    const QImage calendarPixels = calendarView->viewport()->grab().toImage();
    QCOMPARE(calendarPixels.pixelColor(headerCell.right() - 3, headerCell.center().y()), QColor("#10263e"));
}

void AbyssTests::zebraRows()
{
    RestoreAppearance restore;
    Theme("Abyss");
    QTableWidget table(3, 1);
    table.setAlternatingRowColors(true);
    for (int row = 0; row < 3; ++row) table.setItem(row, 0, new QTableWidgetItem(QString::number(row)));
    table.resize(480, 240); table.show(); Settle();
    table.setColumnWidth(0, table.viewport()->width() - 2);
    table.clearSelection(); Settle();
    const auto color = [&](int row) {
        const QRect cell = table.visualItemRect(table.item(row, 0));
        return table.viewport()->grab().toImage().pixelColor(cell.right() - 12, cell.center().y());
    };
    QCOMPARE(color(0), QColor("#0b1727"));
    QCOMPARE(color(1), QColor("#0e1f33"));
    table.selectRow(1); Settle();
    QCOMPARE(color(1), QColor("#0b6cc6"));
    // Coin Control has a subclass and ID-specific item rules. Its flat mode
    // must retain alternate rows through those rules as well.
    QDialog dialog;
    dialog.setObjectName("CoinControlDialog");
    auto* layout = new QVBoxLayout(&dialog);
    CoinControlTreeWidget tree(&dialog);
    tree.setObjectName("treeWidget");
    tree.setColumnCount(1);
    tree.setAlternatingRowColors(true);
    tree.setRootIsDecorated(false);
    for (int row = 0; row < 3; ++row) new QTreeWidgetItem(&tree, {QString::number(row)});
    layout->addWidget(&tree); dialog.resize(480, 240); dialog.show(); Settle();
    const auto treeColor = [&](int row) {
        const QRect cell = tree.visualItemRect(tree.topLevelItem(row));
        return tree.viewport()->grab().toImage().pixelColor(cell.right() - 12, cell.center().y());
    };
    QCOMPARE(treeColor(0), QColor("#0b1727"));
    QCOMPARE(treeColor(1), QColor("#0e1f33"));
    tree.setCurrentItem(tree.topLevelItem(1)); Settle();
    QCOMPARE(treeColor(1), QColor("#0b6cc6"));
}

void AbyssTests::networkBar()
{
    RestoreAppearance restore;
    Theme("Abyss");
    // No client model: its mempool poll would redraw the bar from the test
    // node every MODEL_UPDATE_DELAY and overwrite the states fed in here.
    OverviewPage page;
    page.resize(1280, 850);
    page.show(); Settle();
    auto* card = page.findChild<QFrame*>("networkCard");
    QVERIFY(card && card->isVisible());
    const auto entry = [&](const char* name) { return card->findChild<QWidget*>(name); };
    const auto part = [&](const char* name, const char* label) {
        QWidget* widget = entry(name);
        return widget ? widget->findChild<QLabel*>(label) : nullptr;
    };
    const QStringList names{"networkBlock", "networkAge", "networkSynced", "networkChainLocked", "networkSyncing",
                            "networkPeers", "networkMasternodes", "networkMempool"};
    for (const QString& name : names) QVERIFY2(entry(name.toUtf8().constData()), qPrintable(name));
    auto* progress = card->findChild<QProgressBar*>("networkSyncProgress");
    QVERIFY(progress);
    const auto shown = [&](const QStringList& expected) {
        for (const QString& name : names) {
            if (entry(name.toUtf8().constData())->isVisible() != expected.contains(name)) return name;
        }
        return QString();
    };
    // The glyph's color is that of its most opaque pixel; its edges blend.
    const auto glyph = [&](const char* name) {
        const QPixmap pixmap = part(name, "networkIcon")->pixmap(Qt::ReturnByValue);
        const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
        QRgb densest = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) > qAlpha(densest)) densest = image.pixel(x, y);
            }
        }
        return std::make_pair(pixmap.size(), QColor(qRed(densest), qGreen(densest), qBlue(densest)));
    };
    const QSize glyph_size = QSize(18, 18) * qApp->devicePixelRatio();
    const auto border = [&] {
        const QImage image = card->grab().toImage();
        return image.pixelColor(image.width() / 2, 0);
    };
    std::map<QLabel*, QFont> fonts;
    for (QLabel* label : card->findChildren<QLabel*>()) fonts.emplace(label, label->font());

    OverviewPage::NetworkStatus synced;
    synced.height = 150628;
    synced.block_age = 2 * 60;
    synced.chain_synced = synced.data_synced = true;
    synced.chain_locked = true;
    synced.peers = 20;
    synced.masternodes = 312;
    synced.mempool = 14;
    page.setNetworkStatus(synced); Settle();
    QCOMPARE(shown({"networkBlock", "networkAge", "networkSynced", "networkChainLocked", "networkPeers",
                    "networkMasternodes", "networkMempool"}), QString());
    QVERIFY(!progress->isVisible());
    QCOMPARE(part("networkBlock", "networkValue")->text(), QString("150") + QChar(0x2009) + "628");
    QCOMPARE(part("networkAge", "networkText")->text(), OverviewPage::tr("%1 ago").arg(OverviewPage::tr("%n minutes", "", 2)));
    QCOMPARE(part("networkPeers", "networkValue")->text(), QString("20"));
    QCOMPARE(part("networkPeers", "networkText")->text(), OverviewPage::tr("peers"));
    QCOMPARE(part("networkMasternodes", "networkValue")->text(), QString("312"));
    QCOMPARE(part("networkMempool", "networkValue")->text(), QString("14"));
    // In words, where the bar has the room for them.
    QCOMPARE(part("networkMasternodes", "networkText")->text(), QString("masternodes"));
    QCOMPARE(part("networkMempool", "networkText")->text(), QString("in mempool"));
    {
        OverviewPage::NetworkStatus one = synced;
        one.masternodes = 1;
        page.setNetworkStatus(one);
        QCOMPARE(part("networkMasternodes", "networkText")->text(), QString("masternode"));
        page.setNetworkStatus(synced);
    }
    // Every entry explains itself; the syncing entry's tooltip is set by state below.
    for (const QString& name : names) {
        if (name != "networkSyncing") QVERIFY2(!entry(name.toUtf8().constData())->toolTip().isEmpty(), qPrintable(name));
    }
    // The age in one unit and plain words, singular when it is one.
    const auto age_text = [&](qint64 secs) {
        OverviewPage::NetworkStatus aged = synced;
        aged.block_age = secs;
        page.setNetworkStatus(aged);
        return part("networkAge", "networkText")->text();
    };
    const auto ago = [](const QString& words) { return OverviewPage::tr("%1 ago").arg(words); };
    QCOMPARE(age_text(59), ago(OverviewPage::tr("under a minute")));
    QCOMPARE(age_text(60), ago(OverviewPage::tr("1 minute")));
    QCOMPARE(age_text(119 * 60), ago(OverviewPage::tr("%n minutes", "", 119)));
    QCOMPARE(age_text(119 * 60), ago(QString("119 minutes")));
    QCOMPARE(age_text(2 * 60 * 60), ago(OverviewPage::tr("%n hours", "", 2)));
    QCOMPARE(age_text(47 * 60 * 60), ago(OverviewPage::tr("%n hours", "", 47)));
    QCOMPARE(age_text(2 * 24 * 60 * 60), ago(OverviewPage::tr("%n days", "", 2)));
    QCOMPARE(age_text(14 * 24 * 60 * 60), ago(OverviewPage::tr("%n weeks", "", 2)));
    QCOMPARE(age_text(31556952), ago(OverviewPage::tr("1 year")));
    QCOMPARE(age_text(7 * 31556952), ago(OverviewPage::tr("%n years", "", 7)));
    // Through a translation with three plural forms the bar picks the form by
    // the number (GPT-Terra r1 R2 / r2 R1): 21 and 1 share one, 2 and 5 differ.
    {
        RussianPlurals russian;
        QVERIFY(qApp->installTranslator(&russian));
        const auto remove = interfaces::MakeHandler([&russian] { qApp->removeTranslator(&russian); });
        page.resize(2560, 850); Settle();
        OverviewPage::NetworkStatus counted = synced;
        for (const auto& [count, word] : std::vector<std::pair<int, const char*>>{
                 {1, "мастернода"}, {2, "мастерноды"}, {5, "мастернод"}, {21, "мастернода"}}) {
            counted.masternodes = count;
            page.setNetworkStatus(counted);
            QCOMPARE(part("networkMasternodes", "networkText")->text(), QString::fromUtf8(word));
        }
        const auto ru = [](const char* words) { return QString::fromUtf8("%1 назад").arg(QString::fromUtf8(words)); };
        QCOMPARE(age_text(60), ru("1 минута"));
        QCOMPARE(age_text(2 * 60), ru("2 минуты"));
        QCOMPARE(age_text(5 * 60), ru("5 минут"));
        QCOMPARE(age_text(21 * 60), ru("21 минута"));
        QCOMPARE(age_text(112 * 60), ru("112 минут"));
        QCOMPARE(age_text(3 * 60 * 60), ru("3 часа"));
        QCOMPARE(age_text(11 * 24 * 60 * 60), ru("11 дней"));
    }
    QCOMPARE(age_text(21 * 60), ago(OverviewPage::tr("%n minutes", "", 21)));
    page.resize(1280, 850);
    page.setNetworkStatus(synced); Settle();
    QCOMPARE(card->property("syncing").toBool(), false);
    QCOMPARE(border(), QColor("#20344c"));
    // Every glyph is its Tabler artwork's coverage in a theme color, drawn for
    // the screen. The words take the matching colors from the stylesheet.
    const auto themed = [](GUIUtil::ThemedColor color) { return GUIUtil::getThemedQColor(color); };
    const std::vector<std::pair<const char*, QColor>> tints{
        {"networkBlock", themed(GUIUtil::ThemedColor::BLUE)},
        {"networkAge", themed(GUIUtil::ThemedColor::UNCONFIRMED)},
        {"networkSynced", themed(GUIUtil::ThemedColor::GREEN)},
        {"networkChainLocked", themed(GUIUtil::ThemedColor::GREEN)},
        {"networkSyncing", themed(GUIUtil::ThemedColor::ORANGE)},
        {"networkPeers", themed(GUIUtil::ThemedColor::UNCONFIRMED)},
        {"networkMasternodes", themed(GUIUtil::ThemedColor::UNCONFIRMED)},
        {"networkMempool", themed(GUIUtil::ThemedColor::UNCONFIRMED)},
    };
    for (const auto& [name, color] : tints) {
        const auto [size, tint] = glyph(name);
        QVERIFY2(size == glyph_size && tint == color, qPrintable(QString("%1: %2 %3x%4").arg(name, tint.name()).arg(size.width()).arg(size.height())));
    }
    QCOMPARE(part("networkSynced", "networkText")->palette().color(QPalette::WindowText), QColor("#7ed321"));
    QCOMPARE(part("networkChainLocked", "networkText")->palette().color(QPalette::WindowText), QColor("#7ed321"));
    QCOMPARE(part("networkSyncing", "networkText")->palette().color(QPalette::WindowText), QColor("#f5b42d"));
    QCOMPARE(part("networkPeers", "networkText")->palette().color(QPalette::WindowText), QColor("#91a5bc"));
    QCOMPARE(part("networkPeers", "networkValue")->palette().color(QPalette::WindowText), QColor("#e6edf7"));

    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const auto capture = [&](const QString& name) {
        return output.isEmpty() || (QDir().mkpath(output) && card->grab().save(QDir(output).filePath(name + ".png")));
    };
    QVERIFY(capture("Abyss-network-synced"));

    // Catching up with the chain: how far, no ChainLock even if one is known,
    // no mempool, and no masternode count yet.
    OverviewPage::NetworkStatus syncing;
    syncing.height = 131046;
    syncing.block_age = 3 * 24 * 60 * 60;
    syncing.progress = 0.873;
    syncing.chain_locked = true;
    syncing.peers = 8;
    syncing.mempool = 5;
    page.setNetworkStatus(syncing); Settle();
    QCOMPARE(shown({"networkBlock", "networkAge", "networkSyncing", "networkPeers", "networkMasternodes"}), QString());
    QVERIFY(progress->isVisible());
    QCOMPARE(progress->value(), 873);
    QCOMPARE(progress->height(), 6);
    QCOMPARE(part("networkAge", "networkText")->text(), OverviewPage::tr("%1 behind").arg(OverviewPage::tr("%n days", "", 3)));
    QCOMPARE(part("networkSyncing", "networkText")->text(), OverviewPage::tr("Syncing %1%").arg(87));
    QCOMPARE(part("networkMasternodes", "networkValue")->text(), QString(QChar(0x2013)));
    QCOMPARE(card->property("syncing").toBool(), true);
    QCOMPARE(border(), QColor("#5a4410"));
    const QImage track = progress->grab().toImage();
    QCOMPARE(track.pixelColor(track.width() / 4, track.height() / 2), QColor("#f5b42d"));
    QCOMPARE(track.pixelColor(track.width() - 3, track.height() / 2), QColor("#20344c"));
    QVERIFY(capture("Abyss-network-syncing"));
    syncing.peers = 1;
    page.setNetworkStatus(syncing); Settle();
    QCOMPARE(part("networkPeers", "networkText")->text(), OverviewPage::tr("peer"));

    // The chain is in, the masternode data is not: still syncing, but the tip
    // is current and the masternodes can be counted.
    OverviewPage::NetworkStatus data = synced;
    data.data_synced = false;
    data.progress = 0.4;
    page.setNetworkStatus(data); Settle();
    QCOMPARE(shown({"networkBlock", "networkAge", "networkSyncing", "networkPeers", "networkMasternodes"}), QString());
    // No percentage next to a current tip, and no track: "Syncing 100%" read as stuck.
    QCOMPARE(part("networkSyncing", "networkText")->text(), OverviewPage::tr("Syncing data…"));
    QVERIFY(!progress->isVisible());
    QCOMPARE(part("networkAge", "networkText")->text(), OverviewPage::tr("%1 ago").arg(OverviewPage::tr("%n minutes", "", 2)));
    QCOMPARE(part("networkMasternodes", "networkValue")->text(), QString("312"));

    // Back in sync, the card and every label are as they were: only the card
    // was repolished, so no label lost the font GUIUtil gave it.
    page.setNetworkStatus(synced); Settle();
    QCOMPARE(card->property("syncing").toBool(), false);
    QCOMPARE(border(), QColor("#20344c"));
    for (const auto& [label, font] : fonts) QVERIFY2(label->font() == font, qPrintable(label->objectName()));

    // At the page's narrowest the bar still shows every entry whole.
    if (GUIUtil::fontsLoaded()) {
        for (const auto& status : {synced, syncing}) {
            page.setNetworkStatus(status);
            page.resize(page.minimumWidth(), 850); Settle();
            QVERIFY2(card->minimumSizeHint().width() <= card->width(),
                     qPrintable(QString("bar needs %1, has %2").arg(card->minimumSizeHint().width()).arg(card->width())));
            for (const QString& name : names) {
                QWidget* widget = entry(name.toUtf8().constData());
                if (!widget->isVisible()) continue;
                QVERIFY2(widget->width() >= widget->sizeHint().width(), qPrintable(name));
            }
        }
        // The words do not set the page's minimum: it is the same whichever are
        // up, or the window could never get narrow enough for the short ones.
        page.setNetworkStatus(synced);
        page.resize(1280, 850); Settle();
        QCOMPARE(part("networkMasternodes", "networkText")->text(), QString("masternodes"));
        const int roomy_minimum = page.minimumSizeHint().width();
        page.resize(page.minimumWidth(), 850); Settle();
        qInfo() << "NETWORK_WORDS" << card->width() << card->minimumSizeHint().width() << card->sizeHint().width()
                << part("networkMasternodes", "networkText")->text() << page.minimumWidth() << roomy_minimum;
        QCOMPARE(page.minimumSizeHint().width(), roomy_minimum);
        // Where they do not fit, the short forms, and the bar is still whole. The
        // form's placeholder amounts hold this page wide; a wallet's ordinary ones
        // leave it narrow enough.
        OverviewPage narrow;
        for (const char* name : {"labelBalance", "labelUnconfirmed", "labelImmature", "labelTotal"}) {
            narrow.findChild<QLabel*>(name)->setText("1.00 DFCN");
        }
        for (const char* name : {"labelSpendable", "labelWatchonly", "labelWatchAvailable", "labelWatchPending",
                                 "labelWatchImmature", "labelWatchTotal", "lineWatchBalance"}) {
            narrow.findChild<QWidget*>(name)->hide();
        }
        narrow.setNetworkStatus(synced);
        narrow.show(); Settle();
        auto* narrow_card = narrow.findChild<QFrame*>("networkCard");
        const auto words = [&] {
            return QStringList{narrow_card->findChild<QWidget*>("networkMasternodes")->findChild<QLabel*>("networkText")->text(),
                               narrow_card->findChild<QWidget*>("networkMempool")->findChild<QLabel*>("networkText")->text()};
        };
        narrow.resize(narrow.minimumWidth(), 850); Settle();
        qInfo() << "NETWORK_WORDS_NARROW" << narrow_card->width() << narrow_card->minimumSizeHint().width() << narrow_card->sizeHint().width()
                << words() << narrow.minimumWidth();
        GUIUtil::setFontScale(50);
        GUIUtil::updateFonts(); Settle();
        narrow.resize(narrow.minimumWidth(), 850); Settle();
        qInfo() << "NETWORK_WORDS_LARGE" << narrow_card->width() << narrow_card->minimumSizeHint().width() << narrow_card->sizeHint().width()
                << words() << narrow.minimumWidth();
        QCOMPARE(words(), (QStringList{"MN", "tx"}));
        QVERIFY(narrow_card->minimumSizeHint().width() <= narrow_card->width());
        const int short_minimum = narrow.minimumSizeHint().width();
        narrow.resize(2560, 850); Settle();
        QCOMPARE(words(), (QStringList{"masternodes", "in mempool"}));
        QCOMPARE(narrow.minimumSizeHint().width(), short_minimum);
        narrow.hide();
    }

    // The classic themes keep their pages as they were: no bar.
    Theme("Light");
    QVERIFY(!card->isVisible());
}

void AbyssTests::paymentPages()
{
    if (!GUIUtil::fontsLoaded()) QSKIP("Payment layout needs minimal:enable_fonts or a native platform");
    RestoreAppearance restore;
    const int flash = QApplication::cursorFlashTime();
    QApplication::setCursorFlashTime(0);
    const auto restore_flash = interfaces::MakeHandler([flash] { QApplication::setCursorFlashTime(flash); });
    WalletFixture fixture(m_node);
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    for (const char* classic : {"Light", "Dark", "Traditional"}) {
        Theme(classic);
        SendCoinsDialog send;
        send.setModel(fixture.model.get());
        ReceiveCoinsDialog receive;
        receive.setModel(fixture.model.get());
        send.resize(1280, 850); receive.resize(1280, 850);
        send.show(); receive.show(); Settle();
        // The unchanged r12 selectable balance has a first-switch font-cache
        // difference in Traditional. Warm it once, then compare the entire page.
        if (QString(classic) == "Traditional") {
            Theme("Abyss"); Theme(classic); Settle();
        }
        const QImage original_send = send.grab().toImage();
        const QImage original_receive = receive.grab().toImage();
        QPointer<QWidget> original_receive_focus = receive.focusWidget();
        auto* actions = send.findChild<QHBoxLayout*>("horizontalLayout");
        QVERIFY(actions);
        const int original_action_count = actions->count();
        auto* send_button = send.findChild<QPushButton*>("sendButton");
        auto* clear_button = send.findChild<QPushButton*>("clearButton");
        auto* receive_button = receive.findChild<QPushButton*>("receiveButton");
        QVERIFY(send_button && clear_button && receive_button);
        Theme("Abyss"); Settle();
        auto* summary = send.findChild<QFrame*>("abyssSendSummary");
        auto* amount = send.findChild<QLabel*>("abyssEnteredAmount");
        auto* count = send.findChild<QLabel*>("abyssRecipientCount");
        auto* rate = send.findChild<QLabel*>("abyssFeeRate");
        QVERIFY(summary && amount && count && rate && summary->isVisible());
        QCOMPARE(send.findChild<QPushButton*>("sendButton"), send_button);
        QCOMPARE(receive.findChild<QPushButton*>("receiveButton"), receive_button);
        // Abyss folds the request form behind the address card's link: open it to lay it out.
        auto* request_toggle = receive.findChild<QPushButton*>("abyssReceiveRequestToggle");
        QVERIFY(request_toggle);
        request_toggle->click(); Settle();
        QVERIFY(receive.findChild<QFrame*>("frame2")->isVisible());
        // The link hands the focus to the form; the classic comparisons want it where it was.
        if (original_receive_focus) original_receive_focus->setFocus(Qt::OtherFocusReason);
        else if (QWidget* focused = receive.focusWidget()) focused->clearFocus();
        Settle();
        QCOMPARE(count->text(), QString("1"));
        QCOMPARE(rate->text(), send.findChild<QLabel*>("labelFeeMinimized")->text());
        for (int width : {704, 1026}) {
            send.resize(width, 740); receive.resize(width, 740); Settle();
            QCOMPARE(send.width(), width);
            QCOMPARE(receive.width(), width);
            const QRect action = OnPage(send, send_button);
            QVERIFY(send.rect().contains(action));
            auto* fields_scroll = send.findChild<QScrollArea*>("abyssSendFieldsScroll");
            QVERIFY(fields_scroll);
            QVERIFY(OnPage(send, summary).left() > OnPage(send, fields_scroll).right());
            QCOMPARE(fields_scroll->horizontalScrollBar()->maximum(), 0);
            auto* label = receive.findChild<QLineEdit*>("reqLabel");
            auto* field = receive.findChild<QLabel*>("label_2");
            QVERIFY(label && field && OnPage(receive, field).bottom() < OnPage(receive, label).top());
            // The whole address on its one line, and the whole card on the page.
            auto* address = receive.findChild<QLabel*>("abyssReceiveAddress");
            auto* address_card = receive.findChild<QFrame*>("abyssReceiveCard");
            QVERIFY(address && address_card);
            QVERIFY2(AmountFits(address), qPrintable(QString("%1: %2 in %3").arg(width).arg(address->text()).arg(address->width())));
            QVERIFY2(receive.rect().contains(OnPage(receive, address_card)), qPrintable(QString::number(width)));
            if (!output.isEmpty() && QString(classic) == "Light") {
                QVERIFY(QDir().mkpath(output));
                QVERIFY(send.grab().save(QDir(output).filePath(QString("Abyss-send-stage2-%1.png").arg(width))));
                QVERIFY(receive.grab().save(QDir(output).filePath(QString("Abyss-receive-stage2-%1.png").arg(width))));
            }
        }
        Theme(classic);
        send.resize(1280, 850); receive.resize(1280, 850); Settle();
        QVERIFY(!summary->isVisible());
        QCOMPARE(send.grab().toImage(), original_send);
        QCOMPARE(receive.grab().toImage(), original_receive);
        QCOMPARE(send.findChild<QPushButton*>("clearButton"), clear_button);
        Theme("Abyss"); Theme(classic); Settle();
        QCOMPARE(actions->count(), original_action_count);
        QCOMPARE(actions->itemAt(0)->widget(), send_button);
        QCOMPARE(actions->itemAt(1)->widget(), clear_button);
        QCOMPARE(actions->itemAt(2)->widget(), send.findChild<QPushButton*>("addButton"));
        QCOMPARE(send.grab().toImage(), original_send);
        QCOMPARE(receive.grab().toImage(), original_receive);

        Theme("Abyss"); Settle();
        send.activateWindow(); Settle();
        auto* address = send.findChild<QLineEdit*>("payTo");
        auto* recipient_label = send.findChild<QLineEdit*>("addAsLabel");
        QVERIFY(address && recipient_label);
        address->setFocus(); Settle();
        QTest::keyClick(address, Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), recipient_label);
        Theme(classic); Theme("Abyss"); Settle();
        QCOMPARE(QApplication::focusWidget(), recipient_label);
        send.hide();
        Theme(classic); Theme("Abyss"); Settle();
        QCOMPARE(send.focusWidget(), recipient_label);
        send.show(); QApplication::setActiveWindow(&send); Settle();
        QCOMPARE(QApplication::focusWidget(), recipient_label);
        auto* first = send.findChild<SendCoinsEntry*>();
        QVERIFY(first);
        first->setAmount(125000000);
        const auto format = [&](CAmount value) {
            return BitcoinUnits::formatWithUnit(fixture.options->getDisplayUnit(), value);
        };
        QCOMPARE(amount->text(), format(125000000));
        send.findChild<QPushButton*>("addButton")->click(); Settle();
        QCOMPARE(count->text(), QString("2"));
        auto entries = send.findChildren<SendCoinsEntry*>();
        QCOMPARE(entries.size(), 2);
        auto* second = entries.at(0) == first ? entries.at(1) : entries.at(0);
        second->setAmount(50000000);
        QCOMPARE(amount->text(), format(175000000));
        second->findChild<QToolButton*>("deleteButton")->click(); Settle();
        QCOMPARE(count->text(), QString("1"));
        QCOMPARE(amount->text(), format(125000000));
        send.findChild<QPushButton*>("buttonChooseFee")->click(); Settle();
        QVERIFY(send.findChild<QFrame*>("frameFeeSelection")->isVisible());
        send.resize(704, 1000); Settle();
        QCOMPARE(send.width(), 704);
        QVERIFY(QMetaObject::invokeMethod(&send, "coinControlFeatureChanged", Q_ARG(bool, true)));
        Settle();
        QCOMPARE(send.width(), 704);
        QVERIFY(send.findChild<QFrame*>("frameCoinControl")->isVisible());
        QCOMPARE(send.findChild<QScrollArea*>("abyssSendFieldsScroll")->horizontalScrollBar()->maximum(), 0);
        if (!output.isEmpty() && QString(classic) == "Light") {
            QVERIFY(send.grab().save(QDir(output).filePath("Abyss-send-stage2-expanded.png")));
        }
        clear_button->click(); Settle();
        QCOMPARE(count->text(), QString("1"));
        QCOMPARE(amount->text(), format(0));
        auto* request_label = receive.findChild<QLineEdit*>("reqLabel");
        request_label->setText("Sample");
        receive.findChild<QPushButton*>("clearButton")->click();
        QVERIFY(request_label->text().isEmpty());
        // Rendering/reparenting must not enable wallet actions as a side effect.
        const bool enabled = receive_button->isEnabled();
        Theme(classic); Theme("Abyss"); Settle();
        QCOMPARE(receive_button->isEnabled(), enabled);
        if (QString(classic) == "Light") {
            // Invalid input must stop before unlock/transaction preparation and
            // reveal an off-screen recipient using the actual Send connection.
            SendCoinsDialog invalid;
            invalid.setModel(fixture.model.get());
            invalid.resize(704, 740); invalid.show(); Settle();
            auto* valid_entry = invalid.findChild<SendCoinsEntry*>();
            QVERIFY(valid_entry);
            valid_entry->setAddress(QString::fromStdString(EncodeDestination(PKHash(fixture.chain.coinbaseKey.GetPubKey()))));
            valid_entry->setAmount(100000000);
            auto* middle_entry = invalid.addEntry();
            middle_entry->setAddress(QString::fromStdString(EncodeDestination(PKHash(fixture.chain.coinbaseKey.GetPubKey()))));
            middle_entry->setAmount(100000000);
            QVERIFY(valid_entry->validate(fixture.node));
            QVERIFY(middle_entry->validate(fixture.node));
            auto* bad_entry = invalid.addEntry();
            bad_entry->setAmount(100000000);
            auto* bad_address = bad_entry->findChild<QLineEdit*>("payTo");
            auto* scroller = invalid.findChild<QScrollArea*>("abyssSendFieldsScroll");
            QVERIFY(bad_address && scroller); Settle();
            QVERIFY(!bad_address->visibleRegion().isEmpty());
            scroller->verticalScrollBar()->setValue(0); Settle();
            QVERIFY(bad_address->visibleRegion().isEmpty());
            QSignalSpy sent(&invalid, &SendCoinsDialog::coinsSent);
            auto* invalid_send = invalid.findChild<QPushButton*>("sendButton");
            QVERIFY(invalid_send && invalid_send->isEnabled());
            QSignalSpy clicked(invalid_send, &QPushButton::clicked);
            invalid_send->click(); Settle();
            QCOMPARE(clicked.count(), 1);
            QCOMPARE(sent.count(), 0);
            QVERIFY(!bad_address->visibleRegion().isEmpty());
        }
        auto* custom_radio = send.findChild<QRadioButton*>("radioCustomFee");
        auto* smart_radio = send.findChild<QRadioButton*>("radioSmartFee");
        auto* custom_fee = send.findChild<BitcoinAmountField*>("customFee");
        QVERIFY(custom_radio && smart_radio && custom_fee);
        custom_radio->click(); custom_fee->setValue(2000); Settle();
        QCOMPARE(rate->text(), format(2000) + "/kB");
        smart_radio->click(); Settle();
        QCOMPARE(rate->text(), send.findChild<QLabel*>("labelSmartFee")->text());

        const int scale = GUIUtil::getFontScale();
        GUIUtil::setFontScale(25); GUIUtil::updateFonts();
        send.resize(1026, 850); receive.resize(1026, 850); Settle();
        QCOMPARE(send.width(), 1026);
        QCOMPARE(receive.width(), 1026);
        QVERIFY(send.rect().contains(OnPage(send, send_button)));
        auto* fields_scroll = send.findChild<QScrollArea*>("abyssSendFieldsScroll");
        QCOMPARE(fields_scroll->horizontalScrollBar()->maximum(), 0);
        auto* available = send.findChild<QPushButton*>("useAvailableBalanceButton");
        QVERIFY(available);
        fields_scroll->ensureWidgetVisible(available); Settle();
        QVERIFY(available->visibleRegion().contains(available->rect()));
        auto* scaled_address = receive.findChild<QLabel*>("abyssReceiveAddress");
        QVERIFY(scaled_address && AmountFits(scaled_address));
        QVERIFY(receive.rect().contains(OnPage(receive, receive.findChild<QFrame*>("abyssReceiveCard"))));
        if (!output.isEmpty() && QString(classic) == "Light") {
            QVERIFY(send.grab().save(QDir(output).filePath("Abyss-send-stage2-font25.png")));
            QVERIFY(receive.grab().save(QDir(output).filePath("Abyss-receive-stage2-font25.png")));
        }
        GUIUtil::setFontScale(scale); GUIUtil::updateFonts();
        QPointer<QHBoxLayout> detached_actions;
        Theme(classic);
        {
            SendCoinsDialog cleanup;
            cleanup.setModel(fixture.model.get());
            detached_actions = cleanup.findChild<QHBoxLayout*>("horizontalLayout");
            QVERIFY(detached_actions);
            Theme("Abyss"); Settle();
            QVERIFY(detached_actions);
        }
        QVERIFY(detached_actions.isNull());
    }
}

void AbyssTests::paymentSummaryLifecycle()
{
    RestoreAppearance restore;
    Theme("Abyss");
    // No model/show is needed to describe one empty recipient. A real page
    // is also constructed before it is selected in the wallet's stack.
    QSettings settings;
    const QVariant previous_change = settings.value("sCustomChangeAddress");
    {
        SendCoinsDialog send;
        auto* count = send.findChild<QLabel*>("abyssRecipientCount");
        auto* amount = send.findChild<QLabel*>("abyssEnteredAmount");
        QVERIFY(count && amount);
        QCOMPARE(count->text(), QString("1"));
        auto* entry = send.findChild<SendCoinsEntry*>();
        QVERIFY(entry);
        entry->setAmount(125000000);
        QCOMPARE(amount->text(), BitcoinUnits::formatWithUnit(BitcoinUnits::DASH, 125000000));
        send.addEntry();
        QCOMPARE(count->text(), QString("2"));
        send.clear();
        QCOMPARE(count->text(), QString("1"));
        QCOMPARE(amount->text(), BitcoinUnits::formatWithUnit(BitcoinUnits::DASH, 0));
    }
    // A model-less preview must not persist a custom change address.
    QCOMPARE(settings.value("sCustomChangeAddress"), previous_change);
}

void AbyssTests::networkModelLifecycle()
{
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    ClientModel replacement(fixture.node, fixture.options.get());
    OverviewPage page;
    page.setClientModel(fixture.client.get());
    auto* peers = page.findChild<QWidget*>("networkPeers");
    QVERIFY(peers);
    auto* value = peers->findChild<QLabel*>("networkValue");
    QVERIFY(value);
    QSignalSpy other_receiver(fixture.client.get(), &ClientModel::numConnectionsChanged);
    fixture.client->numConnectionsChanged(11);
    QCOMPARE(value->text(), QString("11"));
    page.setClientModel(&replacement);
    replacement.numConnectionsChanged(23);
    QCOMPARE(value->text(), QString("23"));
    // Disconnect this page's subscriptions, not the sender's other listeners.
    fixture.client->numConnectionsChanged(777);
    QCOMPARE(other_receiver.count(), 2);
    QCOMPARE(value->text(), QString("23"));
    QVERIFY(QMetaObject::invokeMethod(&replacement, "masternodeListChanged", Qt::QueuedConnection));
    page.setClientModel(nullptr);
    fixture.client->masternodeListChanged();
    replacement.masternodeListChanged();
    replacement.numConnectionsChanged(99);
    Settle();
    QCOMPARE(value->text(), QString("23"));
    page.setClientModel(&replacement);
    replacement.numConnectionsChanged(31);
    QCOMPARE(value->text(), QString("31"));
}

void AbyssTests::emptyStates()
{
    RestoreAppearance restore;
    // Pixel comparisons must not depend on a line edit's cursor blink phase.
    const int flash = QApplication::cursorFlashTime();
    QApplication::setCursorFlashTime(0);
    const auto restore_flash = interfaces::MakeHandler([flash] { QApplication::setCursorFlashTime(flash); });
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const auto capture = [&](QWidget& widget, const char* name) {
        if (output.isEmpty()) return true;
        return QDir().mkpath(output) && widget.grab().save(QDir(output).filePath(QString("Abyss-empty-%1.png").arg(name)));
    };
    WalletFixture fixture(m_node);
    // A payment request belongs to one of the wallet's receiving addresses;
    // a saved sending address is somebody else's.
    const CTxDestination receiving = PKHash(fixture.chain.coinbaseKey.GetPubKey());
    fixture.wallet->SetAddressBook(receiving, "", "receive");
    CKey other_key;
    other_key.MakeNewKey(true);
    const QString address = QString::fromStdString(EncodeDestination(receiving));
    const QString sending = QString::fromStdString(EncodeDestination(PKHash(other_key.GetPubKey())));
    // Every line of the sentence and the whole button are inside the message.
    const auto fits = [](EmptyStateWidget* empty) {
        auto* text = empty->findChild<QLabel*>("emptyStateText");
        QPushButton* button = empty->actionButton();
        return text && text->height() >= text->heightForWidth(text->width()) &&
               empty->rect().contains(QRect(button->mapTo(empty, QPoint()), button->size()));
    };

    // Overview: an empty list says so, or that it is still loading.
    {
        Theme("Abyss");
        OverviewPage overview;
        overview.setClientModel(fixture.client.get());
        overview.setWalletModel(fixture.model.get());
        overview.resize(1280, 820); overview.show(); Settle();
        auto* empty = overview.findChild<EmptyStateWidget*>("emptyState");
        auto* list = overview.findChild<QListView*>("listTransactions");
        auto* chip = overview.findChild<QLabel*>("labelTransactionsStatus");
        QVERIFY(empty && list && chip);
        QCOMPARE(chip->text(), OverviewPage::tr("out of sync"));
        QCOMPARE(empty->state(), EmptyStateWidget::State::Loading);
        QVERIFY(empty->isVisible());
        QCOMPARE(empty->geometry(), list->viewport()->geometry());
        QVERIFY(capture(overview, "overview-loading"));
        overview.showOutOfSyncWarning(false); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        QCOMPARE(empty->actionButton()->text(), OverviewPage::tr("Receive"));
        QVERIFY(empty->actionButton()->isVisible());
        QVERIFY(fits(empty));
        QSignalSpy receive(&overview, &OverviewPage::receiveClicked);
        empty->actionButton()->click();
        QCOMPARE(receive.count(), 1);
        QVERIFY(capture(overview, "overview"));
        // Discreet mode hides the list, and with it whether anything is in it.
        overview.setPrivacy(true); Settle();
        QVERIFY(!empty->isVisible());
        overview.setPrivacy(false); Settle();
        QVERIFY(empty->isVisible());
        Theme("Light"); Settle();
        QVERIFY(!empty->isVisible());
        QCOMPARE(chip->text(), "(" + OverviewPage::tr("out of sync") + ")");
        Theme("Abyss"); Settle();
        QVERIFY(empty->isVisible());
        QCOMPARE(chip->text(), OverviewPage::tr("out of sync"));
    }

    // Transactions: nothing yet, or still loading. The header stays visible.
    {
        Theme("Light");
        TransactionView view;
        view.setModel(fixture.model.get());
        view.resize(1280, 700); view.show(); Settle();
        const QImage classic = view.grab().toImage();
        auto* empty = view.findChild<EmptyStateWidget*>("emptyState");
        auto* table = view.findChild<QTableView*>("transactionView");
        QVERIFY(empty && table);
        QVERIFY(!empty->isVisible());
        Theme("Abyss"); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        QVERIFY(empty->isVisible());
        QVERIFY(table->horizontalHeader()->isVisible());
        QCOMPARE(empty->geometry(), table->viewport()->geometry());
        QVERIFY(!empty->geometry().intersects(table->horizontalHeader()->geometry()));
        QVERIFY(fits(empty));
        QSignalSpy receive(&view, &TransactionView::receiveClicked);
        empty->actionButton()->click();
        QCOMPARE(receive.count(), 1);
        QVERIFY(capture(view, "transactions"));
        view.setOutOfSync(true); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Loading);
        QVERIFY(capture(view, "transactions-loading"));
        view.setOutOfSync(false);
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        Theme("Light"); Settle();
        QVERIFY(!empty->isVisible());
        QCOMPARE(view.grab().toImage(), classic);
        // The history page is often opened long after a theme change: an
        // unpolished page gets no StyleChange, only its first show.
        Theme("Abyss");
        TransactionView unshown;
        unshown.setModel(fixture.model.get());
        auto* unshown_empty = unshown.findChild<EmptyStateWidget*>("emptyState");
        QVERIFY(unshown_empty);
        QCOMPARE(unshown_empty->state(), EmptyStateWidget::State::Empty);
        Theme("Light");
        unshown.resize(1280, 700); unshown.show(); Settle();
        QCOMPARE(unshown_empty->state(), EmptyStateWidget::State::Hidden);
        QVERIFY(!unshown_empty->isVisible());
    }

    // Receive: the address card comes first; an empty payment history folds
    // down to its title and one line, and opens with its first request.
    {
        Theme("Abyss");
        ReceiveCoinsDialog receive;
        receive.setModel(fixture.model.get());
        receive.resize(1280, 820); receive.show(); Settle();
        auto* none = receive.findChild<QLabel*>("abyssReceiveHistoryNone");
        auto* table = receive.findChild<QTableView*>("recentRequestsView");
        auto* show = receive.findChild<QPushButton*>("showRequestButton");
        auto* remove = receive.findChild<QPushButton*>("removeRequestButton");
        auto* history = receive.findChild<QFrame*>("frame");
        QVERIFY(none && table && show && remove && history);
        auto* title = receive.findChild<QLabel*>("label_6");
        auto* folded_title = receive.findChild<QLabel*>("abyssReceiveHistoryTitle");
        QVERIFY(title && folded_title);
        QVERIFY(none->isVisible() && folded_title->isVisible() && !title->isVisible());
        QVERIFY(!table->isVisible() && !show->isVisible() && !remove->isVisible());
        QCOMPARE(none->text(), ReceiveCoinsDialog::tr("No payment requests yet"));
        QCOMPARE(folded_title->text(), title->text());
        // One line: the title and what the history says, side by side.
        QVERIFY(std::abs(OnPage(receive, none).center().y() - OnPage(receive, folded_title).center().y()) <= 1);
        // And that line is the card's middle: no room is left under it for the hidden buttons' row.
        QVERIFY2(std::abs(OnPage(receive, history).center().y() - OnPage(receive, folded_title).center().y()) <= 2,
                 qPrintable(QString("card %1, line %2").arg(OnPage(receive, history).center().y()).arg(OnPage(receive, folded_title).center().y())));
        // Folded, the card is no taller than its title and that line need.
        QVERIFY2(history->height() <= history->sizeHint().height(), qPrintable(QString("%1 > %2").arg(history->height()).arg(history->sizeHint().height())));
        QVERIFY(capture(receive, "receive"));
        RecentRequestsTableModel* requests = fixture.model->getRecentRequestsTableModel();
        requests->addNewRequest(SendCoinsRecipient(address, "Sample", 100000000, ""));
        QCOMPARE(requests->rowCount({}), 1);
        Settle();
        QVERIFY(table->isVisible() && show->isVisible() && remove->isVisible() && !none->isVisible());
        QVERIFY(title->isVisible() && !folded_title->isVisible());
        QVERIFY(capture(receive, "receive-history"));
        QVERIFY(requests->removeRows(0, 1));
        Settle();
        QVERIFY(none->isVisible() && !table->isVisible());
        Theme("Light"); Settle();
        QVERIFY(!none->isVisible() && table->isVisible() && show->isVisible() && remove->isVisible());
        QVERIFY(title->isVisible() && !folded_title->isVisible());
    }

    // Address book: Add address opens the existing dialog; a search that
    // hides every saved address offers to clear it.
    {
        Theme("Light");
        AddressBookPage book(AddressBookPage::ForEditing, AddressBookPage::SendingTab);
        book.setModel(fixture.model->getAddressTableModel());
        book.resize(760, 520); book.show(); Settle();
        const QSize classic_size = book.size();
        const QImage classic = book.grab().toImage();
        auto* empty = book.findChild<EmptyStateWidget*>("emptyState");
        auto* search = book.findChild<QLineEdit*>("searchLineEdit");
        QVERIFY(empty && search);
        QVERIFY(!empty->isVisible());
        Theme("Abyss"); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        QCOMPARE(empty->actionButton()->text(), AddressBookPage::tr("Add address"));
        QVERIFY(fits(empty));
        QVERIFY(capture(book, "addressbook"));
        // The theme round trip alone gives the classic page back, pixel for pixel.
        Theme("Light"); Settle();
        QVERIFY(!empty->isVisible());
        // Abyss's buttons can need a wider window than the classic ones (the
        // minimal platform without fonts does), and a window keeps the width
        // it grew to. Given its old size back, the page is the same.
        book.resize(classic_size); Settle();
        QCOMPARE(book.size(), classic_size);
        QCOMPARE(book.grab().toImage(), classic);
        Theme("Abyss"); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        QString opened;
        QTimer::singleShot(0, [&opened] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (widget->inherits("EditAddressDialog") && widget->isVisible()) {
                    opened = widget->metaObject()->className();
                    qobject_cast<QDialog*>(widget)->reject();
                }
            }
        });
        empty->actionButton()->click();
        QCOMPARE(opened, QString("EditAddressDialog"));
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        // An address the search hides still changes what the empty list means.
        AddressTableModel* addresses = fixture.model->getAddressTableModel();
        search->setText("no such address"); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        QCOMPARE(addresses->addRow(AddressTableModel::Send, "Alice", sending), sending);
        Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Filtered);
        QCOMPARE(empty->actionButton()->text(), EmptyStateWidget::tr("Clear filters"));
        QVERIFY(capture(book, "addressbook-filtered"));
        empty->actionButton()->click(); Settle();
        QVERIFY(search->text().isEmpty());
        QCOMPARE(empty->state(), EmptyStateWidget::State::Hidden);
        search->setText("no such address"); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Filtered);
        // While hidden, the address turns into a receiving one (importprivkey
        // does that) and back: only the source model says so.
        const CTxDestination sending_dest = PKHash(other_key.GetPubKey());
        fixture.wallet->SetAddressBook(sending_dest, "Alice", "receive");
        Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        fixture.wallet->SetAddressBook(sending_dest, "Alice", "send");
        Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Filtered);
        QVERIFY(addresses->removeRows(addresses->lookupAddress(sending), 1, {}));
        Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        search->clear(); Settle();
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        Theme("Light"); Settle();
        QVERIFY(!empty->isVisible());
        // A page that was never shown follows a theme change too.
        Theme("Abyss");
        AddressBookPage unshown(AddressBookPage::ForSelection, AddressBookPage::SendingTab);
        unshown.setModel(fixture.model->getAddressTableModel());
        auto* unshown_empty = unshown.findChild<EmptyStateWidget*>("emptyState");
        QVERIFY(unshown_empty);
        QCOMPARE(unshown_empty->state(), EmptyStateWidget::State::Empty);
        Theme("Light");
        unshown.show(); Settle();
        QCOMPARE(unshown_empty->state(), EmptyStateWidget::State::Hidden);
        QVERIFY(!unshown_empty->isVisible());
        unshown.hide();
        // Receiving addresses are made on the Receive page: that tab keeps its table.
        AddressBookPage receiving(AddressBookPage::ForEditing, AddressBookPage::ReceivingTab);
        receiving.setModel(fixture.model->getAddressTableModel());
        QVERIFY(!receiving.findChild<EmptyStateWidget*>());
    }

    // No wallet: Abyss has no File menu, so the card opens a wallet itself.
    {
        Theme("Light");
        WalletFrame frame(nullptr);
        frame.resize(1000, 700); frame.show(); Settle();
        const QImage classic = frame.grab().toImage();
        auto* card = frame.findChild<EmptyStateWidget*>("emptyState");
        QVERIFY(card && !card->isVisible());
        Theme("Abyss"); Settle();
        QVERIFY(card->isVisible());
        QCOMPARE(card->state(), EmptyStateWidget::State::Empty);
        QVERIFY(fits(card));
        QVERIFY(!card->actionButton()->isEnabled());
        QMenu wallets;
        frame.setOpenWalletMenu(&wallets);
        QVERIFY(card->actionButton()->isEnabled());
        QCOMPARE(card->actionButton()->menu(), &wallets);
        auto* create = card->findChild<QPushButton*>("emptyStateSecondary");
        QVERIFY(create && create->isVisible());
        QSignalSpy created(&frame, &WalletFrame::createWalletButtonClicked);
        create->click();
        QCOMPARE(created.count(), 1);
        QVERIFY(capture(frame, "nowallet"));
        Theme("Light"); Settle();
        QVERIFY(!card->isVisible());
        QCOMPARE(frame.grab().toImage(), classic);
    }

    // A history whose every row a filter hides: Clear filters shows it again.
    {
        Theme("Abyss");
        {
            auto spk_man = fixture.wallet->GetOrCreateLegacyScriptPubKeyMan();
            const CBlockIndex* tip = WITH_LOCK(::cs_main, return fixture.chain.m_node.chainman->ActiveChain().Tip());
            LOCK2(fixture.wallet->cs_wallet, spk_man->cs_KeyStore);
            spk_man->AddKeyPubKey(fixture.chain.coinbaseKey, fixture.chain.coinbaseKey.GetPubKey());
            fixture.wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        }
        {
            WalletRescanReserver reserver(*fixture.wallet);
            reserver.reserve();
            const CWallet::ScanResult result = fixture.wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, true);
            QCOMPARE(result.status, CWallet::ScanResult::SUCCESS);
        }
        WalletModel seeded(interfaces::MakeWallet(fixture.wallet), *fixture.client);
        TransactionView view;
        view.setModel(&seeded);
        view.resize(1280, 700); view.show(); Settle();
        auto* empty = view.findChild<EmptyStateWidget*>("emptyState");
        auto* table = view.findChild<QTableView*>("transactionView");
        auto* search = view.findChild<QLineEdit*>("search_widget");
        auto* amount = view.findChild<QLineEdit*>("amountWidget");
        QVERIFY(empty && table && search && amount);
        // Measure against no filter at all, which is what Clear filters restores.
        view.chooseType(0);
        view.chooseDate(TransactionView::All);
        const int rows = table->model()->rowCount();
        QVERIFY(rows > 0);
        QCOMPARE(empty->state(), EmptyStateWidget::State::Hidden);
        search->setText("no such transaction");
        view.changedSearch();
        QCOMPARE(table->model()->rowCount(), 0);
        QCOMPARE(empty->state(), EmptyStateWidget::State::Filtered);
        QCOMPARE(empty->actionButton()->text(), EmptyStateWidget::tr("Clear filters"));
        // A filter explains an empty list better than a sync still running.
        view.setOutOfSync(true);
        QCOMPARE(empty->state(), EmptyStateWidget::State::Filtered);
        view.setOutOfSync(false);
        Settle();
        QVERIFY(capture(view, "transactions-filtered"));
        amount->setText("1000000000");
        view.changedAmount();
        view.chooseType(3); // Sent to: none of these coinbase payments
        view.chooseDate(TransactionView::Today);
        empty->actionButton()->click(); Settle();
        QVERIFY(search->text().isEmpty());
        QVERIFY(amount->text().isEmpty());
        QCOMPARE(table->model()->rowCount(), rows);
        QCOMPARE(empty->state(), EmptyStateWidget::State::Hidden);
    }
}

void AbyssTests::screenshots()
{
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (output.isEmpty()) return; // Opt-in artifacts, no production hooks.
    QVERIFY(GUIUtil::fontsLoaded());
    QVERIFY(QDir().mkpath(output));
    // Pixel comparisons must not depend on a line edit's cursor blink phase.
    const int cursor_flash_time = QApplication::cursorFlashTime();
    QApplication::setCursorFlashTime(0);
    const auto restore_cursor = interfaces::MakeHandler([cursor_flash_time] {
        QApplication::setCursorFlashTime(cursor_flash_time);
    });
    RestoreAppearance restore;
    const QString classic = qEnvironmentVariable("DEFCON_QT_CLASSIC_REFERENCE");
    if (!classic.isEmpty()) Theme(classic.toUtf8().constData());
    WalletFixture fixture(m_node);
    const auto capture = [&](QWidget& widget, const QString& name, int width = 1280) {
        GUIUtil::updateFonts();
        widget.resize(width, 850);
        widget.show(); Settle();
        if (auto* page = qobject_cast<OverviewPage*>(&widget)) {
            qInfo() << "OVERVIEW_AUDIT" << name << page->size() << "pageMin" << page->minimumWidth();
            for (QLabel* label : page->findChild<QFrame*>("frame")->findChildren<QLabel*>()) {
                for (QTextDocument* document : label->findChildren<QTextDocument*>()) {
                    qInfo() << "LABEL_DOCUMENT_AUDIT" << label->objectName()
                            << label->font().toString() << document->defaultFont().toString()
                            << label->contentsMargins() << label->frameWidth();
                }
            }
            for (const char* object : {"frame", "frame_2", "frameCoinJoin", "labelBalance", "labelTotal", "labelTotalText"}) {
                auto* child = page->findChild<QWidget*>(object);
                qInfo() << "CHILD" << object << OnPage(*page, child) << "min" << child->minimumWidth()
                        << "hint" << child->sizeHint() << "font" << child->font().toString();
            }
        }
        const bool saved = widget.grab().save(QDir(output).filePath(name + ".png"));
        widget.hide();
        return saved;
    };
    OverviewPage overview;
    overview.setClientModel(fixture.client.get());
    overview.setWalletModel(fixture.model.get());
    interfaces::WalletBalances balances{};
    balances.balance = 5082097599LL * COIN / 100;
    overview.setBalance(balances);
    SendCoinsDialog send; send.setModel(fixture.model.get());
    ReceiveCoinsDialog receive; receive.setModel(fixture.model.get());
    const QStringList themes = classic.isEmpty() ? QStringList{"Abyss", "Light", "Dark", "Traditional"} : QStringList{classic};
    for (const QString& name : themes) {
        const QByteArray bytes = name.toUtf8();
        const char* theme = bytes.constData();
        Theme(theme);
        for (int width : {0, 1280, 1920, 2560}) {
            QVERIFY(capture(overview, QString("%1-overview-%2").arg(theme).arg(width ? width : overview.minimumWidth()), width ? width : overview.minimumWidth()));
        }
        QVERIFY(capture(send, QString(theme) + "-send"));
        if (QString(theme) == "Abyss") {
            QVERIFY(capture(receive, "Abyss-receive"));
            for (int scale : {25, 50}) {
                GUIUtil::setFontScale(scale);
                QVERIFY(capture(overview, QString("Abyss-overview-fontscale-%1").arg(scale), 1280));
            }
            GUIUtil::setFontScale(0);
        }
    }
    if (!classic.isEmpty()) return;
    Theme("Abyss");
    AddressBookPage addresses(AddressBookPage::ForEditing, AddressBookPage::SendingTab);
    addresses.setModel(fixture.model->getAddressTableModel());
    QVERIFY(capture(addresses, "Abyss-address-book"));
    SignVerifyMessageDialog sign(nullptr); sign.setModel(fixture.model.get());
    QVERIFY(capture(sign, "Abyss-sign"));
    QVERIFY(QMetaObject::invokeMethod(&sign, "showPage", Q_ARG(int, 1))); QVERIFY(capture(sign, "Abyss-verify"));
    OptionsDialog options(nullptr, true); options.setModel(fixture.options.get());
    QVERIFY(capture(options, "Abyss-options-reset"));
    QVERIFY(QMetaObject::invokeMethod(&options, "showPage", Q_ARG(int, 5))); QVERIFY(capture(options, "Abyss-appearance"));
    CCoinControl control;
    CoinControlDialog coins(control, fixture.model.get());
    QVERIFY(capture(coins, "Abyss-coin-control"));
    RPCConsole debug(m_node, nullptr, Qt::Window);
    // Follow the normal GUI shutdown protocol, including early assertion returns.
    const auto stop_console = interfaces::MakeHandler([&debug] { debug.setClientModel(nullptr); });
    debug.setClientModel(fixture.client.get());
    QVERIFY(capture(debug, "Abyss-debug"));
    OpenURIDialog uri(nullptr);
    QVERIFY(capture(uri, "Abyss-open-uri"));
    TransactionView transactions;
    transactions.setModel(fixture.model.get());
    QVERIFY(capture(transactions, "Abyss-transactions"));
    auto* date = transactions.findChild<QDateTimeEdit*>();
    QVERIFY(date && date->calendarWidget());
    QVERIFY(capture(*date->calendarWidget(), "Abyss-calendar", 700));
    MasternodeList masternodes;
    masternodes.setClientModel(fixture.client.get());
    QVERIFY(capture(masternodes, "Abyss-masternodes"));
    QRDialog qr;
#ifdef USE_QRCODE
    qr.setInfo("Test payment", "defcon:XtestAddress123456789?amount=1234.56789", "Disposable regtest fixture", "Test payment");
    QVERIFY(capture(qr, "Abyss-qr-dialog", 700));
    auto* code = qr.findChild<QRImageWidget*>();
    QVERIFY(code);
    QVERIFY(code->exportImage().save(QDir(output).filePath("Abyss-qr-export.png")));
#endif
    ShutdownWindow shutdown;
    QVERIFY(capture(shutdown, "Abyss-shutdown", 700));
    HelpMessageDialog help(nullptr, HelpMessageDialog::cmdline);
    QVERIFY(capture(help, "Abyss-help"));
}


namespace {
// Deterministic device metrics stand in for different monitor DPRs. Qt's
// actual ScreenChangeInternal event is used below; no global DPI is changed.
template <typename Base>
class DpiWidget : public Base
{
public:
    using Base::Base;
    qreal ratio{1};
protected:
    int metric(QPaintDevice::PaintDeviceMetric value) const override
    {
        if (value == QPaintDevice::PdmDevicePixelRatioScaled) return qRound(ratio * QPaintDevice::devicePixelRatioFScale());
        if (value == QPaintDevice::PdmDevicePixelRatio) return qRound(ratio);
        return Base::metric(value);
    }
};

class ControlledSky : public StarfieldWidget
{
public:
    qint64 elapsed{0};
    qint64 m_epoch_utc_ms{0}; //!< The system clock at elapsed 0.
    qint64 wall_step{0};      //!< A correction of the system clock alone.
    mutable int utc_reads{0}; //!< How often the sky read the system clock.
    int slot_turns_after{-1}; //!< After this many reads the clock is turn_ms further on.
    qint64 turn_ms{600000};   //!< One ten-minute slot, or a small step across a turn.
protected:
    qint64 clockMilliseconds() const override { return elapsed; }
    qint64 utcMilliseconds() const override
    {
        ++utc_reads;
        return m_epoch_utc_ms + elapsed + wall_step + (slot_turns_after >= 0 && utc_reads > slot_turns_after ? turn_ms : 0);
    }
};
} // namespace


void AbyssTests::skySuspendClock()
{
    StarfieldWidget live;
    QVERIFY(live.clockMilliseconds() < 100);
    QVERIFY(live.clockMilliseconds() >= 0);
#if defined(Q_OS_LINUX) && defined(CLOCK_BOOTTIME)
    timespec boot{};
    QCOMPARE(clock_gettime(CLOCK_BOOTTIME, &boot), 0);
    const qint64 boot_ms = static_cast<qint64>(boot.tv_sec) * 1000 + boot.tv_nsec / 1000000;
    QVERIFY(std::abs(StarfieldWidget::suspendAwareClockMilliseconds() - boot_ms) < 100);
#endif
    QVERIFY(live.m_stars.isEmpty());
    QVERIFY(live.m_steady.isNull() && live.m_moon.isNull());

    // The real sky and the Moon follow the system clock: a sleep moves them on,
    // and so does a corrected clock, at the next tick. Motion keeps the
    // suspend-aware clock and never jumps with the system clock.
    ControlledSky sky;
    sky.m_epoch_utc_ms = 1792173600000LL;
    sky.resize(1280, 720);
    sky.setSkyVisible(true);
    sky.setSkyTier(1);
    sky.show();
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();
    sky.redrawSteadySky();
    sky.onTick();
    QCOMPARE(sky.phaseTimeMilliseconds(), sky.m_epoch_utc_ms + sky.elapsed);
    sky.elapsed += 8 * 3600000LL;
    sky.onTick();
    sky.grab();
    QCOMPARE(sky.phaseTimeMilliseconds(), sky.m_epoch_utc_ms + sky.elapsed);
    QCOMPARE(sky.motionMilliseconds(), sky.elapsed);
    const qint64 bucket = sky.m_sky_bucket;
    QCOMPARE(bucket, (sky.m_epoch_utc_ms + sky.elapsed) / 600000);
    const int redraws = sky.m_steady_redraws;
    sky.wall_step = 3600000LL; // The system clock is put an hour forward.
    sky.onTick();
    sky.grab();
    QCOMPARE(sky.phaseTimeMilliseconds(), sky.m_epoch_utc_ms + sky.elapsed + sky.wall_step);
    QCOMPARE(sky.m_sky_bucket, bucket + 6);
    QCOMPARE(sky.m_steady_redraws, redraws + 1);
    QCOMPARE(sky.motionMilliseconds(), sky.elapsed);
    // Off with a cache of the current slot keeps the picture as it is.
    sky.setAnimated(false);
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, redraws + 1);
    const qint64 frozen = sky.motionMilliseconds();
    const qint64 phase = sky.phaseTimeMilliseconds();
    sky.elapsed += 8 * 3600000LL;
    sky.wall_step -= 600000LL;
    QCOMPARE(sky.motionMilliseconds(), frozen);
    QCOMPARE(sky.phaseTimeMilliseconds(), phase);
    sky.setAnimated(true);
    QCOMPARE(sky.motionMilliseconds(), frozen);
    QCOMPARE(sky.phaseTimeMilliseconds(), sky.m_epoch_utc_ms + sky.elapsed + sky.wall_step);
    // Off again before any tick, as from the settings dialog while the main
    // window is inactive: the cache still shows the slot of eight hours ago,
    // so it is drawn once for the time it stops at, and then stays.
    QVERIFY(sky.m_sky_bucket != sky.skyBucket());
    sky.setAnimated(false);
    sky.grab();
    QCOMPARE(sky.m_sky_bucket, sky.m_frozen_utc_ms / 600000);
    QCOMPARE(sky.m_steady_redraws, redraws + 2);
    sky.elapsed += 3600000LL;
    sky.wall_step += 3600000LL;
    sky.onTick();
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, redraws + 2);
}

void AbyssTests::presentationIcons()
{
    RestoreAppearance restore;
    const bool high_dpi = QCoreApplication::testAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    const auto restore_dpi = interfaces::MakeHandler([high_dpi] { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, high_dpi); });
    SendCoinsEntry entry;
    entry.show(); Settle();
    struct ButtonIcon { const char* object; const char* classic; const char* glyph; GUIUtil::ThemedColor tint; };
    const std::vector<ButtonIcon> icons{
        {"addressBookButton", "address-book", "address-book", GUIUtil::ThemedColor::BLUE},
        {"pasteButton", "editpaste", "clipboard", GUIUtil::ThemedColor::BLUE},
        {"deleteButton", "remove", "trash", GUIUtil::ThemedColor::RED},
        {"deleteButton_is", "remove", "trash", GUIUtil::ThemedColor::RED},
        {"deleteButton_s", "remove", "trash", GUIUtil::ThemedColor::RED},
    };
    for (const char* theme : {"Light", "Abyss", "Dark", "Abyss", "Traditional"}) {
        Theme(theme); Settle();
        for (const auto& spec : icons) {
            auto* button = entry.findChild<QAbstractButton*>(spec.object);
            QVERIFY(button);
            const QSize size = button->iconSize();
            const QImage shown = button->icon().pixmap(entry.windowHandle(), size).toImage();
            QVERIFY(!shown.isNull());
            const QIcon expected = GUIUtil::isModernTheme() ?
                QIcon(GUIUtil::getTintedSvgPixmap(QString(":/icons/tabler/") + spec.glyph + ".svg", size, GUIUtil::getThemedQColor(spec.tint))) :
                GUIUtil::getIcon(spec.classic, spec.tint);
            QCOMPARE(shown, expected.pixmap(entry.windowHandle(), size).toImage());
            QCOMPARE(button->accessibleName(), GUIUtil::isModernTheme() ? button->toolTip() : QString());
        }
    }
    Theme("Abyss");
    // Every newly shipped glyph must really decode and draw, including all
    // connection/staking states. Resource presence alone is insufficient.
    for (const char* glyph : {"settings", "eye", "eye-off", "lock", "lock-open", "terminal-2", "address-book", "clipboard", "trash",
                             "antenna-bars-1", "antenna-bars-2", "antenna-bars-3", "antenna-bars-4", "antenna-bars-5",
                             "network-off", "leaf", "leaf-off", "clock-pause", "signature"}) {
        const QPixmap px = GUIUtil::getTintedSvgPixmap(QString(":/icons/tabler/") + glyph + ".svg", QSize(22, 22), QColor("#1297ff"));
        int ink = 0;
        const QImage image = px.toImage();
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) if (qAlpha(image.pixel(x, y))) ++ink;
        QVERIFY2(ink > 20, glyph);
        QCOMPARE(px.devicePixelRatioF(), qApp->devicePixelRatio());
    }
}

void AbyssTests::iconPixelRatios()
{
    RestoreAppearance restore;
    Theme("Abyss");
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    BitcoinGUI window(m_node, style.get());
    DpiWidget<QLabel> status(&window);
    const auto original = window.labelStakingIcon;
    window.labelStakingIcon = &status;
    const auto restore_label = interfaces::MakeHandler([&] { window.labelStakingIcon = original; });
    status.ratio = qFuzzyCompare(qApp->devicePixelRatio(), qreal(1)) ? 2 : 1;
    QCOMPARE(status.devicePixelRatioF(), status.ratio);
    window.setStakingStatus();
    QCOMPARE(status.pixmap(Qt::ReturnByValue).devicePixelRatioF(), status.ratio);
    QCOMPARE(status.pixmap(Qt::ReturnByValue).size(), QSize(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE) * status.ratio);
    const qint64 key = status.pixmap(Qt::ReturnByValue).cacheKey();
    window.setStakingStatus();
    QCOMPARE(status.pixmap(Qt::ReturnByValue).cacheKey(), key);

    const bool high_dpi = QCoreApplication::testAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, false);
    const auto restore_dpi = interfaces::MakeHandler([high_dpi] { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, high_dpi); });
    DpiWidget<QToolButton> button;
    const QSize size(22, 22);
    const QColor tint("#1297ff");
    const QString resource(":/icons/tabler/home.svg");
    GUIUtil::setTintedSvgIcon(&button, resource, size, tint, true);
    QSignalSpy clicked(&button, &QToolButton::clicked);
    const QString tooltip = status.toolTip();
    status.hide();
    const bool enabled = status.isEnabled();
    QMap<qreal, qint64> cache_keys;
    for (const qreal ratio : {1.0, 1.25, 1.5, 2.0, 1.0}) {
        status.ratio = ratio;
        button.ratio = ratio;
        QEvent screen(QEvent::ScreenChangeInternal);
        QApplication::sendEvent(&status, &screen);
        QApplication::sendEvent(&button, &screen);
        const QPixmap image = status.pixmap(Qt::ReturnByValue);
        QCOMPARE(image.devicePixelRatioF(), ratio);
        QCOMPARE(image.size(), QSize(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE) * ratio);
        QCOMPARE(status.toolTip(), tooltip);
        QVERIFY(status.isHidden());
        QCOMPARE(status.isEnabled(), enabled);
        if (cache_keys.contains(ratio)) QCOMPARE(image.cacheKey(), cache_keys[ratio]);
        cache_keys[ratio] = image.cacheKey();
        for (const auto state : {QIcon::Off, QIcon::On}) {
            QPixmap actual = button.icon().pixmap(size * ratio, QIcon::Normal, state);
            actual.setDevicePixelRatio(ratio);
            const QPixmap expected = GUIUtil::getTintedSvgPixmap(resource, size, state == QIcon::On ? QColor(Qt::white) : tint, ratio);
            QCOMPARE(actual.toImage(), expected.toImage());
        }
        QCOMPARE(button.findChildren<GUIUtil::SvgIconFilter*>().size(), 1);
        QCOMPARE(status.findChildren<GUIUtil::SvgIconFilter*>().size(), 1);
    }
    QCOMPARE(clicked.count(), 0); // A screen change never dispatches an action.
    const qint64 stable_button = button.icon().cacheKey();
    QEvent show(QEvent::Show);
    QApplication::sendEvent(&button, &show);
    QCOMPARE(button.icon().cacheKey(), stable_button); // No same-DPR layout churn.
    button.ratio = 1.5;
    QApplication::sendEvent(&button, &show);
    QVERIFY(button.icon().availableSizes().contains(size * 1.5));
    button.ratio = 2;
    button.setParent(&window); // ParentChange must follow a late target window.
    QVERIFY(button.icon().availableSizes().contains(size * 2));

    // Reproduce a direct spinner/status writer after a prior SVG state.
    QPixmap external(13, 11);
    external.fill(Qt::magenta);
    status.setPixmap(external);
    button.setIcon(QIcon(external));
    const qint64 external_button = button.icon().cacheKey();
    status.ratio = button.ratio = 1.25;
    QEvent changed(QEvent::ScreenChangeInternal);
    QApplication::sendEvent(&status, &changed);
    QApplication::sendEvent(&button, &changed);
    QCOMPARE(status.pixmap(Qt::ReturnByValue).cacheKey(), external.cacheKey());
    QCOMPARE(button.icon().cacheKey(), external_button);

#ifndef Q_OS_MAC
    DpiWidget<QToolButton> console(&window);
    const auto old_console = window.consoleButton;
    window.consoleButton = &console;
    const auto restore_console = interfaces::MakeHandler([&] { window.consoleButton = old_console; });
    console.ratio = 1.5;
    window.eventFilter(&window, &show);
    QVERIFY(console.icon().availableSizes().contains(size * 1.5));
    console.ratio = 2;
    window.eventFilter(&window, &changed);
    QVERIFY(console.icon().availableSizes().contains(size * 2));
#endif
    status.clear();
    QEvent screen(QEvent::ScreenChangeInternal);
    QApplication::sendEvent(&status, &screen);
    QVERIFY(!GUIUtil::HasPixmap(&status)); // Do not revive a cleared status.
    window.setStakingStatus();
    Theme("Light"); Settle();
    window.setStakingStatus();
    const QPixmap classic = status.pixmap(Qt::ReturnByValue);
    GUIUtil::setIcon(&button, "editpaste");
    const qint64 classic_button_key = button.icon().cacheKey();
    status.ratio = button.ratio = 2;
    QApplication::sendEvent(&status, &screen);
    QApplication::sendEvent(&button, &screen);
    QCOMPARE(status.pixmap(Qt::ReturnByValue).cacheKey(), classic.cacheKey());
    QCOMPARE(button.icon().cacheKey(), classic_button_key);
    Theme("Abyss"); Settle();
    window.setStakingStatus();
    QCOMPARE(status.pixmap(Qt::ReturnByValue).devicePixelRatioF(), qreal(2));
}

void AbyssTests::initialWindowGeometry_data()
{
    QTest::addColumn<bool>("tools");
    QTest::newRow("main") << false;
    QTest::newRow("tools") << true;
}

void AbyssTests::initialWindowGeometry()
{
    QFETCH(bool, tools);
    if (!GUIUtil::fontsLoaded()) QSKIP("Window geometry requires a font-capable platform");
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    if (available.width() < 1100 || available.height() < 700) {
        QSKIP("Run the real-window geometry regression on a desktop-sized screen; minimal is 240x320");
    }
    RestoreAppearance restore;
    Theme("Abyss");
    QSettings().remove("MainWindowGeometry");
    QSettings().remove("RPCConsoleWindowGeometry");
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);
    const auto create = [&]() -> std::unique_ptr<QWidget> {
        std::unique_ptr<QWidget> window;
        if (tools) {
            window = std::make_unique<RPCConsole>(m_node, nullptr, Qt::Window);
        } else {
            auto main = std::make_unique<BitcoinGUI>(m_node, style.get());
            interfaces::BlockAndHeaderTipInfo tip{};
            main->setClientModel(fixture.client.get(), &tip);
            main->addWallet(fixture.model.get());
            window = std::move(main);
        }
        // WSLg's host WM shifts even a plain QWidget::move by its decoration
        // offset. Exercise Qt layout/placement without that external policy.
        // Decorated frame/DPI acceptance is a separate native Windows trial.
        if (QGuiApplication::platformName() == "xcb") window->setWindowFlag(Qt::BypassWindowManagerHint);
        return window;
    };
    auto window = create();
    const auto detach = interfaces::MakeHandler([&] {
        if (auto* main = qobject_cast<BitcoinGUI*>(window.get())) main->setClientModel(nullptr);
    });
    window->show(); Settle();
    QTRY_VERIFY(window->windowHandle() && window->windowHandle()->isExposed());
    qInfo() << "INITIAL_GEOMETRY" << (tools ? "tools" : "main") << available << window->frameGeometry() << window->geometry() << GUIUtil::getActiveTheme() << "DPR" << window->devicePixelRatioF();
    QTRY_VERIFY2(available.contains(window->frameGeometry()), "Fresh window frame must fit the available screen without maximizing");
    QTRY_VERIFY2((window->frameGeometry().center() - available.center()).manhattanLength() <= 4,
             "Center the final layout, not an earlier constructor size");

    if (!tools) {
        // A late layout request (for example after wallet loading) must also
        // clamp an enlarged window, without treating it as a fresh placement.
        window->move(available.bottomRight() - QPoint(40, 40));
        window->setMinimumSize(1, 1); // Force the next layout to publish its actual minimum.
        QApplication::postEvent(window.get(), new QEvent(QEvent::LayoutRequest));
        QTRY_VERIFY(available.contains(window->frameGeometry()));
    }

    // A valid user placement survives both hide/show and save/restore.
    const QPoint inset(std::min(20, available.width() - window->frameGeometry().width()),
                       std::min(20, available.height() - window->frameGeometry().height()));
    window->move(window->pos() + available.topLeft() + inset - window->frameGeometry().topLeft());
    Settle();
    const QRect placed = window->geometry();
    QVERIFY(available.contains(window->frameGeometry()));
    window->hide(); window->show(); Settle();
    QTRY_COMPARE(window->geometry(), placed);
    window->hide();
    if (auto* main = qobject_cast<BitcoinGUI*>(window.get())) main->setClientModel(nullptr);
    window.reset(); // Save geometry through the real destructor.
    window = create(); window->show(); Settle();
    QTRY_COMPARE(window->geometry(), placed);
    QVERIFY(available.contains(window->frameGeometry()));
    window->showMaximized(); Settle();
    QVERIFY(window->isMaximized());
    window->showNormal(); Settle();
    QTRY_COMPARE(window->geometry(), placed);
    window->showMinimized(); Settle();
    QVERIFY(window->isMinimized());
    window->showNormal(); Settle();
    QTRY_COMPARE(window->geometry(), placed);
}

void AbyssTests::statusBarOnce()
{
    // Abyss says the sync state once, in the Overview's network bar, and has
    // the wallet lock in its quick row: the status bar's own sync text,
    // progress bar and lock stay with the classic themes.
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);
    Theme("Light");
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.addWallet(fixture.model.get());
    window.resize(1280, 850); window.show(); Settle();
    auto* text = window.findChild<QLabel*>("lblStatusBarProgress");
    auto* bar = window.statusBar()->findChild<QProgressBar*>();
    auto* lock = window.findChild<QLabel*>("labelWalletEncryptionIcon");
    QVERIFY(text && bar && lock);
    // What the classic status bar shows follows the node: the text while the
    // network is on and the node is catching up or alone, the bar only with peers.
    const bool classic_text = m_node.getNetworkActive() && (!m_node.masternodeSync().isSynced() || fixture.client->getNumConnections() == 0);
    const bool classic_bar = m_node.getNetworkActive() && !m_node.masternodeSync().isSynced() && fixture.client->getNumConnections() > 0;
    QCOMPARE(text->isVisible(), classic_text);
    QCOMPARE(bar->isVisible(), classic_bar);
    QVERIFY(lock->isVisible());
    if (!classic_text) QSKIP("The test node shows no sync text that Abyss could hide");
    Theme("Abyss"); Settle();
    QVERIFY(!text->isVisible());
    QVERIFY(!bar->isVisible());
    QVERIFY(!lock->isVisible());
    QVERIFY(window.findChild<QToolButton*>("walletLockButton")->isVisible());
    QVERIFY(window.findChild<QFrame*>("networkCard")->isVisible());
    // The detailed sync window still opens from the block icon's slot after the
    // user closed it: with the status bar's progress bar hidden, the gate is
    // the node's state (GPT-Terra r1 R1). The test node is not synced.
    auto* overlay = window.findChild<ModalOverlay*>();
    QVERIFY(overlay && m_node.getNetworkActive() && !m_node.masternodeSync().isSynced());
    if (overlay->isLayerVisible()) overlay->closeClicked();
    Settle();
    QVERIFY(!overlay->isLayerVisible());
    window.showModalOverlay(); Settle();
    QVERIFY(overlay->isLayerVisible());
    overlay->closeClicked(); Settle();
    QVERIFY(!overlay->isLayerVisible());
    // A later wallet state update keeps them hidden.
    QMetaObject::invokeMethod(&window, "updateWalletStatus", Qt::DirectConnection); Settle();
    QVERIFY(!lock->isVisible());
    Theme("Light"); Settle();
    QCOMPARE(text->isVisible(), classic_text);
    QCOMPARE(bar->isVisible(), classic_bar);
    QVERIFY(lock->isVisible());
    // The classic gate is the progress bar, as before: without peers it stays closed.
    window.showModalOverlay(); Settle();
    QCOMPARE(overlay->isLayerVisible(), classic_bar);
}

void AbyssTests::shortcutUnderlines()
{
    // Abyss hides the Alt-key underlines in button and label text; a classic
    // theme gets the platform style's own answer back. The keys still work.
    RestoreAppearance restore;
    Theme("Light");
    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    auto* with = new QToolButton(&host);
    with->setText("&Send");
    with->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* without = new QToolButton(&host);
    without->setText("Send");
    without->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* label = new QLabel("&Label:", &host);
    for (QWidget* widget : {static_cast<QWidget*>(with), static_cast<QWidget*>(without), static_cast<QWidget*>(label)}) layout->addWidget(widget);
    host.show(); QApplication::setActiveWindow(&host); Settle();
    const int classic_hint = qApp->style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr, with);
    const auto same_picture = [&] { Settle(); return with->grab().toImage() == without->grab().toImage(); };
    QCOMPARE(with->size(), without->size());
    QCOMPARE(same_picture(), classic_hint == 0);
    Theme("Abyss");
    QCOMPARE(qApp->style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr, with), 0);
    QCOMPARE(qApp->style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr, label), 0);
    QVERIFY(same_picture());
    // The mnemonic is still registered: hiding the underline changes nothing
    // about the shortcut (minimal delivers no Alt+S to the shortcut map, so
    // the registration is what can be checked here).
    QCOMPARE(with->shortcut(), QKeySequence::mnemonic("&Send"));
    QVERIFY(without->shortcut().isEmpty());
    Theme("Light");
    QCOMPARE(qApp->style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr, with), classic_hint);
    QCOMPARE(same_picture(), classic_hint == 0);
}

void AbyssTests::buttonRows()
{
    // A minimum height above the maximum fails quietly: the layout places a
    // widget by its maximum and Qt then draws it at its minimum, so it hangs
    // below its row and the parent cuts off what sticks out. That took the
    // bottom edge off OK and Cancel in the passphrase dialog.
    RestoreAppearance restore;
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);

    // The dialog as reported: the buttons keep their height, inside their row,
    // and a classic theme gets its own limits back.
    Theme("Light");
    AskPassphraseDialog unlock(AskPassphraseDialog::Unlock, nullptr);
    unlock.show(); Settle();
    auto* box = unlock.findChild<QDialogButtonBox*>("buttonBox");
    QVERIFY(box);
    const QList<QAbstractButton*> buttons = box->buttons();
    QCOMPARE(buttons.size(), 2);
    const int classic_height = buttons[0]->height();
    Theme("Abyss"); Settle();
    for (QAbstractButton* button : buttons) {
        QCOMPARE(button->height(), 42); // 34 of content, 3 + 3 of padding, 1 + 1 of border.
        QCOMPARE(button->minimumHeight(), button->maximumHeight());
        QVERIFY(box->rect().contains(button->geometry()));
    }
    Theme("Light"); Settle();
    for (QAbstractButton* button : buttons) {
        QCOMPARE(button->height(), classic_height);
        QVERIFY(box->rect().contains(button->geometry()));
    }
    unlock.hide();

    // Everywhere else: every widget of each window, shown or not, and every
    // button that is on show.
    Theme("Abyss");
    const auto problems = [](QWidget& window, bool check_overflow) {
        QStringList found;
        QList<QWidget*> widgets = window.findChildren<QWidget*>();
        widgets.prepend(&window);
        for (QWidget* widget : widgets) {
            widget->ensurePolished();
            const QString name = QString("%1#%2").arg(widget->metaObject()->className(), widget->objectName());
            // Inherited and in every theme, not an Abyss rule: general.css gives the
            // debug window's ban heading a 30px minimum plus a 15px margin over the
            // form's 32px maximum. On the polish list; not this patch.
            if (widget->objectName() == "banHeading") continue;
            if (widget->minimumHeight() > widget->maximumHeight()) {
                found << QString("%1 height %2 > %3").arg(name).arg(widget->minimumHeight()).arg(widget->maximumHeight());
            }
            if (widget->minimumWidth() > widget->maximumWidth()) {
                found << QString("%1 width %2 > %3").arg(name).arg(widget->minimumWidth()).arg(widget->maximumWidth());
            }
            if (check_overflow && qobject_cast<QPushButton*>(widget) && !widget->isWindow() && widget->isVisible() &&
                !widget->parentWidget()->rect().contains(widget->geometry())) {
                found << QString("%1 outside its parent").arg(name);
            }
        }
        return found.join("; ");
    };
    AskPassphraseDialog encrypt(AskPassphraseDialog::Encrypt, nullptr);
    AskPassphraseDialog unlock_mixing(AskPassphraseDialog::UnlockMixing, nullptr);
    AskPassphraseDialog change(AskPassphraseDialog::ChangePass, nullptr);
    QMessageBox message(QMessageBox::Warning, "Warning", "A question.", QMessageBox::Ok | QMessageBox::Cancel);
    // The appearance page lists the loaded family's weights and asserts
    // without fonts (appearanceFonts skips for the same reason): on the
    // font-less platform the sweep leaves this one dialog out.
    std::unique_ptr<OptionsDialog> options;
    if (GUIUtil::fontsLoaded()) {
        options = std::make_unique<OptionsDialog>(nullptr, true);
        options->setModel(fixture.options.get());
    }
    SignVerifyMessageDialog sign(nullptr);
    sign.setModel(fixture.model.get());
    OpenURIDialog uri(nullptr);
    CCoinControl control;
    CoinControlDialog coins(control, fixture.model.get());
    HelpMessageDialog help(nullptr, HelpMessageDialog::cmdline);
    QRDialog qr;
    AddressBookPage addresses(AddressBookPage::ForEditing, AddressBookPage::SendingTab);
    addresses.setModel(fixture.model->getAddressTableModel());
    SendCoinsDialog send;
    send.setModel(fixture.model.get());
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    TransactionView transactions;
    transactions.setModel(fixture.model.get());
    BitcoinGUI main_window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    main_window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&main_window] { main_window.setClientModel(nullptr); });
    main_window.addWallet(fixture.model.get());
    for (QWidget* page : QList<QWidget*>{&addresses, &send, &receive, &transactions, &main_window}) {
        page->resize(1280, 850);
    }
    QList<QWidget*> windows{&unlock, &encrypt, &unlock_mixing, &change, &message, &sign, &uri, &coins,
                            &help, &qr, &addresses, &send, &receive, &transactions, &main_window};
    if (options) windows.insert(5, options.get());
    for (QWidget* window : windows) {
        window->show(); Settle();
        // The main window clamps itself to the screen; on minimal's 240x320 its
        // cards are squeezed below their content, which says nothing about a
        // row cutting its button off (initialWindowGeometry skips for the same
        // reason). The limits are checked regardless of size.
        const bool squeezed = window == &main_window && window->size() != QSize(1280, 850);
        if (squeezed) qInfo() << "main window is" << window->size() << ": its overflow check needs a larger screen";
        const QString found = problems(*window, !squeezed);
        QVERIFY2(found.isEmpty(), qPrintable(QString("%1: %2").arg(window->metaObject()->className(), found)));
        window->hide();
    }

    // Opt-in pictures of what was reported and of the taller buttons.
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        const auto capture = [&](QWidget& window, const char* name) {
            window.show(); Settle();
            const bool saved = window.grab().save(QDir(output).filePath(QString("Abyss-buttons-%1.png").arg(name)));
            window.hide();
            return saved;
        };
        QVERIFY(capture(unlock, "unlock"));
        QVERIFY(capture(message, "message"));
        QVERIFY(capture(receive, "receive"));
        QVERIFY(capture(send, "send"));
    }
}

void AbyssTests::walletGoneWhileAdding()
{
    // Building a wallet's view spins the event loop (SendCoinsDialog::addEntry),
    // and that can deliver a queued unload: createwallet followed at once by
    // unloadwallet deleted the model under a view still being built, and the view
    // then crashed on it. The unload here is the real one, queued to the real
    // controller before the window starts building views.
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);
    NodeContext& context = *m_node.context();
    const auto loader = interfaces::MakeWalletLoader(*context.chain, context.coinjoin_loader, *Assert(context.args));
    context.wallet_loader = loader.get();
    auto second = std::make_shared<CWallet>(context.chain.get(), context.coinjoin_loader.get(), "second", CreateMockWalletDatabase());
    AddWallet(second);
    second->LoadWallet();
    second->GetOrCreateLegacyScriptPubKeyMan();
    // Before the loader goes: it would wait for the release of the wallets still
    // registered with it, and this test holds on to both.
    const auto forget = interfaces::MakeHandler([&] {
        context.wallet_loader = nullptr;
        RemoveWallet(second, std::nullopt);
        RemoveWallet(fixture.wallet, std::nullopt);
    });

    for (const char* theme : {"Abyss", "Light"}) {
        Theme(theme);
        // The wallet whose view is being built, and the next one in line.
        for (const size_t doomed : {size_t{0}, size_t{1}}) {
            BitcoinGUI window(m_node, style.get());
            interfaces::BlockAndHeaderTipInfo tip{};
            window.setClientModel(fixture.client.get(), &tip);
            WalletController controller(*fixture.client, nullptr);
            const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
            int removed = 0;
            QObject::connect(&controller, &WalletController::walletRemoved, [&removed] { ++removed; });
            const std::vector<WalletModel*> models = controller.getOpenWallets();
            QCOMPARE(models.size(), size_t{2});
            WalletModel* const survivor = models[1 - doomed];

            Q_EMIT models[doomed]->unload();
            QCOMPARE(removed, 0);
            window.setWalletController(&controller);
            // Nothing here spun the loop: only the building of a view delivered it.
            QCOMPARE(removed, 1);
            QVERIFY(controller.getOpenWallets() == std::vector<WalletModel*>{survivor});
            QList<WalletView*> views = window.findChildren<WalletView*>();
            QCOMPARE(views.size(), 1);
            QCOMPARE(views[0]->getWalletModel(), survivor);
            Settle();

            // The one left behaves as ever: its own unload takes its view with it.
            Q_EMIT survivor->unload();
            Settle();
            QCOMPARE(removed, 2);
            QVERIFY(controller.getOpenWallets().empty());
            QVERIFY(window.findChildren<WalletView*>().isEmpty());
        }
    }
}

//! A page as the window shows it: Abyss pages leave their background to the sky behind them.
static QPixmap PageImage(QWidget& page)
{
    QPixmap picture = page.grab();
    if (!GUIUtil::isModernTheme()) return picture;
    picture.fill(QColor("#050d1a"));
    page.render(&picture, QPoint(), QRegion(), QWidget::DrawChildren);
    return picture;
}

void AbyssTests::pageHeaders()
{
    // Every page says where you are the same way: its name and what it is for,
    // at the same place and in the same type, so that nothing jumps when moving
    // between pages. The classic themes have no such header.
    RestoreAppearance restore;
    QSettings settings;
    const QStringList tabs{"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab"};
    std::map<QString, QVariant> saved_tabs;
    for (const QString& tab : tabs) {
        saved_tabs.emplace(tab, settings.value(tab));
        settings.setValue(tab, true);
    }
    const auto restore_tabs = interfaces::MakeHandler([saved_tabs] {
        QSettings settings;
        for (const auto& [tab, value] : saved_tabs) {
            if (value.isValid()) settings.setValue(tab, value); else settings.remove(tab);
        }
    });
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const auto save = [&output](QWidget* page, const QString& name) {
        return output.isEmpty() || (QDir().mkpath(output) && PageImage(*page).save(QDir(output).filePath(name + ".png")));
    };
    const auto named = [](QWidget* page, const char* a, const char* b) -> QLabel* {
        for (QLabel* label : page->findChildren<QLabel*>()) {
            if (label->objectName() == a || label->objectName() == b) return label;
        }
        return nullptr;
    };
    const auto title_of = [&named](QWidget* page) { return named(page, "abyssPageTitle", "modernOverviewTitle"); };
    const auto subtitle_of = [&named](QWidget* page) { return named(page, "abyssPageSubtitle", "modernOverviewSubtitle"); };

    Theme("Light");
    WalletView view(nullptr);
    // Larger than any page's minimum in any theme, with or without fonts, so that
    // the view never changes size: a table's stretched last column keeps its width
    // when a view narrows in one go, whatever the theme.
    const QSize view_size{1800, 1000};
    view.resize(view_size);
    view.show(); Settle();
    // A wallet with CoinJoin has this card; without a wallet model it stays hidden.
    view.widget(0)->findChild<QFrame*>("frameCoinJoin")->setVisible(true);
    const QStringList titles{"Overview", "Transactions", "Receive DeFCoN", "Send DeFCoN", "CoinJoin", "Multisig", "Masternodes", "Governance"};
    QCOMPARE(view.count(), titles.size());
    // Which field has the keyboard depends on the way through the pages, not on
    // the theme, and a field with the keyboard looks different.
    const auto open = [&view](QWidget* page) {
        view.setCurrentWidget(page); Settle();
        if (QWidget* focus = QApplication::focusWidget()) focus->clearFocus();
        Settle();
    };
    // In a classic theme the pages keep the margins they have on their own, and
    // the empty Multisig page's note takes no free room.
    MasternodeList own_masternodes;
    GovernanceList own_governance;
    const auto classic_layouts = [&] {
        QCOMPARE(view.widget(6)->layout()->contentsMargins(), own_masternodes.layout()->contentsMargins());
        QCOMPARE(view.widget(7)->layout()->contentsMargins(), own_governance.layout()->contentsMargins());
        auto* multisig_box = qobject_cast<QBoxLayout*>(view.widget(5)->layout());
        QVERIFY(multisig_box);
        for (QLabel* label : view.widget(5)->findChildren<QLabel*>()) {
            const int item = multisig_box->indexOf(label);
            if (item >= 0) QCOMPARE(multisig_box->stretch(item), 0);
        }
    };
    classic_layouts();
    std::vector<QImage> classic;
    for (int i = 0; i < view.count(); ++i) {
        QWidget* page = view.widget(i);
        open(page);
        QLabel* title = title_of(page);
        QVERIFY2(!title || !title->isVisible(), qPrintable(titles[i]));
        QVERIFY(save(page, QString("Light-page-%1").arg(i)));
        classic.push_back(page->grab().toImage());
    }

    Theme("Abyss"); Settle();
    std::optional<QPoint> corner;
    std::optional<QFont> title_font, subtitle_font;
    for (int i = 0; i < view.count(); ++i) {
        QWidget* page = view.widget(i);
        open(page);
        QLabel* title = title_of(page);
        QLabel* subtitle = subtitle_of(page);
        QVERIFY2(title && subtitle, qPrintable(titles[i]));
        QVERIFY2(title->isVisible() && subtitle->isVisible(), qPrintable(titles[i]));
        QCOMPARE(title->text(), titles[i]);
        QVERIFY2(subtitle->text().endsWith('.'), qPrintable(subtitle->text()));
        QVERIFY(save(page, QString("Abyss-page-%1").arg(i)));
        const QPoint position = OnPage(*page, title).topLeft();
        qInfo() << "PAGE_HEADER" << titles[i] << position << OnPage(*page, subtitle) << title->font().toString() << subtitle->font().toString();
        if (!corner) {
            corner = position;
            title_font = title->font();
            subtitle_font = subtitle->font();
        }
        QCOMPARE(position, *corner);
        QVERIFY2(title->font() == *title_font, qPrintable(titles[i] + ": " + title->font().toString() + " vs " + title_font->toString()));
        QVERIFY2(subtitle->font() == *subtitle_font, qPrintable(titles[i] + ": " + subtitle->font().toString() + " vs " + subtitle_font->toString()));
        // The header keeps together, however much room the page has to spare.
        QVERIFY2(OnPage(*page, subtitle).top() - OnPage(*page, title).bottom() <= 10, qPrintable(titles[i]));
    }
    // An empty Multisig page keeps its buttons under the header.
    QWidget* multisig = view.widget(5);
    open(multisig);
    QVERIFY(OnPage(*multisig, multisig->findChild<QPushButton*>("multisigCreate")).top() < multisig->height() / 3);

    // The same in a wallet that starts in Abyss: a page built before it joins the
    // view gets a window's margins from the style, not a child's.
    {
        WalletView started(nullptr);
        started.resize(view_size);
        started.show(); Settle();
        for (int i = 0; i < started.count(); ++i) {
            QWidget* page = started.widget(i);
            started.setCurrentWidget(page); Settle();
            QVERIFY2(OnPage(*page, title_of(page)).topLeft() == *corner, qPrintable(titles[i]));
        }
    }

    // Back in a classic theme: every page as it was, to the pixel.
    Theme("Light"); Settle();
    QCOMPARE(view.size(), view_size);
    classic_layouts();
    QStringList differing;
    for (int i = 0; i < view.count(); ++i) {
        QWidget* page = view.widget(i);
        open(page);
        QLabel* title = title_of(page);
        QVERIFY2(!title || !title->isVisible(), qPrintable(titles[i]));
        if (page->grab().toImage() != classic[i]) differing << titles[i];
    }
    QVERIFY2(differing.isEmpty(), qPrintable(differing.join(", ")));
}

void AbyssTests::primaryButtons()
{
    // One blue button to a page at most, so that the page says what it is for.
    // The rest take the dark secondary style.
    RestoreAppearance restore;
    QSettings settings;
    const QVariant multisig_tab = settings.value("fShowMultisigTab");
    settings.setValue("fShowMultisigTab", true);
    const auto restore_tab = interfaces::MakeHandler([multisig_tab] {
        QSettings settings;
        if (multisig_tab.isValid()) settings.setValue("fShowMultisigTab", multisig_tab); else settings.remove("fShowMultisigTab");
    });
    WalletFixture fixture(m_node);
    Theme("Abyss");
    WalletView view(nullptr);
    view.setClientModel(fixture.client.get());
    view.setWalletModel(fixture.model.get());
    view.showOutOfSyncWarning(false);
    view.resize(1042, 850);
    view.show(); Settle();
    auto* coinjoin = view.widget(0)->findChild<QFrame*>("frameCoinJoin");
    auto* toggle = view.widget(0)->findChild<QPushButton*>("toggleCoinJoin");
    QVERIFY(coinjoin && toggle);
    coinjoin->setEnabled(true);
    view.widget(0)->findChild<QPushButton*>("coinJoinRowShow")->click(); // Keeps it open across the status timer.
    coinjoin->show();
    toggle->setEnabled(true);
    const QColor primary("#0f75d6");
    const auto blue = [&primary](QWidget* page) {
        QStringList found;
        for (QPushButton* button : page->findChildren<QPushButton*>()) {
            if (!button->isVisible() || !button->isEnabled()) continue;
            const QImage image = button->grab().toImage();
            // Inside the border, left of the text.
            if (image.pixelColor(6, image.height() / 2) == primary) found << button->objectName();
        }
        return found;
    };
    const std::vector<std::pair<const char*, QStringList>> expected{
        {"Overview", {"stakingButton"}},
        {"Transactions", {"emptyStateAction"}},
        {"Receive", {"abyssReceiveCopy"}},
        {"Send", {"sendButton"}},
        {"CoinJoin", {"sendButton"}},
        {"Multisig", {"multisigCreate"}},
    };
    QCOMPARE(view.count(), int(expected.size()));
    for (int i = 0; i < view.count(); ++i) {
        view.setCurrentWidget(view.widget(i)); Settle();
        const QStringList found = blue(view.widget(i));
        QVERIFY2(found == expected[size_t(i)].second, qPrintable(QString("%1: %2").arg(expected[size_t(i)].first, found.join(','))));
    }
    view.setCurrentWidget(view.widget(0)); Settle();
    QVERIFY(toggle->isVisible() && toggle->isEnabled());
    // Also those a page shows only now and then. Each still reads as disabled:
    // the secondary rules outrank the general one for that state, so each has its
    // own, and its text goes dim. The text is the brightest thing inside the border.
    const auto brightest = [](const QImage& image) {
        int lightness = 0;
        for (int y = image.height() / 4; y < image.height() * 3 / 4; ++y) {
            for (int x = 8; x < image.width() - 8; ++x) lightness = std::max(lightness, image.pixelColor(x, y).lightness());
        }
        return lightness;
    };
    constexpr int DIM_TEXT{140};
    std::vector<QPushButton*> secondary{toggle, view.widget(1)->findChild<QPushButton*>("exportButton")};
    for (const char* name : {"pushButtonCoinControl", "buttonChooseFee", "buttonMinimizeFee", "useAvailableBalanceButton"}) {
        secondary.push_back(view.widget(3)->findChild<QPushButton*>(name));
    }
    for (QPushButton* button : secondary) {
        QVERIFY(button);
        button->setEnabled(true);
        const QImage enabled = button->grab().toImage();
        QVERIFY2(enabled.pixelColor(6, enabled.height() / 2) == QColor("#0b1727"), qPrintable(button->objectName()));
        if (GUIUtil::fontsLoaded()) QVERIFY2(brightest(enabled) > DIM_TEXT, qPrintable(button->objectName()));
        button->setEnabled(false);
        const QImage disabled = button->grab().toImage();
        QVERIFY2(disabled != enabled, qPrintable(button->objectName()));
        QVERIFY2(brightest(disabled) < DIM_TEXT, qPrintable(button->objectName()));
        button->setEnabled(true);
    }
    // The Receive page's dark New address answers a press like the other
    // secondary buttons do, with the primary blue border; its request form,
    // once opened, adds no second blue button.
    {
        view.setCurrentWidget(view.widget(2)); Settle();
        auto* fresh = view.widget(2)->findChild<QPushButton*>("abyssReceiveNewAddress");
        QVERIFY(fresh && fresh->isVisible());
        const QImage up = fresh->grab().toImage();
        fresh->setDown(true);
        const QImage down = fresh->grab().toImage();
        fresh->setDown(false);
        QVERIFY(down != up);
        QCOMPARE(down.pixelColor(6, down.height() / 2), QColor("#10263e"));
        view.widget(2)->findChild<QPushButton*>("abyssReceiveRequestToggle")->click(); Settle();
        auto* create = view.widget(2)->findChild<QPushButton*>("receiveButton");
        QVERIFY(create && create->isVisible() && create->isEnabled());
        QCOMPARE(blue(view.widget(2)), QStringList{"abyssReceiveCopy"});
    }
    // The dialogs a page opens keep their own blue OK: the page's rule stops at the page.
    auto* multisig = view.widget(5);
    QDialog dialog(multisig);
    QPushButton inside_dialog("OK", &dialog);
    dialog.show(); Settle();
    QCOMPARE(inside_dialog.grab().toImage().pixelColor(6, inside_dialog.height() / 2), primary);
    dialog.hide();
    // The two pages without buttons of their own stay without them.
    MasternodeList masternodes;
    GovernanceList governance;
    for (QWidget* page : {static_cast<QWidget*>(&masternodes), static_cast<QWidget*>(&governance)}) {
        page->resize(1042, 850);
        page->show(); Settle();
        QVERIFY(blue(page).isEmpty());
        page->hide();
    }
}

void AbyssTests::overviewColumns()
{
    // The balances and CoinJoin cards read as one table: the labels start at the
    // card's edge, in a column of the same width, the amounts under each other.
    if (!GUIUtil::fontsLoaded()) QSKIP("Text positions require a font-capable platform; run minimal:enable_fonts");
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setClientModel(fixture.client.get());
    page.setWalletModel(fixture.model.get());
    auto* coinjoin = page.findChild<QFrame*>("frameCoinJoin");
    QVERIFY(coinjoin);
    // Abyss keeps the card out of the way until mixing; the row's link opens it for good.
    page.findChild<QPushButton*>("coinJoinRowShow")->click();
    coinjoin->show();
    const QStringList labels{"labelBalanceText", "labelPendingText", "labelImmatureText", "labelTotalText", "labelCoinJoinEnabledText",
                             "labelCompletitionText", "labelAnonymizedText", "labelAmountAndRoundsText", "labelSubmittedDenomText"};
    const QStringList amounts{"labelBalance", "labelUnconfirmed", "labelImmature", "labelTotal", "labelCoinJoinEnabled",
                              "labelAnonymized", "labelAmountRounds", "labelSubmittedDenom"};
    // A row with nothing in it hides itself, also on a theme change; the checks want them all.
    const auto show_all = [&] {
        for (const QString& name : labels + amounts + QStringList{"labelSpendable"}) {
            auto* label = page.findChild<QLabel*>(name);
            QVERIFY2(label, qPrintable(name));
            label->show();
        }
    };
    show_all();
    // Where the text starts: a label's margin can indent it inside a rectangle that
    // lines up. Two pixels are left for the shapes of the first letters.
    const auto text_left = [&](const QString& name) {
        auto* label = page.findChild<QLabel*>(name);
        const QImage image = label->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        for (int x = 0; x < image.width(); ++x) {
            for (int y = 0; y < image.height(); ++y) {
                if (image.pixel(x, y) != image.pixel(0, 0)) return OnPage(page, label).left() + int(x / image.devicePixelRatio());
            }
        }
        return -1;
    };
    const auto problems = [&] {
        QStringList found;
        const int left = OnPage(page, page.findChild<QLabel*>(labels[0])).left();
        const int width = page.findChild<QLabel*>(labels[0])->width();
        const int amount_left = OnPage(page, page.findChild<QLabel*>(amounts[0])).left();
        for (const QString& name : labels) {
            auto* label = page.findChild<QLabel*>(name);
            if (OnPage(page, label).left() != left) found << name + " left";
            if (label->width() != width) found << QString("%1 width %2 not %3").arg(name).arg(label->width()).arg(width);
            if (!(label->alignment() & Qt::AlignLeft)) found << name + " alignment";
        }
        for (const QString& name : amounts) {
            if (OnPage(page, page.findChild<QLabel*>(name)).left() != amount_left) {
                found << QString("%1 at %2 not %3").arg(name).arg(OnPage(page, page.findChild<QLabel*>(name)).left()).arg(amount_left);
            }
        }
        // The card titles start where the labels do.
        for (const char* name : {"label_5", "labelCoinJoinHeader"}) {
            if (qAbs(text_left(name) - text_left(labels[0])) > 2) found << QString("%1 text at %2 not %3").arg(name).arg(text_left(name)).arg(text_left(labels[0]));
        }
        if (qAbs(text_left("labelSpendable") - text_left(amounts[0])) > 2) found << QString("labelSpendable text at %1 not %2").arg(text_left("labelSpendable")).arg(text_left(amounts[0]));
        return found.join("; ");
    };
    page.resize(1280, 850);
    page.show(); Settle();
    QCOMPARE(problems(), QString());
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) QVERIFY(QDir().mkpath(output) && PageImage(page).save(QDir(output).filePath("Abyss-overview-columns.png")));
    // A larger font widens the column on both cards together.
    const int column = page.findChild<QLabel*>(labels[0])->width();
    const auto font_scale = [](int scale) {
        GUIUtil::setFontScale(scale);
        GUIUtil::updateFonts(); Settle();
    };
    // Back to the normal size. A layout can ask for the labels' size hints before
    // the page gets to lower their minimums, and the column has to come back anyway.
    const auto font_back = [&] {
        GUIUtil::setFontScale(0);
        GUIUtil::updateFonts();
        for (const QString& name : labels) page.findChild<QLabel*>(name)->sizeHint();
        Settle();
    };
    // The classic themes keep their own columns: the page back from Abyss is a
    // fresh page's equal, at whatever font size the user keeps. Read before the
    // fresh page is built, whose constructor updates every font.
    const auto classic_columns = [&] {
        std::map<QString, std::pair<int, Qt::Alignment>> back;
        for (const QString& name : labels) {
            auto* label = page.findChild<QLabel*>(name);
            back.emplace(name, std::make_pair(label->minimumWidth(), label->alignment()));
        }
        const int back_spacing = page.findChild<QFormLayout*>("coinJoinFormLayout")->horizontalSpacing();
        OverviewPage fresh;
        fresh.resize(1280, 850);
        fresh.show(); Settle();
        for (const QString& name : labels) {
            auto* original = fresh.findChild<QLabel*>(name);
            const auto& [minimum, alignment] = back.at(name);
            QVERIFY2(minimum == original->minimumWidth(), qPrintable(QString("%1 %2 %3").arg(name).arg(minimum).arg(original->minimumWidth())));
            QVERIFY2(alignment == original->alignment(), qPrintable(name));
        }
        QCOMPARE(back_spacing, fresh.findChild<QFormLayout*>("coinJoinFormLayout")->horizontalSpacing());
    };
    font_scale(50);
    QCOMPARE(problems(), QString());
    QVERIFY(page.findChild<QLabel*>(labels[0])->width() > column);
    font_back();
    QCOMPARE(problems(), QString());
    QCOMPARE(page.findChild<QLabel*>(labels[0])->width(), column);
    Theme("Light"); Settle();
    classic_columns();
    // Again with the larger font kept across the change of theme.
    Theme("Abyss"); Settle();
    show_all(); Settle();
    QCOMPARE(problems(), QString());
    font_scale(50);
    QCOMPARE(problems(), QString());
    Theme("Light"); Settle();
    classic_columns();
    // And back once more: the column still shrinks to what it was.
    Theme("Abyss"); Settle();
    show_all(); Settle();
    font_back();
    QCOMPARE(problems(), QString());
    QCOMPARE(page.findChild<QLabel*>(labels[0])->width(), column);
}

void AbyssTests::transactionFilters()
{
    // Abyss says what its filters filter, instead of two lists reading "All",
    // and has no empty scroll bar strip beside a list with nothing to scroll.
    RestoreAppearance restore;
    Theme("Light");
    WalletFixture fixture(m_node);
    TransactionView view(nullptr);
    view.setModel(fixture.model.get());
    view.resize(1042, 600);
    view.show(); Settle();
    QComboBox* date = nullptr;
    QComboBox* type = nullptr;
    for (QComboBox* combo : view.findChildren<QComboBox*>()) {
        if (combo->findText(TransactionView::tr("Today")) >= 0) date = combo;
        if (combo->findText(TransactionView::tr("Most Common")) >= 0) type = combo;
    }
    auto* amount = view.findChild<QLineEdit*>("amountWidget");
    auto* table = view.findChild<QTableView*>("transactionView");
    QVERIFY(date && type && amount && table);
    const QVariant date_all = date->itemData(0);
    const QVariant type_all = type->itemData(0);
    const int classic_gap = view.width() - (amount->x() + amount->width());
    const auto classic_state = [&] {
        QCOMPARE(date->itemText(0), TransactionView::tr("All"));
        QCOMPARE(type->itemText(0), TransactionView::tr("All"));
        QVERIFY(date->toolTip().isEmpty() && type->toolTip().isEmpty() && amount->toolTip().isEmpty());
#ifndef Q_OS_MAC
        QCOMPARE(table->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
        QVERIFY(classic_gap > 0);
#endif
        QCOMPARE(view.width() - (amount->x() + amount->width()), classic_gap);
    };
    classic_state();

    Theme("Abyss"); Settle();
    QCOMPARE(date->itemText(0), TransactionView::tr("Any date"));
    QCOMPARE(type->itemText(0), TransactionView::tr("Any type"));
    QVERIFY(!date->toolTip().isEmpty() && !type->toolTip().isEmpty() && !amount->toolTip().isEmpty());
    // Only the words: the filters mean what they meant.
    QCOMPARE(date->itemData(0), date_all);
    QCOMPARE(type->itemData(0), type_all);
    QCOMPARE(date->currentIndex(), 0);
#ifndef Q_OS_MAC
    QCOMPARE(table->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QVERIFY(!table->verticalScrollBar()->isVisible());
    // The filter row reaches the list's right edge.
    QCOMPARE(amount->x() + amount->width(), table->x() + table->width());
#endif
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) QVERIFY(QDir().mkpath(output) && PageImage(view).save(QDir(output).filePath("Abyss-transaction-filters.png")));

    Theme("Light"); Settle();
    classic_state();
}

void AbyssTests::fontsAfterDeletedWidgets()
{
    // GUIUtil files its font requests in a map sorted by widget, and a deleted
    // widget's entry turns null where it stands. A request filed while one was
    // there could land where updateFonts() never finds it, and the new widget
    // kept the default size.
    if (!GUIUtil::fontsLoaded()) QSKIP("Font sizes require minimal:enable_fonts or a native platform");
    RestoreAppearance restore;
    Theme("Abyss");
    GUIUtil::updateFonts();
    QWidget parent;
    const auto requested = [&parent] {
        auto* label = new QLabel("x", &parent);
        GUIUtil::setFont({label}, GUIUtil::FontWeight::Normal, 37);
        return label;
    };
    std::vector<QLabel*> before;
    for (int i = 0; i < 256; ++i) before.push_back(requested());
    GUIUtil::updateFonts();
    const QFont wanted = before.front()->font();
    QVERIFY(wanted.pointSize() > 30);
    for (size_t i = 0; i < before.size(); i += 2) delete before[i];
    std::vector<QLabel*> after;
    for (int i = 0; i < 256; ++i) after.push_back(requested());
    GUIUtil::updateFonts();
    const auto missed = std::count_if(after.begin(), after.end(), [&wanted](QLabel* label) { return label->font() != wanted; });
    QCOMPARE(int(missed), 0);
}

void AbyssTests::skyTiers()
{
    using namespace SkyTiers;
    QCOMPARE(COUNT, 11);
    QCOMPARE(threshold(0), CAmount(0));
    QCOMPARE(threshold(1), 100000 * COIN);
    QCOMPARE(threshold(2), 200000 * COIN);
    QCOMPARE(threshold(3), 500000 * COIN);
    QCOMPARE(threshold(4), 1000000 * COIN);
    QCOMPARE(threshold(10), 25000000 * COIN);
    QCOMPARE(threshold(11), threshold(10));
    QCOMPARE(threshold(-1), threshold(0));
    for (int tier = 1; tier < COUNT; ++tier) QVERIFY(threshold(tier) > threshold(tier - 1));
    QCOMPARE(tierForBalance(0, 0), 0);
    QCOMPARE(tierForBalance(100000 * COIN - 1, 0), 0);
    QCOMPARE(tierForBalance(100000 * COIN, 0), 1);
    QCOMPARE(tierForBalance(25000000 * COIN, 0), 10);
    QCOMPARE(tierForBalance(MAX_MONEY, 0), 10);
    // Up at once; down only below 97% of the level's threshold.
    QCOMPARE(tierForBalance(1000000 * COIN, 3), 4);
    QCOMPARE(tierForBalance(970000 * COIN, 4), 4);
    QCOMPARE(tierForBalance(970000 * COIN - 1, 4), 3);
    QCOMPARE(tierForBalance(970000 * COIN, 0), 3); // The same balance from below is level 3.
    QCOMPARE(tierForBalance(0, 10), 0);
    QCOMPARE(tierForBalance(-1, 4), 0);
    QCOMPARE(tierForBalance(500000 * COIN, 99), 3); // An out-of-range level is clamped.
    QSet<QString> names;
    for (int tier = 0; tier < COUNT; ++tier) {
        QVERIFY(!name(tier).isEmpty());
        names.insert(name(tier));
    }
    QCOMPARE(names.size(), COUNT);
    QCOMPARE(name(10), QString("DeFCoN Universe"));
    QCOMPARE(name(11), name(10));
}

void AbyssTests::skyScenes()
{
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    RestoreAppearance restore;
    Theme("Abyss"); // The stylesheet's backdrop, behind which every lit pixel is the sky's.
    const QColor backdrop("#04070d");
    for (const qreal dpr : {1.0, 2.0}) {
        DpiWidget<ControlledSky> sky;
        sky.setObjectName("walletContainer");
        sky.ratio = dpr;
        sky.m_epoch_utc_ms = 1792173600000LL;
        sky.resize(1280, 850);
        sky.setSkyVisible(true);
        sky.setAnimated(false); // A fixed date and no motion: every scene is reproducible.
        sky.show();
        QApplication::setActiveWindow(&sky);
        QApplication::processEvents();
        const auto lit = [&](const QImage& image, const QRectF& logical) {
            const QRect pixels(QPoint(qRound(logical.left() * dpr), qRound(logical.top() * dpr)),
                               QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr)));
            int count = 0;
            for (int y = std::max(0, pixels.top()); y <= std::min(image.height() - 1, pixels.bottom()); ++y) {
                for (int x = std::max(0, pixels.left()); x <= std::min(image.width() - 1, pixels.right()); ++x) {
                    if (image.pixelColor(x, y) != backdrop) ++count;
                }
            }
            return count;
        };
        QVector<QImage> scenes;
        for (int tier = 0; tier < SkyTiers::COUNT; ++tier) {
            sky.setSkyTier(tier);
            // The first level is the one the sky was shown with; every other one rebuilds the cache itself.
            if (tier == 0) sky.invalidateImages();
            QCOMPARE(sky.skyTier(), tier);
            const int redraws = sky.m_steady_redraws;
            const QImage image = sky.grab().toImage();
            QCOMPARE(image.size(), QSize(1280 * dpr, 850 * dpr));
            QCOMPARE(sky.m_steady_redraws, redraws + 1);
            sky.grab();
            QCOMPARE(sky.m_steady_redraws, redraws + 1); // The cache held for a second paint.
            if (!output.isEmpty()) QVERIFY(image.save(QDir(output).filePath(QString("Abyss-sky-tier-%1-dpr%2.png").arg(tier).arg(dpr))));
            // The Moon is drawn on the two levels the user chose, and from Saturn up the scene takes its place.
            const bool moon = tier == 2 || tier == 3;
            QVERIFY2(sky.m_moon.isNull() != moon, qPrintable(QString("tier %1 moon drawn %2").arg(tier).arg(!sky.m_moon.isNull())));
            const QRectF moon_disk(sky.m_moon_position - QPointF(25, 25), QSizeF(50, 50));
            const QRectF hero(sky.heroPosition() - QPointF(50, 50), QSizeF(100, 100));
            if (moon) QVERIFY2(lit(image, moon_disk) > 1500 * dpr * dpr, qPrintable(QString("tier %1 moon").arg(tier)));
            if (tier >= 4) QVERIFY2(lit(image, hero) > 400 * dpr * dpr, qPrintable(QString("tier %1 scene").arg(tier)));
            for (const QImage& earlier : scenes) QVERIFY2(image != earlier, qPrintable(QString("tier %1 repeats a scene").arg(tier)));
            scenes.append(image);
        }
        QCOMPARE(scenes.size(), SkyTiers::COUNT);
        // The level is kept but nothing is allocated while the sky is off (classic themes).
        sky.setSkyTier(5);
        sky.setSkyVisible(false);
        QVERIFY(sky.m_stars.isEmpty() && sky.m_steady.isNull() && sky.m_moon.isNull() && sky.m_nebula.isNull());
        sky.setSkyTier(7);
        QCOMPARE(sky.skyTier(), 7);
        QVERIFY(sky.m_stars.isEmpty() && sky.m_steady.isNull());
        sky.setSkyVisible(true);
        sky.grab();
        QVERIFY(!sky.m_nebula.isNull());
        QCOMPARE(sky.grab().toImage(), scenes.at(7));
    }
}

void AbyssTests::skyCatalogue()
{
    constexpr qreal degree = 3.141592653589793 / 180;
    // Polaris stands at the observer's latitude at any hour, north up; the
    // far south never rises.
    for (const qreal lst : {0.0, 6.0, 12.0, 18.0}) {
        const StarfieldWidget::Projection projection(QSize(1280, 850), lst);
        QPointF at;
        qreal altitude = 0;
        QVERIFY(projection.place(2.530, 89.264, at, altitude));
        QVERIFY(std::abs(altitude / degree - 47) < 1.0);
        QVERIFY(std::abs(at.x() - 640) < 12);
        QVERIFY(at.y() < 425);
        QVERIFY(!projection.place(12.443, -63.099, at, altitude)); // Acrux
    }
    // Sidereal time: the J2000 value, the rate, and the longitude.
    const qint64 j2000 = 946728000000LL;
    QVERIFY(std::abs(StarfieldWidget::siderealHours(j2000, 0) - 18.697374558) < 1e-6);
    QVERIFY(std::abs(StarfieldWidget::siderealHours(j2000 + 3600000, 0) - 18.697374558 - 1.00273790935) < 1e-6);
    QVERIFY(std::abs(StarfieldWidget::siderealHours(j2000, 15) - 19.697374558) < 1e-6);
    QVERIFY(std::abs(StarfieldWidget::siderealHours(j2000 + 24 * 3600000LL, 0) - 18.763097) < 1e-4);
    const qreal longitude = StarfieldWidget::longitudeGuess(j2000);
    QVERIFY(longitude >= -180 && longitude <= 180);
    // Every line joins two different catalogue stars, and the table is sane.
    QVERIFY(SkyCatalogue::STAR_COUNT >= 300);
    QVERIFY(SkyCatalogue::LINE_COUNT >= 100);
    for (const auto& line : SkyCatalogue::LINES) {
        QVERIFY(line.from < SkyCatalogue::STAR_COUNT && line.to < SkyCatalogue::STAR_COUNT);
        QVERIFY(line.from != line.to);
    }
    for (const auto& star : SkyCatalogue::STARS) {
        QVERIFY(star.ra_hours >= 0 && star.ra_hours < 24);
        QVERIFY(std::abs(star.dec_degrees) <= 90);
        QVERIFY(star.magnitude > -2 && star.magnitude < 6);
    }
    // The real sky is projected again after ten minutes, never while frozen,
    // and the plain field of level 0 has nothing to refresh.
    ControlledSky sky;
    sky.m_epoch_utc_ms = 1792173600000LL;
    sky.resize(1280, 850);
    sky.setSkyVisible(true);
    sky.setSkyTier(1);
    sky.show();
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();
    sky.grab();
    const int redraws = sky.m_steady_redraws;
    sky.elapsed = 5 * 60000;
    sky.onTick();
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, redraws);
    sky.elapsed = 10 * 60000;
    sky.onTick();
    QVERIFY(sky.m_steady.isNull());
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, redraws + 1);
    sky.setAnimated(false);
    sky.elapsed += 3600000;
    sky.onTick();
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, redraws + 1);
    sky.setAnimated(true);
    sky.setSkyTier(0);
    sky.grab();
    const int plain = sky.m_steady_redraws;
    sky.elapsed += 3600000;
    sky.onTick();
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, plain);
    // A visible but inactive window gets no frames, but its real sky is still
    // redrawn once when the slot turns; still, hidden or plain, it waits.
    sky.setSkyTier(1);
    sky.grab();
    const int active = sky.m_steady_redraws;
    QApplication::setActiveWindow(nullptr);
    QApplication::processEvents();
    sky.updateTimerState();
    QVERIFY(!sky.m_timer->isActive());
    QVERIFY(sky.m_refresh->isActive());
    QVERIFY(sky.m_refresh->interval() > 0 && sky.m_refresh->interval() <= 601000);
    sky.elapsed += 600000;
    sky.onRefresh(); // What the single shot runs.
    QVERIFY(sky.m_steady.isNull());
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, active + 1);
    QVERIFY(sky.m_refresh->isActive());
    sky.onRefresh(); // The same slot: nothing to redraw.
    sky.grab();
    QCOMPARE(sky.m_steady_redraws, active + 1);
    // The slot turns while the picture is drawn, between the rebuild's first
    // look at the clock and the timer's: the paint keeps the picture it just
    // drew, and the redraw for the new slot waits for the event loop.
    sky.m_steady = QPixmap();
    sky.slot_turns_after = sky.utc_reads + 1;
    sky.grab();
    QVERIFY(sky.utc_reads > sky.slot_turns_after); // The clock really turned during the rebuild.
    QVERIFY(!sky.m_steady.isNull()); // The paint kept, and showed, the picture it drew.
    QCOMPARE(sky.m_steady_redraws, active + 2);
    QVERIFY(sky.m_refresh->isActive());
    QCOMPARE(sky.m_refresh->interval(), 0);
    sky.onRefresh();
    QVERIFY(sky.m_steady.isNull());
    sky.grab();
    QCOMPARE(sky.m_sky_bucket, sky.skyBucket());
    QCOMPARE(sky.m_steady_redraws, active + 3);
    QVERIFY(sky.m_refresh->interval() > 0);
    sky.slot_turns_after = -1;
    sky.elapsed += 600000; // The extra slot becomes ordinary elapsed time: the clock does not go back.
    // A millisecond before the turn the timer is armed, and a second look at
    // the clock would already be past it: one reading decides both, so the
    // refresh comes a second past the turn, not a whole slot later.
    sky.elapsed += 600000 - (sky.m_epoch_utc_ms + sky.elapsed + sky.wall_step) % 600000 - 1;
    QCOMPARE(sky.m_sky_bucket, sky.skyBucket());
    sky.turn_ms = 2;
    sky.slot_turns_after = sky.utc_reads + 1;
    sky.updateRefreshTimer();
    QVERIFY(sky.m_refresh->isActive());
    QVERIFY2(sky.m_refresh->interval() <= 1001, qPrintable(QString::number(sky.m_refresh->interval())));
    sky.slot_turns_after = -1;
    sky.turn_ms = 600000;
    sky.elapsed += 1001; // Past the turn, and past the 2 ms the clock already showed.
    sky.onRefresh();
    QVERIFY(sky.m_steady.isNull());
    sky.grab();
    QCOMPARE(sky.m_sky_bucket, sky.skyBucket());
    QCOMPARE(sky.m_steady_redraws, active + 4);
    sky.setAnimated(false);
    QVERIFY(!sky.m_refresh->isActive());
    sky.setAnimated(true);
    QVERIFY(sky.m_refresh->isActive());
    sky.setSkyTier(0);
    QVERIFY(!sky.m_refresh->isActive());
    sky.setSkyTier(1);
    QVERIFY(sky.m_refresh->isActive());
    sky.hide();
    QVERIFY(!sky.m_refresh->isActive());
}

void AbyssTests::skyBadge()
{
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setWalletModel(fixture.model.get());
    // Wide enough for the badge with or without fonts: without them the bar's words are wider.
    page.resize(1600, 850);
    page.show(); Settle();
    auto* card = page.findChild<QFrame*>("networkCard");
    QVERIFY(card);
    auto* badge = card->findChild<QWidget*>("networkSky");
    QVERIFY(badge);
    auto* text = badge->findChild<QLabel*>("networkText");
    QVERIFY(text);
    QSignalSpy changed(&page, &OverviewPage::skyTierChanged);
    QVERIFY(badge->isHidden());
    QCOMPARE(page.skyTier(), 0);
    // What the bar holds, for a failure message.
    const auto describe = [&] {
        QStringList parts{QString("card %1 layout hint %2").arg(card->width()).arg(card->layout()->sizeHint().width()),
                          QString("badge hint %1 at %2+%3 hidden %4").arg(badge->sizeHint().width()).arg(badge->x()).arg(badge->width()).arg(badge->isHidden())};
        for (QWidget* other : card->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (other != badge && !other->isHidden()) parts << QString("%1 %2+%3").arg(other->objectName()).arg(other->x()).arg(other->width());
        }
        return parts.join("; ");
    };

    interfaces::WalletBalances balances;
    balances.balance = 1000000 * COIN;
    page.setBalance(balances); Settle();
    QCOMPARE(page.skyTier(), 4);
    QVERIFY2(badge->isVisible(), qPrintable(describe()));
    QCOMPARE(text->text(), SkyTiers::name(4));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(changed.last().at(0).toInt(), 4);
    QVERIFY(badge->toolTip().contains(SkyTiers::name(4)));
    QVERIFY(badge->toolTip().contains(SkyTiers::name(5)));
    // Abyss's amounts (U3-C): "2 000 000 DFCN", no decimals.
    QVERIFY2(badge->toolTip().contains(Thin("2 000 000") + QLatin1Char(' ') + BitcoinUnits::name(fixture.options->getDisplayUnit())), qPrintable(badge->toolTip()));
    QVERIFY2(!badge->toolTip().contains(".00000000"), qPrintable(badge->toolTip()));
    // Pending and immature count towards the level; watch-only amounts do not.
    balances.balance = 0;
    balances.unconfirmed_balance = 600000 * COIN;
    balances.immature_balance = 400000 * COIN;
    page.setBalance(balances);
    QCOMPARE(page.skyTier(), 4);
    balances = {};
    balances.watch_only_balance = 25000000 * COIN;
    page.setBalance(balances);
    QCOMPARE(page.skyTier(), 0);
    QVERIFY(badge->isHidden());
    QCOMPARE(changed.last().at(0).toInt(), 0);
    // Nor does a wallet without private keys: a descriptor wallet reports
    // what it only watches as its own balance, so the page leaves the sky at 0.
    fixture.wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    QVERIFY(fixture.model->wallet().privateKeysDisabled());
    const int changes_before_keyless = changed.count();
    balances = {};
    balances.balance = 25000000 * COIN;
    page.setBalance(balances);
    QCOMPARE(page.skyTier(), 0);
    QVERIFY(badge->isHidden());
    QCOMPARE(changed.count(), changes_before_keyless);
    fixture.wallet->UnsetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    QVERIFY(!fixture.model->wallet().privateKeysDisabled());
    // Hysteresis through the page: a dip of three percent keeps the level.
    balances = {};
    balances.balance = 1000000 * COIN;
    page.setBalance(balances);
    QCOMPARE(page.skyTier(), 4);
    balances.balance = 975000 * COIN;
    page.setBalance(balances);
    QCOMPARE(page.skyTier(), 4);
    balances.balance = 960000 * COIN;
    page.setBalance(balances);
    QCOMPARE(page.skyTier(), 3);
    QCOMPARE(text->text(), SkyTiers::name(3));
    // Discreet mode hides the level and the badge, and brings them back.
    const int before = changed.count();
    page.setPrivacy(true); Settle();
    QCOMPARE(page.skyTier(), 0);
    QVERIFY(badge->isHidden());
    QCOMPARE(changed.count(), before + 1);
    QCOMPARE(changed.last().at(0).toInt(), 0);
    page.setPrivacy(false); Settle();
    QCOMPARE(page.skyTier(), 3);
    QVERIFY(badge->isVisible());
    QCOMPARE(changed.last().at(0).toInt(), 3);
    // The top level says so instead of naming a next one.
    balances.balance = 25000000 * COIN;
    // The longer name can turn the words short; the badge takes its place once the bar is laid out again.
    page.setBalance(balances); Settle();
    QCOMPARE(page.skyTier(), 10);
    QCOMPARE(text->text(), SkyTiers::name(10));
    QVERIFY(badge->toolTip().contains(OverviewPage::tr("The highest level.")));
    // The badge sits in the bar's free middle, over no other entry, at its full
    // size; where it does not fit it steps aside, and the long words give way
    // to it first. It never makes the page wider than it already shrinks to.
    const auto clear_of_entries = [&] {
        if (badge->isHidden()) return true;
        if (!card->rect().contains(badge->geometry()) || badge->width() < badge->sizeHint().width()) return false;
        for (QWidget* other : card->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (other != badge && !other->isHidden() && other->geometry().intersects(badge->geometry())) return false;
        }
        return true;
    };
    auto* masternode_words = card->findChild<QWidget*>("networkMasternodes")->findChild<QLabel*>("networkText");
    QVERIFY(masternode_words);
    QVERIFY2(badge->isVisible(), qPrintable(describe()));
    QVERIFY2(clear_of_entries(), qPrintable(describe()));
    const int bar_minimum_with_badge = card->minimumSizeHint().width();
    bool stepped_aside = false;
    bool long_with_badge = false;
    for (int width = 1800; width >= std::max(page.minimumWidth(), 600); width -= 10) {
        page.resize(width, 850); Settle();
        QVERIFY2(clear_of_entries(), qPrintable(QString("badge over an entry at %1: %2").arg(width).arg(describe())));
        const bool long_words = masternode_words->text() == QString("masternodes");
        QVERIFY2(!long_words || badge->isVisible(), qPrintable(QString("long words without the badge at %1: %2").arg(width).arg(describe())));
        stepped_aside |= badge->isHidden();
        long_with_badge |= long_words && badge->isVisible();
    }
    QVERIFY(stepped_aside);
    QVERIFY(long_with_badge);
    page.resize(1600, 850); Settle();
    QVERIFY2(badge->isVisible(), qPrintable(describe()));
    balances.balance = 0;
    page.setBalance(balances); Settle();
    QCOMPARE(page.skyTier(), 0);
    QVERIFY(badge->isHidden());
    // The bar asks for no more room with the badge than without it.
    QCOMPARE(card->minimumSizeHint().width(), bar_minimum_with_badge);

    // Pictures for the evidence: the sky behind the Overview page at every level, as the preview showed it.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        ControlledSky sky;
        sky.setObjectName("walletContainer");
        sky.m_epoch_utc_ms = 1792173600000LL;
        sky.resize(1280, 850);
        sky.setSkyVisible(true);
        sky.setAnimated(false);
        auto* layout = new QVBoxLayout(&sky);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* composed = new OverviewPage(&sky);
        composed->setWalletModel(fixture.model.get());
        layout->addWidget(composed);
        sky.show(); Settle();
        for (int tier = 0; tier < SkyTiers::COUNT; ++tier) {
            interfaces::WalletBalances level;
            level.balance = SkyTiers::threshold(tier) + (tier > 0 ? 50000 * COIN : 0);
            composed->setBalance(level); Settle();
            QCOMPARE(composed->skyTier(), tier);
            sky.setSkyTier(composed->skyTier());
            QVERIFY(sky.grab().save(QDir(output).filePath(QString("Abyss-sky-overview-tier-%1.png").arg(tier))));
        }
    }

    // A classic theme keeps the level as data and never shows the pill.
    Theme("Light");
    OverviewPage classic;
    classic.setWalletModel(fixture.model.get());
    classic.resize(1280, 850);
    classic.show(); Settle();
    QSignalSpy classic_changed(&classic, &OverviewPage::skyTierChanged);
    auto* classic_badge = classic.findChild<QWidget*>("networkSky");
    QVERIFY(classic_badge);
    balances.balance = 1000000 * COIN;
    classic.setBalance(balances); Settle();
    QCOMPARE(classic.skyTier(), 4);
    QVERIFY(classic_badge->isHidden());
    QCOMPARE(classic_changed.count(), 1);
}

void AbyssTests::skyWindowTier()
{
    if (!GUIUtil::fontsLoaded()) QSKIP("Window geometry requires minimal:enable_fonts or a native platform");
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name); // Keep this test's isolated QSettings scope.
    QVERIFY(style);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850);
    window.show(); Settle();
    auto* sky = qobject_cast<StarfieldWidget*>(window.centralWidget());
    QVERIFY(sky);
    QVERIFY(sky->m_skyVisible);
    QCOMPARE(sky->skyTier(), 0);
    auto* privacy_action = window.findChild<QAction*>("m_mask_values_action");
    QVERIFY(privacy_action);
    const bool initial_privacy = privacy_action->isChecked();
    const auto restore_privacy = interfaces::MakeHandler([privacy_action, initial_privacy] { privacy_action->setChecked(initial_privacy); });
    privacy_action->setChecked(false);
    window.addWallet(fixture.model.get()); Settle();
    QCOMPARE(sky->skyTier(), 0); // An empty wallet: the plain field.
    auto* page = window.findChild<OverviewPage*>();
    QVERIFY(page);
    interfaces::WalletBalances balances;
    for (int tier = 0; tier < SkyTiers::COUNT; ++tier) {
        balances.balance = SkyTiers::threshold(tier);
        page->setBalance(balances); Settle();
        QCOMPARE(sky->skyTier(), tier);
        QCOMPARE(page->skyTier(), tier);
    }
    // The badge names the level. Whether it fits the bar is skyBadge's case: on
    // the minimal platform this window is held to the 240x320 screen.
    auto* badge = window.findChild<QWidget*>("networkSky");
    QVERIFY(badge);
    QCOMPARE(badge->findChild<QLabel*>("networkText")->text(), SkyTiers::name(10));
    // Discreet mode through the window's own action takes the sky to the plain field and back.
    privacy_action->setChecked(true); Settle();
    QCOMPARE(sky->skyTier(), 0);
    QVERIFY(badge->isHidden());
    privacy_action->setChecked(false); Settle();
    QCOMPARE(sky->skyTier(), 10);
    // A classic theme keeps the level but draws no sky; Abyss returns with it.
    Theme("Light"); Settle();
    QVERIFY(!sky->m_skyVisible);
    QVERIFY(sky->m_stars.isEmpty());
    QCOMPARE(sky->skyTier(), 10);
    Theme("Abyss"); Settle();
    QVERIFY(sky->m_skyVisible);
    QCOMPARE(sky->skyTier(), 10);
    // Without the wallet the plain field returns.
    window.removeWallet(fixture.model.get()); Settle();
    QCOMPARE(sky->skyTier(), 0);
}

void AbyssTests::skyCpu()
{
    // Opt-in measurement, not a check: process CPU per level while the sky
    // runs on its own clock, logged for the evidence folder.
    const int seconds = qEnvironmentVariableIntValue("DEFCON_QT_SKY_CPU_SECONDS");
    if (seconds <= 0) QSKIP("Set DEFCON_QT_SKY_CPU_SECONDS to measure idle CPU per level");
#ifdef WIN32
    QSKIP("CPU time is sampled through getrusage");
#else
    const auto cpu_ms = [] {
        rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
    };
    // A real idle wait: the event loop sleeps until the sky's own timer or the
    // end of the interval wakes it, as in the running wallet.
    const auto spin = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    for (const qreal dpr : {1.0, 2.0}) {
        DpiWidget<StarfieldWidget> sky;
        sky.ratio = dpr;
        sky.resize(dpr > 1 ? QSize(2560, 1440) : QSize(1280, 850));
        sky.setSkyVisible(true);
        sky.show();
        QApplication::setActiveWindow(&sky);
        QApplication::processEvents();
        for (int tier = 0; tier < SkyTiers::COUNT; ++tier) {
            sky.setSkyTier(tier);
            spin(1000); // Warm the caches of this level.
            QVERIFY(sky.wantsAnimation());
            const double cpu_before = cpu_ms();
            QElapsedTimer wall;
            wall.start();
            spin(seconds * 1000);
            const double cpu = cpu_ms() - cpu_before;
            qInfo() << "SKY_CPU" << "tier" << tier << "dpr" << dpr << "size" << sky.size() << "cpu_ms" << cpu
                    << "wall_ms" << wall.elapsed() << "percent" << 100.0 * cpu / wall.elapsed()
                    << "redraws" << sky.m_steady_redraws << "interval_ms" << sky.m_timer->interval();
        }
    }
#endif
}

void AbyssTests::coinJoinWidgetsPerPage()
{
    // OverviewPage::coinJoinStatus() kept its "already applied" state in
    // function-local statics, shared by every page in the process: a second
    // page -- another wallet, or a recreated window -- skipped its first update
    // and showed the advanced CoinJoin widgets with the advanced UI off. That
    // made the second window 78 px taller in initialWindowGeometry. Two windows
    // open at once must agree.
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);
    std::vector<std::unique_ptr<BitcoinGUI>> windows;
    const auto detach = interfaces::MakeHandler([&] {
        for (auto& window : windows) window->setClientModel(nullptr);
    });
    for (int i = 0; i < 2; ++i) {
        auto window = std::make_unique<BitcoinGUI>(m_node, style.get());
        interfaces::BlockAndHeaderTipInfo tip{};
        window->setClientModel(fixture.client.get(), &tip);
        window->addWallet(fixture.model.get());
        windows.push_back(std::move(window));
    }
    QVERIFY(!fixture.model->getOptionsModel()->getShowAdvancedCJUI());
    for (size_t i = 0; i < windows.size(); ++i) {
        auto* overview = windows[i]->findChild<OverviewPage*>();
        QVERIFY(overview);
        for (const char* name : {"labelCompletitionText", "coinJoinProgress", "labelSubmittedDenomText", "labelSubmittedDenom"}) {
            auto* widget = overview->findChild<QWidget*>(name);
            QVERIFY(widget);
            QVERIFY2(widget->isHidden(), qPrintable(QString("window %1 shows %2 without the advanced CoinJoin UI").arg(i).arg(name)));
        }
    }
}

void AbyssTests::quickControls()
{
    const bool high_dpi = QCoreApplication::testAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    const auto restore_dpi = interfaces::MakeHandler([high_dpi] { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, high_dpi); });
    if (!GUIUtil::fontsLoaded()) QSKIP("Quick-control geometry requires minimal:enable_fonts or a native platform");
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name); // Keep this test's isolated QSettings scope.
    QVERIFY(style);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850);
    window.show(); Settle();
    auto* row = window.findChild<QWidget*>("abyssQuickControls");
    auto* lock = window.findChild<QToolButton*>("walletLockButton");
    auto* selector = window.findChild<QWidget*>("walletSelector");
    QVERIFY(row && lock && selector);
    QVERIFY(row->isVisible());
    QVERIFY(!lock->isEnabled()); // No wallet: visible state, no operation.
    const QString no_wallet = BitcoinGUI::tr("No wallet loaded.");
    QCOMPARE(lock->toolTip(), no_wallet);
    QCOMPARE(lock->accessibleName(), no_wallet);
    window.addWallet(fixture.model.get()); Settle();
    QVERIFY(selector->isHidden()); // One wallet does not need a selector.
    QVERIFY(lock->isVisible()); // The action remains available nevertheless.
    QVERIFY(lock->isEnabled());
    QCOMPARE(lock->toolTip(), BitcoinGUI::tr("Wallet is <b>unencrypted</b>. Click to encrypt it."));
    QCOMPARE(lock->accessibleName(), window.findChild<QAction*>("encryptWalletAction")->text().remove('&'));
    QCOMPARE(lock->parentWidget(), row);
    // Keep the four controls in the window's top-right corner on every page,
    // without overlapping the menus or consuming a sidebar navigation slot.
    auto* navigation = window.findChild<QToolBar*>("navigationToolBar");
    auto* top_bar = window.findChild<QToolBar*>("abyssQuickControlsBar");
    QVERIFY(navigation);
    QVERIFY(!navigation->isAncestorOf(row));
    QVERIFY(row->minimumSizeHint().width() <= 238);
    const auto verify_position = [&] {
        const QRect position = OnPage(window, row);
        QVERIFY(position.right() >= window.width() - 16);
        QVERIFY(position.bottom() < OnPage(window, window.centralWidget()).top());
        if (top_bar) {
            QVERIFY(top_bar->isVisible());
        } else {
            QCOMPARE(window.menuBar()->cornerWidget(Qt::TopRightCorner), row);
            for (auto* action : window.menuBar()->actions()) {
                QVERIFY(!row->geometry().intersects(window.menuBar()->actionGeometry(action)));
            }
        }
        int previous = -1;
        for (const char* name : {"abyssOptionsButton", "abyssPrivacyButton", "walletLockButton", "abyssConsoleButton"}) {
            const auto* button = row->findChild<QToolButton*>(name);
            QVERIFY(button && button->isVisible());
            QVERIFY(button->x() > previous);
            previous = button->geometry().right();
        }
    };
    for (int width : {958, 1280, 1920}) {
        window.resize(width, 850); Settle();
        window.gotoOverviewPage(); Settle(); verify_position();
        window.gotoSendCoinsPage(); Settle(); verify_position();
        window.gotoReceiveCoinsPage(); Settle(); verify_position();
    }
    window.gotoOverviewPage(); window.resize(1280, 850); Settle();
    qInfo() << "QUICK_LAYOUT" << window.size() << row->size() << "rowMin" << row->minimumSizeHint()
            << "overviewMin" << window.findChild<OverviewPage*>()->minimumWidth();
    for (const char* name : {"abyssOptionsButton", "abyssPrivacyButton", "walletLockButton", "abyssConsoleButton"}) {
        auto* button = row->findChild<QToolButton*>(name);
        QVERIFY(button);
        QVERIFY(button->width() >= 40 && button->height() >= 40);
        QCOMPARE(button->focusPolicy(), Qt::StrongFocus);
        QVERIFY(!button->accessibleName().isEmpty());
        QVERIFY(row->rect().contains(button->geometry()));
    }
    auto* sky = qobject_cast<StarfieldWidget*>(window.centralWidget());
    auto* network_card = window.findChild<QFrame*>("networkCard");
    QVERIFY(sky && network_card);
    const QRectF moon_disk(sky->m_moon_position - QPointF(25, 25), QSizeF(50, 50));
    QVERIFY2(moon_disk.bottom() < OnPage(*sky, network_card).top(), "The initial 50 px Moon must be above the opaque block card");
    auto* privacy = row->findChild<QToolButton*>("abyssPrivacyButton");
    auto* privacy_action = window.findChild<QAction*>("m_mask_values_action");
    QVERIFY(privacy_action);
    QSignalSpy privacy_signal(&window, &BitcoinGUI::setPrivacy);
    const bool initial = privacy_action->isChecked();
    privacy->click();
    QCOMPARE(privacy_action->isChecked(), !initial);
    QCOMPARE(privacy->isChecked(), !initial);
    QCOMPARE(privacy_signal.count(), 1);
    privacy->setFocus();
    QTest::keyClick(privacy, Qt::Key_Space);
    QCOMPARE(privacy_action->isChecked(), initial);
    QCOMPARE(privacy_signal.count(), 2);
    // Disconnect only operation slots on this disposable window: prove dispatch
    // to existing QActions without opening dialogs or encrypting even a mock wallet.
    for (const char* name : {"optionsAction", "openRPCConsoleAction", "encryptWalletAction", "lockWalletAction", "unlockWalletAction"}) {
        auto* action = window.findChild<QAction*>(name);
        QVERIFY(action);
        QObject::disconnect(action, SIGNAL(triggered(bool)), nullptr, nullptr);
    }
    for (const auto& pair : {std::make_pair("abyssOptionsButton", "optionsAction"), std::make_pair("abyssConsoleButton", "openRPCConsoleAction")}) {
        auto* button = row->findChild<QToolButton*>(pair.first);
        auto* action = window.findChild<QAction*>(pair.second);
        QSignalSpy triggered(action, &QAction::triggered);
        action->setEnabled(false); QVERIFY(!button->isEnabled());
        action->setEnabled(true); QVERIFY(button->isEnabled());
        action->setIcon(QIcon()); QVERIFY(!button->icon().isNull()); // ActionChanged cannot erase artwork.
        button->click(); QCOMPARE(triggered.count(), 1);
    }
    const std::vector<std::pair<int, const char*>> states{
        {WalletModel::Unencrypted, "encryptWalletAction"}, {WalletModel::Locked, "unlockWalletAction"},
        {WalletModel::Unlocked, "lockWalletAction"}, {WalletModel::UnlockedForMixingOnly, "lockWalletAction"}};
    for (const auto& state : states) {
        window.setEncryptionStatus(state.first);
        const char* glyph = state.first == WalletModel::Locked ? "lock" : "lock-open";
        const GUIUtil::ThemedColor color = state.first == WalletModel::Locked ? GUIUtil::ThemedColor::GREEN :
            state.first == WalletModel::UnlockedForMixingOnly ? GUIUtil::ThemedColor::ORANGE : GUIUtil::ThemedColor::RED;
        const QPixmap expected = GUIUtil::getTintedSvgPixmap(QString(":/icons/tabler/") + glyph + ".svg",
            lock->iconSize(), GUIUtil::getThemedQColor(color));
        QCOMPARE(lock->icon().pixmap(window.windowHandle(), lock->iconSize()).toImage(), expected.toImage());
        const auto* status = window.findChild<QLabel*>("labelWalletEncryptionIcon");
        QVERIFY(status);
        QCOMPARE(status->pixmap(Qt::ReturnByValue).toImage(), GUIUtil::getTintedSvgPixmap(QString(":/icons/tabler/") + glyph + ".svg",
            QSize(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE), GUIUtil::getThemedQColor(color)).toImage());
        auto* action = window.findChild<QAction*>(state.second);
        action->setEnabled(true);
        QSignalSpy triggered(action, &QAction::triggered);
        QVERIFY(lock->isEnabled());
        lock->click(); QCOMPARE(triggered.count(), 1);
        action->setEnabled(false); QVERIFY(!lock->isEnabled());
    }
    window.setEncryptionStatus(fixture.model->getEncryptionStatus());
    window.setStakingStatus();
    auto* staking = window.findChild<QLabel*>("labelStakingIcon");
    QVERIFY(staking);
    const qint64 cache_key = staking->pixmap(Qt::ReturnByValue).cacheKey();
    window.setStakingStatus();
    QCOMPARE(staking->pixmap(Qt::ReturnByValue).cacheKey(), cache_key);
    auto* proxy = window.findChild<QLabel*>("labelProxyIcon");
    QVERIFY(proxy);
    proxy->clear();
    window.setProxyIcon(false, QString());
    QVERIFY(proxy->isHidden());
    window.setProxyIcon(true, "127.0.0.1:9050");
    QVERIFY(proxy->isVisible()); // First enable in Abyss must show the new artwork.
    const QString proxy_tooltip = proxy->toolTip();
    window.setProxyIcon(false, QString());
    QVERIFY(proxy->isHidden());
    Theme("Light"); Settle();
    QVERIFY(proxy->isHidden()); // Theme redraw must retain cached hidden state.
    window.setProxyIcon(true, "127.0.0.1:9050");
    QVERIFY(proxy->isVisible());
    QCOMPARE(proxy->toolTip(), proxy_tooltip);
    QCOMPARE(proxy->pixmap(Qt::ReturnByValue).toImage(),
        GUIUtil::getIcon("proxy", GUIUtil::ThemedColor::GREEN).pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE).toImage());
    Theme("Abyss"); Settle();
    window.setProxyIcon(false, QString()); // Restore fixture's actual no-proxy presentation.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        QVERIFY(row->grab().save(QDir(output).filePath("Abyss-quick-controls.png")));
        // Hide only the disposable fixture's sync overlay for this design
        // capture. Production warnings and synchronization logic are unchanged.
        auto* overlay = window.findChild<ModalOverlay*>();
        QVERIFY(overlay);
        overlay->hide(); Settle();
        QVERIFY(window.grab().save(QDir(output).filePath("Abyss-test-wallet-window.png")));
    }
    for (const char* classic : {"Light", "Dark", "Traditional"}) {
        Theme(classic); Settle();
        QVERIFY(row->isHidden());
        if (top_bar) {
            QVERIFY(top_bar->isHidden());
        } else {
            QCOMPARE(window.menuBar()->cornerWidget(Qt::TopRightCorner), window.findChild<QToolButton*>("consoleButton"));
        }
        QCOMPARE(lock->parentWidget(), selector);
        QCOMPARE(lock->focusPolicy(), Qt::NoFocus);
        QCOMPARE(lock->accessibleName(), QString());
        auto* encrypt_action = window.findChild<QAction*>("encryptWalletAction");
        encrypt_action->setEnabled(false);
        // The hidden one-wallet selector can disable its descendants. The
        // button itself must keep the classic explicit-enabled policy.
        QVERIFY(!lock->testAttribute(Qt::WA_ForceDisabled));
        encrypt_action->setEnabled(true);
        QCOMPARE(lock->icon().pixmap(window.windowHandle(), lock->iconSize()).toImage(),
            GUIUtil::getIcon("lock_open", GUIUtil::ThemedColor::RED).pixmap(window.windowHandle(), lock->iconSize()).toImage());
        const auto* status = window.findChild<QLabel*>("labelWalletEncryptionIcon");
        QCOMPARE(status->pixmap(Qt::ReturnByValue).toImage(),
            GUIUtil::getIcon("lock_open", GUIUtil::ThemedColor::RED).pixmap(STATUSBAR_ICONSIZE, STATUSBAR_ICONSIZE).toImage());
#ifndef Q_OS_MAC
        QVERIFY(window.findChild<QToolButton*>("consoleButton")->isVisible());
#endif
        Theme("Abyss"); Settle();
        QVERIFY(row->isVisible());
        verify_position();
        QCOMPARE(lock->parentWidget(), row);
#ifndef Q_OS_MAC
        QVERIFY(window.findChild<QToolButton*>("consoleButton")->isHidden());
#endif
    }
    window.removeWallet(fixture.model.get()); Settle();
    QVERIFY(!lock->isEnabled()); // A closed wallet leaves no operable stale lock.
    QCOMPARE(lock->toolTip(), no_wallet);
    QCOMPARE(lock->accessibleName(), no_wallet);
    Theme("Light"); Settle();
    QCOMPARE(lock->toolTip(), BitcoinGUI::tr("Wallet is <b>unencrypted</b>. Click to encrypt it."));
    QCOMPARE(lock->accessibleName(), QString());
    Theme("Abyss"); Settle();
    QCOMPARE(lock->toolTip(), no_wallet);
    QCOMPARE(lock->accessibleName(), no_wallet);
    QVERIFY(!lock->isEnabled());
}

void AbyssTests::topMenus_data()
{
    QTest::addColumn<bool>("multisig");
    QTest::newRow("without-multisig") << false;
    QTest::newRow("with-multisig") << true;
}

void AbyssTests::topMenus()
{
    QFETCH(bool, multisig);
    RestoreAppearance restore;
    QSettings().setValue("fShowMultisigTab", multisig);
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);
    // Start in a classic theme: the new presentation must not change it.
    Theme("Light");
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850); window.show(); Settle();
    auto* row = window.findChild<QWidget*>("abyssMenuControls");
    auto* wallet = window.findChild<QMenu*>("abyssWalletMenu");
    auto* tools = window.findChild<QMenu*>("abyssToolsMenu");
    auto* security = window.findChild<QMenu*>("abyssSecurityMenu");
    QVERIFY(row && wallet && tools && security);
    QVERIFY(row->isHidden());
    QCOMPARE(window.classicMenus.size(), 4);
    const auto original_menus = window.classicMenus;
    QMap<QMenu*, QList<QAction*>> original_actions;
    for (QMenu* menu : original_menus) original_actions.insert(menu, menu->actions());
    // Every old operation is still reachable. Options/privacy move to the
    // existing direct controls; all others must be present in the new menus.
    const auto contains = [](QMenu* menu, QAction* target) {
        std::function<bool(QMenu*)> find = [&](QMenu* current) {
            for (QAction* action : current->actions()) {
                if (action == target || (action->menu() && find(action->menu()))) return true;
                // A paired row reaches both of its actions (U3-B).
                for (const auto* cell : AbyssMenus::pairCells(action)) {
                    if (cell->action() == target) return true;
                }
            }
            return false;
        };
        return find(menu);
    };
    for (QMenu* menu : original_menus) {
        for (QAction* action : menu->actions()) {
            if (action->isSeparator() || action == window.optionsAction || action == window.m_mask_values_action) continue;
            QVERIFY2(contains(wallet, action) || contains(tools, action) || contains(original_menus.last(), action), qPrintable(action->text()));
        }
    }
    QCOMPARE(tools->actions().contains(window.multisigAction), multisig);
    auto* privacy = window.findChild<QToolButton*>("abyssPrivacyButton");
    QVERIFY(privacy);
    const bool enabled = window.encryptWalletAction->isEnabled();
    const bool checked = window.m_mask_values_action->isChecked();
    const auto shortcut = window.m_mask_values_action->shortcut();
    Theme("Abyss"); Settle();
    QCOMPARE(window.encryptWalletAction->isEnabled(), enabled);
    QCOMPARE(window.m_mask_values_action->isChecked(), checked);
    QCOMPARE(window.m_mask_values_action->shortcut(), shortcut);
    window.addWallet(fixture.model.get()); Settle();
    for (int state : {WalletModel::Unencrypted, WalletModel::Locked, WalletModel::Unlocked, WalletModel::UnlockedForMixingOnly}) {
        window.setEncryptionStatus(state);
        for (QAction* action : {window.encryptWalletAction, window.changePassphraseAction, window.unlockWalletAction, window.lockWalletAction}) {
            QVERIFY(security->actions().contains(action));
            const bool was_enabled = action->isEnabled();
            const bool was_visible = action->isVisible();
            // A full theme change refreshes from the real fixture wallet,
            // which is unencrypted. For injected encryption states, test only
            // menu presentation; real-wallet theme round trips follow below.
            window.updateAbyssMenus();
            QCOMPARE(action->isEnabled(), was_enabled);
            QCOMPARE(action->isVisible(), was_visible);
        }
    }
    window.setEncryptionStatus(fixture.model->getEncryptionStatus());
    if (!window.appMenuBar->isNativeMenuBar()) {
        QVERIFY(row->isVisible());
        QCOMPARE(window.appMenuBar->cornerWidget(Qt::TopLeftCorner), row);
        for (int width : {958, 1280}) {
            window.resize(width, 850); Settle();
            QVERIFY(window.appMenuBar->rect().contains(row->geometry()));
            auto* quick = window.findChild<QWidget*>("abyssQuickControls");
            QVERIFY(quick && !row->geometry().intersects(quick->geometry()));
        }
        for (const char* name : {"abyssWalletMenuButton", "abyssToolsMenuButton", "abyssHelpMenuButton"}) {
            auto* button = row->findChild<QToolButton*>(name);
            QVERIFY(button && button->isVisible() && button->menu());
            QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
            QCOMPARE(button->focusPolicy(), Qt::StrongFocus);
            QVERIFY(!button->icon().isNull());
            QVERIFY(!button->accessibleName().isEmpty());
            bool keyboard_opened{false};
            button->setFocus();
            QTimer::singleShot(0, &window, [&] {
                keyboard_opened = button->menu()->isVisible();
                if (keyboard_opened) QTest::keyClick(button->menu(), Qt::Key_Escape);
            });
            QTest::keyClick(button, Qt::Key_Space);
            QVERIFY(keyboard_opened);
            QVERIFY(!button->menu()->isVisible());
        }
        const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
        if (!output.isEmpty() && !multisig) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(window.appMenuBar->grab().save(QDir(output).filePath("Abyss-top-menus.png")));
            wallet->popup(row->mapToGlobal(QPoint(0, row->height()))); Settle();
            QVERIFY(wallet->grab().save(QDir(output).filePath("Abyss-wallet-menu.png")));
            wallet->close();
            tools->popup(row->mapToGlobal(QPoint(0, row->height()))); Settle();
            QVERIFY(tools->grab().save(QDir(output).filePath("Abyss-tools-menu.png")));
            tools->close();
        }
    }
    for (const char* theme : {"Light", "Dark", "Traditional"}) {
        Theme(theme); Settle();
        QVERIFY(row->isHidden());
        QVERIFY(!wallet->menuAction()->isVisible());
        QVERIFY(!tools->menuAction()->isVisible());
        for (QMenu* menu : original_menus) {
            QVERIFY(menu->menuAction()->isVisible());
            QCOMPARE(menu->actions(), original_actions.value(menu));
        }
        Theme("Abyss"); Settle();
    }
    window.removeWallet(fixture.model.get()); Settle();
    QVERIFY(!window.encryptWalletAction->isEnabled());
    QVERIFY(!window.backupWalletAction->isEnabled());
}

void AbyssTests::menuPairs()
{
    // AbyssMenus::addPair on a menu of its own: two actions side by side in
    // one row, each a cell that highlights and triggers alone (U3-B). Short
    // texts, so that the menu and its list fit minimal's 240x320 screen.
    RestoreAppearance restore;
    Theme("Abyss");
    QWidget host;
    host.setGeometry(0, 0, 240, 300);
    host.show();
    QApplication::setActiveWindow(&host);
    Settle();
    QMenu menu(&host);
    QMenu list(&host);
    QAction before("&Be"), a("&Al"), b("&Bo"), c("&Ch"), d("De"), after("&Zu"), e("&Ec"), f("Fo"), l1("L1"), l2("L2");
    a.setStatusTip("alpha tip");
    b.setStatusTip("bravo tip");
    list.addAction(&l1);
    list.addAction(&l2);
    menu.addAction(&before);
    QWidgetAction* r1 = AbyssMenus::addPair(&menu, &a, &b);
    menu.addSeparator();
    QWidgetAction* r2 = AbyssMenus::addPair(&menu, &c, &d, &list);
    menu.addAction(&after);
    QWidgetAction* r3 = AbyssMenus::addPair(&menu, &e, &f);
    f.setEnabled(false);
    std::map<QAction*, std::unique_ptr<QSignalSpy>> spies;
    for (QAction* action : {&before, &a, &b, &c, &d, &after, &e, &f, &l1}) {
        spies[action] = std::make_unique<QSignalSpy>(action, &QAction::triggered);
    }
    const auto counts = [&] {
        std::map<QAction*, int> now;
        for (const auto& [action, spy] : spies) now[action] = spy->count();
        return now;
    };
    // Exactly `only` fired since `was` (none with nullptr).
    const auto fired = [&](const std::map<QAction*, int>& was, QAction* only) {
        for (const auto& [action, count] : counts()) {
            if (count != was.at(action) + (action == only ? 1 : 0)) return false;
        }
        return true;
    };
    const auto open = [&] {
        CloseMenus();
        // Away from where the menu opens: QMenu takes a move as a move only
        // once the pointer has left where it was when the menu opened.
        PointAt(&host, host.mapToGlobal(QPoint(230, 290)));
        menu.popup(host.mapToGlobal(QPoint(0, 0)));
        Settle();
        return menu.isVisible();
    };
    // The entry on display: a plain action, a row's current cell, or nothing.
    const auto current = [&]() -> QAction* {
        QAction* active = menu.activeAction();
        const auto cells = AbyssMenus::pairCells(active);
        if (cells.isEmpty()) return active;
        for (const auto* cell : cells) {
            if (cell->isSelected()) return cell->action();
        }
        return nullptr;
    };
    const auto local = [&](QWidget* widget) { return menu.windowHandle()->mapFromGlobal(Centre(widget)); };
    const auto move = [&](QWidget* widget) { PointAt(&menu, Centre(widget)); };
    const auto click = [&](QWidget* widget) { QTest::mouseClick(menu.windowHandle(), Qt::LeftButton, {}, local(widget)); };
    const auto key = [&](QWidget* target, Qt::Key k) { QTest::keyClick(target, k); };

    // Layout: one line per row, equal halves, cells of all rows lined up.
    QVERIFY(open());
    const auto cells1 = AbyssMenus::pairCells(r1);
    const auto cells2 = AbyssMenus::pairCells(r2);
    const auto cells3 = AbyssMenus::pairCells(r3);
    QCOMPARE(cells1.size(), 2);
    QCOMPARE(cells2.size(), 2);
    QCOMPARE(cells3.size(), 2);
    QVERIFY(AbyssMenus::pairCells(&before).isEmpty());
    const auto on_menu = [&](QWidget* widget) { return QRect(widget->mapTo(&menu, QPoint()), widget->size()); };
    for (const auto& cells : {cells1, cells2, cells3}) {
        QCOMPARE(on_menu(cells[0]).top(), on_menu(cells[1]).top());
        QCOMPARE(cells[0]->height(), cells[1]->height());
        QVERIFY(on_menu(cells[0]).right() < on_menu(cells[1]).left());
        QVERIFY(std::abs(cells[0]->width() - cells[1]->width()) <= 1);
        for (auto* cell : cells) {
            QVERIFY(cell->width() >= cell->sizeHint().width());
            QVERIFY(menu.rect().contains(on_menu(cell)));
        }
    }
    QCOMPARE(on_menu(cells1[1]).left(), on_menu(cells2[1]).left());
    QCOMPARE(cells1[0]->accessibleName(), QString("Al"));
    QCOMPARE(cells1[1]->accessibleName(), QString("Bo"));
    QCOMPARE(cells2[1]->accessibleName(), QString("De"));
    QCOMPARE(cells1[0]->accessibleDescription(), QString("alpha tip"));
    QVERIFY(!cells1[0]->hasSubmenu() && !cells2[0]->hasSubmenu() && cells2[1]->hasSubmenu());
    // The row stands for its actions; they are not in the menu themselves.
    for (QAction* shared : {&a, &b, &c, &d, &e, &f}) QVERIFY(!menu.actions().contains(shared));
    QVERIFY(r1->isEnabled() && r2->isEnabled() && r3->isEnabled());

    // Keyboard: up/down between rows, left/right between cells; a disabled
    // cell is passed over; the list's cell opens nothing until asked.
    QList<QAction*> seen;
    const auto press = [&](Qt::Key k) {
        key(&menu, k);
        seen << current();
    };
    for (Qt::Key k : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Right, Qt::Key_Left, Qt::Key_Right, Qt::Key_Right, Qt::Key_Down, Qt::Key_Right}) press(k);
    QVERIFY(!list.isVisible());
    for (Qt::Key k : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Right}) press(k);
    QCOMPARE(seen, (QList<QAction*>{&before, &a, &b, &a, &b, &b, &c, &d, &after, &e, &e}));
    QVERIFY(menu.isVisible());
    QVERIFY(fired({{&before, 0}, {&a, 0}, {&b, 0}, {&c, 0}, {&d, 0}, {&after, 0}, {&e, 0}, {&f, 0}, {&l1, 0}}, nullptr));

    // Enter triggers the current cell's action, only that one, once, and closes the menu.
    struct Route {
        std::vector<Qt::Key> keys;
        QAction* target;
    };
    for (const Route& route : {Route{{Qt::Key_Down, Qt::Key_Down}, &a}, Route{{Qt::Key_Down, Qt::Key_Down, Qt::Key_Right}, &b},
                               Route{{Qt::Key_Down, Qt::Key_Down, Qt::Key_Down}, &c},
                               Route{{Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Down}, &e}}) {
        QVERIFY(open());
        for (Qt::Key k : route.keys) key(&menu, k);
        QCOMPARE(current(), route.target);
        const auto was = counts();
        key(&menu, Qt::Key_Return);
        QVERIFY2(fired(was, route.target), qPrintable(route.target->text()));
        QVERIFY(!menu.isVisible());
    }

    // The list's cell: Right opens the list with its first entry active, Left
    // in the list comes back to the cell, Enter opens it and triggers nothing.
    QVERIFY(open());
    for (Qt::Key k : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Right}) key(&menu, k);
    QCOMPARE(current(), &d);
    QVERIFY(!list.isVisible());
    key(&menu, Qt::Key_Right);
    QVERIFY(list.isVisible());
    QCOMPARE(list.activeAction(), &l1);
    key(&list, Qt::Key_Left);
    QVERIFY(!list.isVisible() && menu.isVisible());
    QCOMPARE(current(), &d);
    key(&menu, Qt::Key_Left);
    QCOMPARE(current(), &c);
    QCOMPARE(r2->menu(), nullptr); // the list hangs on the row only while its cell is current
    key(&menu, Qt::Key_Right);
    {
        const auto was = counts();
        key(&menu, Qt::Key_Return);
        QVERIFY(list.isVisible());
        QVERIFY(fired(was, nullptr));
    }
    key(&list, Qt::Key_Escape);
    QVERIFY(!list.isVisible() && menu.isVisible());
    key(&menu, Qt::Key_Escape);
    QVERIFY(!menu.isVisible());

    // Mouse: the cell under the pointer is current, with its text and status
    // tip on the row; a click triggers it; a disabled cell does nothing.
    QVERIFY(open());
    QVERIFY2(menu.hasMouseTracking(), qPrintable(QString("menu without mouse tracking; style %1, hint %2")
                                                     .arg(menu.style()->metaObject()->className())
                                                     .arg(menu.style()->styleHint(QStyle::SH_Menu_MouseTracking, nullptr, &menu))));
    move(cells1[0]);
    QCOMPARE(menu.activeAction(), r1);
    QCOMPARE(current(), &a);
    QCOMPARE(r1->text(), QString("Al"));
    QCOMPARE(r1->statusTip(), QString("alpha tip"));
    move(cells1[1]);
    QCOMPARE(current(), &b);
    QCOMPARE(r1->text(), QString("Bo"));
    QCOMPARE(r1->statusTip(), QString("bravo tip"));
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        QVERIFY(menu.grab().save(QDir(output).filePath("Abyss-menu-pairs.png")));
    }
    {
        const auto was = counts();
        click(cells1[1]);
        QVERIFY(fired(was, &b));
        QVERIFY(!menu.isVisible());
    }
    QVERIFY(open());
    move(cells3[1]);
    QCOMPARE(menu.activeAction(), r3);
    QCOMPARE(current(), nullptr);
    {
        const auto was = counts();
        click(cells3[1]);
        QVERIFY(menu.isVisible());
        QVERIFY(fired(was, nullptr));
    }
    // The list opens on hover over its cell, closes when the pointer moves to
    // the other cell (that move reaches the menu through the open list), and
    // a click on the other cell with the list open triggers that cell.
    move(cells2[1]);
    QCOMPARE(current(), &d);
    QTRY_VERIFY(list.isVisible());
    move(cells2[0]);
    QVERIFY(!list.isVisible());
    QCOMPARE(current(), &c);
    QCOMPARE(r2->menu(), nullptr);
    move(cells2[1]);
    QTRY_VERIFY(list.isVisible());
    if (!output.isEmpty()) QVERIFY(list.grab().save(QDir(output).filePath("Abyss-menu-pairs-list.png")));
    {
        const auto was = counts();
        click(cells2[0]);
        QVERIFY(fired(was, &c));
        QVERIFY(!menu.isVisible() && !list.isVisible());
    }
    // A press on the list's cell opens the list; the release keeps it.
    QVERIFY(open());
    QTest::mousePress(menu.windowHandle(), Qt::LeftButton, {}, local(cells2[1]));
    QTRY_VERIFY(list.isVisible());
    QTest::mouseRelease(menu.windowHandle(), Qt::LeftButton, {}, local(cells2[1]));
    QVERIFY(list.isVisible() && menu.isVisible());
    QCOMPARE(spies[&d]->count(), 0);

    // Mnemonics: a letter that is one cell's triggers it; a plain entry's
    // stays QMenu's; "B" is two entries' (Be, and the cell Bo): each press
    // visits the next, nothing fires.
    struct Letter {
        Qt::Key key;
        QAction* fires;
    };
    for (const Letter& letter : {Letter{Qt::Key_A, &a}, Letter{Qt::Key_C, &c}, Letter{Qt::Key_E, &e}, Letter{Qt::Key_Z, &after}}) {
        QVERIFY(open());
        const auto was = counts();
        key(&menu, letter.key);
        QVERIFY2(fired(was, letter.fires), qPrintable(letter.fires->text()));
        QVERIFY(!menu.isVisible());
    }
    QVERIFY(open());
    {
        const auto was = counts();
        key(&menu, Qt::Key_B);
        QCOMPARE(current(), &before);
        key(&menu, Qt::Key_B);
        QCOMPARE(current(), &b);
        key(&menu, Qt::Key_B);
        QCOMPARE(current(), &before);
        QVERIFY(menu.isVisible());
        QVERIFY(fired(was, nullptr));
    }
    // Only enabled entries match (Astra r1 R1: with every wallet closed,
    // Wallet's "B" met the disabled "Backup Wallet…" first and never reached
    // "Show Automatic Backups"). A disabled first match, the reverse order,
    // and a cell against a plain entry: the one enabled match acts.
    struct Disabled {
        QAction* off;
        QAction* fires;
    };
    a.setText("&Qa");
    b.setText("&Qb");
    before.setText("&Qe");
    for (const Disabled& combo : {Disabled{&a, &b}, Disabled{&b, &a}}) {
        before.setEnabled(false);
        combo.off->setEnabled(false);
        QVERIFY(open());
        const auto was = counts();
        key(&menu, Qt::Key_Q);
        QVERIFY2(fired(was, combo.fires), qPrintable(combo.fires->text()));
        QVERIFY(!menu.isVisible());
        combo.off->setEnabled(true);
        before.setEnabled(true);
    }
    // The cell disabled, the plain entry enabled: the plain entry is the one match.
    b.setText("&Bo");
    a.setEnabled(false);
    QVERIFY(open());
    {
        const auto was = counts();
        key(&menu, Qt::Key_Q);
        QVERIFY(fired(was, &before));
        QVERIFY(!menu.isVisible());
    }
    // The plain entry disabled, the cell enabled: the cell is the one match.
    a.setEnabled(true);
    before.setEnabled(false);
    QVERIFY(open());
    {
        const auto was = counts();
        key(&menu, Qt::Key_Q);
        QVERIFY(fired(was, &a));
        QVERIFY(!menu.isVisible());
    }
    before.setEnabled(true);
    a.setText("&Al");
    before.setText("&Be");

    // The cells follow their actions: enabled, text.
    f.setEnabled(true);
    QVERIFY(open());
    for (Qt::Key k : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Down, Qt::Key_Right}) key(&menu, k);
    QCOMPARE(current(), &f);
    {
        const auto was = counts();
        key(&menu, Qt::Key_Return);
        QVERIFY(fired(was, &f));
    }
    a.setEnabled(false);
    QVERIFY(open());
    key(&menu, Qt::Key_Down);
    key(&menu, Qt::Key_Down);
    QCOMPARE(current(), &b);
    key(&menu, Qt::Key_Left);
    QCOMPARE(current(), &b);
    b.setEnabled(false);
    QVERIFY(!r1->isEnabled());
    QVERIFY(open());
    key(&menu, Qt::Key_Down);
    key(&menu, Qt::Key_Down);
    if (menu.style()->styleHint(QStyle::SH_Menu_AllowActiveAndDisabled, nullptr, &menu)) {
        QCOMPARE(menu.activeAction(), r1);
        QCOMPARE(current(), nullptr);
    } else {
        QCOMPARE(current(), &c);
    }
    a.setEnabled(true);
    b.setEnabled(true);
    QVERIFY(r1->isEnabled());
    a.setText("&Alpha");
    QCOMPARE(cells1[0]->accessibleName(), QString("Alpha"));

    // The cells paint in their menu's font, as QMenu paints its own entries,
    // also when that is not the cells' own font: where a desktop gives menus a
    // font of their own (Windows, macOS, KDE), a cell's own font is the
    // application's (the native trial found the cells larger, the right one
    // cut off). The same text in a plain entry and in a cell leaves the same ink.
    CloseMenus();
    {
        QMenu font_menu(&host);
        const QFont app_font = QApplication::font();
        QFont menu_font = app_font;
        QFont cell_font = app_font;
        cell_font.setFamily(app_font.family());
        if (app_font.pointSizeF() > 0) {
            menu_font.setPointSizeF(app_font.pointSizeF() / 2);
            cell_font.setPointSizeF(app_font.pointSizeF());
        } else {
            menu_font.setPixelSize(std::max(4, app_font.pixelSize() / 2));
            cell_font.setPixelSize(app_font.pixelSize());
        }
        font_menu.setFont(menu_font);
        QAction plain("WWW"), left("WWW"), right("Ww");
        font_menu.addAction(&plain);
        const auto font_cells = AbyssMenus::pairCells(AbyssMenus::addPair(&font_menu, &left, &right));
        QCOMPARE(font_cells.size(), 2);
        for (auto* cell : font_cells) cell->setFont(cell_font);
        PointAt(&host, host.mapToGlobal(QPoint(230, 290)));
        font_menu.popup(host.mapToGlobal(QPoint(0, 0)));
        Settle();
        QVERIFY(font_menu.isVisible());
        const QImage image = font_menu.grab().toImage();
        // The width of what is drawn in a rectangle of the menu: the columns
        // holding a pixel unlike the rectangle's right end, where no text is.
        const auto ink = [&](const QRect& logical) {
            const qreal dpr = image.devicePixelRatio();
            const QRect r = QRectF(logical.x() * dpr, logical.y() * dpr, logical.width() * dpr, logical.height() * dpr).toAlignedRect() & image.rect();
            const QRgb ground = image.pixel(r.right() - 1, r.center().y());
            int first = -1, last = -1;
            for (int x = r.left(); x <= r.right(); ++x) {
                for (int y = r.top(); y <= r.bottom(); ++y) {
                    const QRgb pixel = image.pixel(x, y);
                    if (std::abs(qRed(pixel) - qRed(ground)) + std::abs(qGreen(pixel) - qGreen(ground)) + std::abs(qBlue(pixel) - qBlue(ground)) > 96) {
                        if (first < 0) first = x;
                        last = x;
                        break;
                    }
                }
            }
            return first < 0 ? 0 : last - first + 1;
        };
        const int plain_ink = ink(font_menu.actionGeometry(&plain));
        const int cell_ink = ink(QRect(font_cells[0]->mapTo(&font_menu, QPoint()), font_cells[0]->size()));
        // The font-less minimal run draws no text at all: there the ink says nothing.
        if (GUIUtil::fontsLoaded() || plain_ink > 0) {
            QVERIFY2(plain_ink > 0 && std::abs(cell_ink - plain_ink) <= 2, qPrintable(QString("ink: plain %1, cell %2").arg(plain_ink).arg(cell_ink)));
        }
        font_menu.hide();
    }
    CloseMenus();
}

void AbyssTests::hoverMenus()
{
    // AbyssMenus::HoverMenus on buttons of its own, small enough for minimal's
    // 240x320 screen, so that every menu opens below its button, clear of the
    // others (on the real window there, the menus cover the bar: see
    // topMenusHover). Pointer moves take the window system's path.
    RestoreAppearance restore;
    Theme("Abyss");
    QWidget host;
    host.setGeometry(0, 0, 240, 300);
    auto* layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    QAction a1("A1"), a2("A2"), b1("B1"), c1("C1"), s1("S1"), other_action("Ot");
    QMenu first(&host), second(&host), third(&host), sub(&host);
    sub.setTitle("Su");
    first.addAction(&a1);
    first.addMenu(&sub);
    first.addAction(&a2);
    second.addAction(&b1);
    third.addAction(&c1);
    sub.addAction(&s1);
    QList<QToolButton*> buttons;
    for (QMenu* menu : {&first, &second, &third}) {
        auto* button = new QToolButton(&host);
        button->setText(QString("M%1").arg(buttons.size() + 1));
        button->setPopupMode(QToolButton::InstantPopup);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setMenu(menu);
        layout->addWidget(button);
        buttons << button;
    }
    // Not watched, as the quick buttons of the real window are not.
    auto* other = new QToolButton(&host);
    other->setDefaultAction(&other_action);
    layout->addWidget(other);
    QSignalSpy other_spy(&other_action, &QAction::triggered);
    AbyssMenus::HoverMenus hover(buttons, &host);
    QCOMPARE(hover.buttons(), buttons);
    host.show();
    QApplication::setActiveWindow(&host);
    Settle();
    QVERIFY(host.isActiveWindow());

    QElapsedTimer clock;
    clock.start();
    std::map<const QMenu*, std::vector<qint64>> shown, hidden;
    std::vector<int> levels;
    for (QMenu* menu : {&first, &second, &third, &sub}) {
        QObject::connect(menu, &QMenu::aboutToShow, [&, menu] {
            shown[menu].push_back(clock.elapsed());
            if (menu != &sub) levels.push_back(QThread::currentThread()->loopLevel());
        });
        QObject::connect(menu, &QMenu::aboutToHide, [&, menu] { hidden[menu].push_back(clock.elapsed()); });
    }
    const QPoint away = host.mapToGlobal(QPoint(200, 280));
    const QPoint away2 = host.mapToGlobal(QPoint(210, 270));
    const int open_delay = AbyssMenus::HoverMenus::OPEN_DELAY_MS;
    const int close_delay = AbyssMenus::HoverMenus::CLOSE_DELAY_MS;
    const auto local = [&](const QPoint& global) { return host.windowHandle()->mapFromGlobal(global); };
    // On minimal a closed popup leaves no active window at all
    // (QApplication::activeWindow() is null afterwards); a desktop keeps the
    // main window active. Hover opens only in an active window, so the steps
    // that hover-open after a close make the window active again.
    const auto activate = [&] { QApplication::setActiveWindow(&host); };
    const bool virtual_pointer = QGuiApplication::platformName() == QLatin1String("minimal") ||
                                 QGuiApplication::platformName() == QLatin1String("offscreen");
    // Clear of the buttons and of where the pointer goes.
    QWidget elsewhere;
    elsewhere.setGeometry(205, 5, 30, 30);
    QStringList problems;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) problems << what;
    };
    qint64 mark = 0;
    MenuScript script;
    script.describe([&]() -> QString {
        return PointerState(&host) + QString(" first: tracking %1 active %2 style %3 hint %4")
            .arg(first.hasMouseTracking())
            .arg(first.activeAction() ? first.activeAction()->text() : QString("none"))
            .arg(first.style()->metaObject()->className())
            .arg(first.style()->styleHint(QStyle::SH_Menu_MouseTracking, nullptr, &first));
    });
    // 1. A sweep across a button opens nothing; resting on it opens its menu
    //    after the delay; moving onto a neighbour switches at once, again and again.
    script.then("start away", [&] { PointAt(&host, away); });
    script.then("sweep across the first button", [&] {
        PointAt(&host, Centre(buttons[0]));
        PointAt(&host, away2);
    });
    script.pause("after the sweep", 3 * open_delay);
    script.then("rest on the first button", [&] {
        expect(shown[&first].empty(), "a sweep opened the menu");
        mark = clock.elapsed();
        PointAt(&host, Centre(buttons[0]));
    });
    script.until("first menu open", [&] { return first.isVisible(); });
    script.then("switch to the second", [&] {
        expect(!shown[&first].empty() && shown[&first].back() - mark >= open_delay * 9 / 10, "opened before its delay");
        expect(hover.openButton() == buttons[0] && hover.mouseSession(), "a hover-opened menu is not a mouse session");
        for (int i : {1, 2}) {
            expect(!first.frameGeometry().intersects(QRect(buttons[i]->mapToGlobal(QPoint()), buttons[i]->size())), "a menu covers a button");
        }
        // No move after this one: only the switch can open the second menu (an
        // Enter, and with it the hover delay, needs a move once a popup closed).
        PointAt(&host, Centre(buttons[1]));
        expect(!first.isVisible(), "the first menu stayed open on the switch");
    });
    script.until("second menu open", [&] { return second.isVisible(); });
    script.then("switch to the third", [&] { PointAt(&host, Centre(buttons[2])); });
    script.until("third menu open", [&] { return third.isVisible() && !second.isVisible(); });
    script.then("back to the first", [&] { PointAt(&host, Centre(buttons[0])); });
    script.until("first menu open again", [&] { return first.isVisible() && !third.isVisible(); });
    // 2. An open submenu keeps the menu open; leaving closes both after the delay.
    script.then("onto the submenu's entry", [&] { PointAt(&host, first.mapToGlobal(first.actionGeometry(sub.menuAction()).center())); });
    script.until("submenu open", [&] { return sub.isVisible(); });
    script.then("into the submenu", [&] { PointAt(&host, sub.mapToGlobal(sub.actionGeometry(&s1).center())); });
    script.pause("resting in the submenu", 3 * close_delay);
    script.then("leave", [&] {
        expect(first.isVisible() && sub.isVisible(), "the submenu did not keep the menu open");
        mark = clock.elapsed();
        PointAt(&host, away);
    });
    script.until("closed after the pointer left", [&] { return !first.isVisible() && !sub.isVisible(); });
    script.then("not before its delay", [&] {
        expect(!hidden[&first].empty() && hidden[&first].back() - mark >= close_delay * 9 / 10, "closed before its delay");
    });
    // 3. A click on the open menu's button closes it; it opens again on hover
    //    only after the pointer has left the button once.
    script.then("rest on the first button", [&] {
        activate();
        PointAt(&host, Centre(buttons[0]));
    });
    script.until("open", [&] { return first.isVisible(); });
    script.then("click its button", [&] { QTest::mouseClick(host.windowHandle(), Qt::LeftButton, {}, local(Centre(buttons[0]))); });
    script.until("closed by the click", [&] { return !first.isVisible(); });
    script.then("move on the button", [&] {
        activate();
        PointAt(&host, Centre(buttons[0]) + QPoint(1, 1));
    });
    script.pause("resting on it", 3 * open_delay);
    script.then("leave and come back", [&] {
        // Where the menu closed is QCursor::pos(): minimal's is the last
        // position its window system saw, these moves; a desktop's is the real
        // pointer (Windows: GetCursorPos), which they leave where it was.
        if (virtual_pointer) expect(!first.isVisible(), "the menu opened again under the pointer that closed it");
        PointAt(&host, away2);
        PointAt(&host, Centre(buttons[0]));
    });
    script.until("opens again", [&] { return first.isVisible(); });
    script.then("escape", [&] { QTest::keyClick(&first, Qt::Key_Escape); });
    script.until("closed by escape", [&] { return !first.isVisible(); });
    // 4. A click-opened menu is a mouse session; switching from it takes the
    //    queued path, twice over (Astra note R2).
    script.then("press on the second button", [&] {
        PointAt(&host, away);
        MenuScript::post([&] { QTest::mousePress(host.windowHandle(), Qt::LeftButton, {}, local(Centre(buttons[1]))); });
    });
    script.until("second open by the press", [&] { return second.isVisible(); });
    script.then("release; switch to the third", [&] {
        QTest::mouseRelease(host.windowHandle(), Qt::LeftButton, {}, local(Centre(buttons[1])));
        expect(second.isVisible(), "the release closed the click-opened menu");
        expect(hover.mouseSession(), "a click-opened menu is not a mouse session");
        PointAt(&host, Centre(buttons[2]));
    });
    script.until("third open", [&] { return third.isVisible() && !second.isVisible(); });
    script.then("switch to the first", [&] { PointAt(&host, Centre(buttons[0])); });
    script.until("first open", [&] { return first.isVisible() && !third.isVisible(); });
    script.then("leave", [&] { PointAt(&host, away2); });
    script.until("closed after leaving", [&] { return !first.isVisible(); });
    // 5. A keyboard-opened menu stays when the pointer leaves.
    script.then("open the first with Space", [&] {
        buttons[0]->setFocus();
        MenuScript::post([&] { QTest::keyClick(buttons[0], Qt::Key_Space); });
    });
    script.until("first open by the keyboard", [&] { return first.isVisible(); });
    script.then("pointer away", [&] {
        expect(!hover.mouseSession(), "a keyboard-opened menu is a mouse session");
        PointAt(&host, away);
    });
    script.pause("pointer away", 3 * close_delay);
    script.then("still open; escape", [&] {
        expect(first.isVisible(), "the pointer closed a keyboard-opened menu");
        QTest::keyClick(&first, Qt::Key_Escape);
    });
    script.until("closed", [&] { return !first.isVisible(); });
    // 6. A key pressed in a mouse session makes it a keyboard one (Astra note R1).
    script.then("rest on the first button again", [&] {
        activate();
        PointAt(&host, Centre(buttons[0]));
    });
    script.until("open on hover", [&] { return first.isVisible(); });
    script.then("leave, and press a key at once", [&] {
        expect(hover.mouseSession(), "a hover-opened menu is not a mouse session");
        PointAt(&host, away2);
        QTest::keyClick(&first, Qt::Key_Down);
        expect(!hover.mouseSession(), "a key press left it a mouse session");
    });
    script.pause("pointer away after the key", 3 * close_delay);
    script.then("still open after the key; escape", [&] {
        expect(first.isVisible(), "the pointer closed a menu in which a key was pressed");
        QTest::keyClick(&first, Qt::Key_Escape);
    });
    script.until("closed again", [&] { return !first.isVisible(); });
    // 7. The button that is not watched: resting on it opens and triggers nothing.
    script.then("rest on the other button", [&] { PointAt(&host, Centre(other)); });
    script.pause("resting on the other", 3 * open_delay);
    script.then("nothing from it", [&] {
        expect(!QApplication::activePopupWidget(), "a popup opened over the other button");
        expect(other_spy.isEmpty(), "the other button triggered");
    });
    // 8. Nothing opens on hover while the window is not active: another window
    //    is. (QApplication::setActiveWindow(nullptr) would do on minimal only;
    //    on a desktop QWidget::isActiveWindow() also asks the window system.)
    script.then("activate another window", [&] {
        elsewhere.show();
        elsewhere.activateWindow();
        QApplication::setActiveWindow(&elsewhere);
    });
    script.until("the window inactive", [&] { return !host.isActiveWindow(); });
    script.then("rest on the second button", [&] {
        PointAt(&host, away);
        PointAt(&host, Centre(buttons[1]));
    });
    script.pause("resting while inactive", 3 * open_delay);
    script.then("nothing while inactive", [&] {
        expect(!second.isVisible(), "a menu opened in an inactive window");
        elsewhere.hide();
        QApplication::setActiveWindow(&host);
    });
    const QString failure = script.run();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
    // Every menu opened at the same loop level: none inside another's exec() (Astra note R2).
    QVERIFY(levels.size() >= 8);
    QVERIFY2(std::all_of(levels.begin(), levels.end(), [&](int level) { return level == levels.front(); }),
             qPrintable(QString("loop levels %1").arg([&] { QStringList l; for (int v : levels) l << QString::number(v); return l.join(','); }())));
}

void AbyssTests::topMenuPairs()
{
    // The real Abyss Wallet and Tools menus: the pairs of the roadmap, the
    // wallet list of its own, the rows following their actions' state.
    RestoreAppearance restore;
    QSettings().setValue("fShowMultisigTab", false);
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);
    NodeContext& context = *m_node.context();
    const auto loader = interfaces::MakeWalletLoader(*context.chain, context.coinjoin_loader, *Assert(context.args));
    context.wallet_loader = loader.get();
    const auto forget = interfaces::MakeHandler([&] {
        context.wallet_loader = nullptr;
        RemoveWallet(fixture.wallet, std::nullopt);
    });
    Theme("Abyss");
    {
        BitcoinGUI window(m_node, style.get());
        interfaces::BlockAndHeaderTipInfo tip{};
        window.setClientModel(fixture.client.get(), &tip);
        WalletController controller(*fixture.client, nullptr);
        const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
        window.setWalletController(&controller);
        window.resize(1280, 850);
        window.show();
        Settle();
        if (window.nativeMenuBar()) QSKIP("A native menu bar keeps single rows");
        QMenu* wallet = window.abyssWalletMenu;
        QMenu* tools = window.abyssToolsMenu;
        QVERIFY(wallet && tools && window.abyssOpenWalletMenu);
        using Pair = std::pair<QAction*, QAction*>;
        const auto pair = [](QAction* row) {
            const auto cells = AbyssMenus::pairCells(row);
            return cells.size() == 2 ? Pair{cells[0]->action(), cells[1]->action()} : Pair{nullptr, nullptr};
        };
        const QList<QAction*> w = wallet->actions();
        QCOMPARE(w.size(), 9);
        QVERIFY(pair(w[0]) == Pair(window.m_create_wallet_action, window.m_open_wallet_action));
        QVERIFY(pair(w[1]) == Pair(window.m_close_wallet_action, window.m_close_all_wallets_action));
        QVERIFY(w[2]->isSeparator());
        QCOMPARE(w[3], window.openAction);
        QVERIFY(pair(w[4]) == Pair(window.backupWalletAction, window.showBackupsAction));
        QCOMPARE(w[5]->menu(), window.findChild<QMenu*>("abyssSecurityMenu"));
        QVERIFY(w[6]->menu() && w[6]->menu()->actions().contains(window.toggleStakingAction));
        QVERIFY(w[7]->isSeparator());
        QCOMPARE(w[8], window.quitAction);
        const QList<QAction*> t = tools->actions();
        QVERIFY(pair(t[0]) == Pair(window.usedSendingAddressesAction, window.usedReceivingAddressesAction));
        QVERIFY(pair(t[1]) == Pair(window.signMessageAction, window.verifyMessageAction));
        QCOMPARE(t[2], window.m_load_psbt_action);
        QCOMPARE(t[3], window.m_load_psbt_clipboard_action);
        QCOMPARE(t[4], window.openConfEditorAction);
        QVERIFY(t[5]->isSeparator());
        const QList<QAction*> paired{window.m_create_wallet_action, window.m_open_wallet_action, window.m_close_wallet_action,
                                     window.m_close_all_wallets_action, window.backupWalletAction, window.showBackupsAction,
                                     window.usedSendingAddressesAction, window.usedReceivingAddressesAction,
                                     window.signMessageAction, window.verifyMessageAction};
        for (QAction* shared : paired) {
            QVERIFY(!wallet->actions().contains(shared) && !tools->actions().contains(shared));
            QVERIFY(shared->isVisible());
            QVERIFY(std::any_of(window.classicMenus.begin(), window.classicMenus.end(), [&](QMenu* m) { return m->actions().contains(shared); }));
        }
        QCOMPARE(AbyssMenus::pairCells(w[4]).at(0)->accessibleName(), QString("Backup Wallet…"));
        QCOMPARE(AbyssMenus::pairCells(t[1]).at(0)->accessibleName(), QString("Sign message…"));
        const auto rows_follow = [&] {
            for (QAction* row : {w[0], w[1], w[4], t[0], t[1]}) {
                const Pair p = pair(row);
                if (row->isEnabled() != (p.first->isEnabled() || p.second->isEnabled())) return false;
            }
            return true;
        };
        QVERIFY(rows_follow());

        // The list cell has a menu of its own; the classic Open Wallet keeps
        // its own, and its menu stays its own (Astra note R3).
        const auto cells = AbyssMenus::pairCells(w[0]);
        QVERIFY(cells[1]->hasSubmenu());
        QVERIFY(window.abyssOpenWalletMenu != window.m_open_wallet_menu);
        const auto classic_intact = [&] {
            return window.m_open_wallet_action->menu() == window.m_open_wallet_menu &&
                   window.m_open_wallet_menu->menuAction() == window.m_open_wallet_action;
        };
        QVERIFY(classic_intact());
        QVERIFY(window.m_open_wallet_action->isEnabled() && window.m_create_wallet_action->isEnabled());
        wallet->popup(window.mapToGlobal(QPoint(0, 0)));
        Settle();
        QTest::keyClick(wallet, Qt::Key_Down);
        QCOMPARE(wallet->activeAction(), w[0]);
        QVERIFY(cells[0]->isSelected());
        QTest::keyClick(wallet, Qt::Key_Right);
        QVERIFY(cells[1]->isSelected());
        QVERIFY(classic_intact());
        QTest::keyClick(wallet, Qt::Key_Right);
        QTRY_VERIFY(window.abyssOpenWalletMenu->isVisible());
        QVERIFY(classic_intact());
        window.fillOpenWalletMenu(window.m_open_wallet_menu);
        const auto entries = [](QMenu* menu) {
            QStringList list;
            for (QAction* action : menu->actions()) list << action->text() + (action->isEnabled() ? "" : " (disabled)");
            return list;
        };
        QVERIFY(!entries(window.abyssOpenWalletMenu).isEmpty());
        QCOMPARE(entries(window.abyssOpenWalletMenu), entries(window.m_open_wallet_menu));
        const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(wallet->grab().save(QDir(output).filePath("Abyss-wallet-menu-pairs.png")));
            QVERIFY(window.abyssOpenWalletMenu->grab().save(QDir(output).filePath("Abyss-wallet-open-list.png")));
        }
        QTest::keyClick(window.abyssOpenWalletMenu, Qt::Key_Left);
        QVERIFY(!window.abyssOpenWalletMenu->isVisible());
        QTest::keyClick(wallet, Qt::Key_Left);
        QVERIFY(cells[0]->isSelected());
        QCOMPARE(w[0]->menu(), nullptr);
        QTest::keyClick(wallet, Qt::Key_Escape);
        QVERIFY(!wallet->isVisible());
        QVERIFY(classic_intact());
        if (!output.isEmpty()) {
            tools->popup(window.mapToGlobal(QPoint(0, 0)));
            Settle();
            for (Qt::Key k : {Qt::Key_Down, Qt::Key_Down, Qt::Key_Right}) QTest::keyClick(tools, k);
            QVERIFY(AbyssMenus::pairCells(t[1]).at(1)->isSelected());
            QVERIFY(tools->grab().save(QDir(output).filePath("Abyss-tools-menu-pairs.png")));
            tools->close();
        }

        // The rows follow their actions as wallets come and go.
        const std::vector<WalletModel*> models = controller.getOpenWallets();
        QVERIFY(!models.empty());
        QVERIFY(window.backupWalletAction->isEnabled());
        for (WalletModel* model : models) Q_EMIT model->unload();
        Settle();
        QVERIFY(controller.getOpenWallets().empty());
        QVERIFY(!window.backupWalletAction->isEnabled());
        QVERIFY(rows_follow());
    }
}

void AbyssTests::topMenusHover()
{
    // The real window: the hover watcher has the three Abyss menu buttons and
    // none of the quick buttons; resting on Wallet opens its menu; the shared
    // classic Help menu, opened from the classic menu bar, is none of its own.
    // The finer paths (switch, close, sessions) are hoverMenus': on minimal's
    // 240x320 screen the real menus cover the bar.
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    WalletFixture fixture(m_node);
    Theme("Abyss");
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.addWallet(fixture.model.get());
    window.show();
    Settle();
    // The window holds itself to minimal's screen when shown; widened again,
    // the quick buttons on the right are clear of the menu buttons (as in topMenus).
    window.resize(1280, 850);
    Settle();
    QApplication::setActiveWindow(&window);
    Settle();
    QVERIFY(window.isActiveWindow());
    if (window.nativeMenuBar()) QSKIP("A native menu bar belongs to the platform");
    auto* bar = window.findChild<QWidget*>("abyssMenuControls");
    QVERIFY(bar && bar->isVisible());
    QList<QToolButton*> buttons;
    for (const char* name : {"abyssWalletMenuButton", "abyssToolsMenuButton", "abyssHelpMenuButton"}) {
        buttons << bar->findChild<QToolButton*>(name);
        QVERIFY(buttons.last());
    }
    auto* quick_row = window.findChild<QWidget*>("abyssQuickControls");
    QVERIFY(quick_row);
    QVERIFY(!QRect(bar->mapToGlobal(QPoint()), bar->size()).intersects(QRect(quick_row->mapToGlobal(QPoint()), quick_row->size())));
    QCOMPARE(QApplication::widgetAt(Centre(buttons[0])), buttons[0]);
    QVERIFY(window.abyssMenuHover);
    QCOMPARE(window.abyssMenuHover->buttons(), buttons);
    for (const char* name : {"abyssOptionsButton", "abyssPrivacyButton", "walletLockButton", "abyssConsoleButton"}) {
        auto* quick = window.findChild<QToolButton*>(name);
        QVERIFY(quick);
        QVERIFY(!window.abyssMenuHover->buttons().contains(quick));
    }
    const QPoint away = window.mapToGlobal(QPoint(900, 700));
    QStringList problems;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) problems << what;
    };
    MenuScript script;
    script.describe([&]() -> QString {
        return PointerState(&window) + QString(" wallet button at %1,%2 %3x%4 under-mouse %5 enabled %6 open %7")
            .arg(Centre(buttons[0]).x()).arg(Centre(buttons[0]).y()).arg(buttons[0]->width()).arg(buttons[0]->height())
            .arg(buttons[0]->underMouse()).arg(buttons[0]->isEnabled())
            .arg(window.abyssMenuHover->openButton() ? window.abyssMenuHover->openButton()->objectName() : QString("none"));
    });
    script.then("away", [&] { PointAt(&window, away); });
    script.then("rest on Wallet", [&] { PointAt(&window, Centre(buttons[0])); });
    script.until("the Wallet menu opens", [&] { return window.abyssWalletMenu->isVisible(); });
    script.then("as a mouse session of the watcher's", [&] {
        expect(window.abyssMenuHover->openButton() == buttons[0], "the watcher does not have the Wallet menu");
        expect(window.abyssMenuHover->mouseSession(), "not a mouse session");
    });
    QString failure = script.run();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(!window.abyssWalletMenu->isVisible());

    // On a desktop-sized screen the real menus open below the bar: the walk on
    // the real window, and a hover-open after a close without making the window
    // active again (a desktop keeps it active; minimal does not, see hoverMenus).
    // The native Windows trial runs this part; minimal skips it.
    if (QGuiApplication::platformName() != QLatin1String("minimal")) {
        QMenu* help_menu = window.classicMenus.last();
        script.then("away first", [&] { PointAt(&window, away + QPoint(3, 3)); });
        script.then("rest on Wallet once more", [&] { PointAt(&window, Centre(buttons[0])); });
        script.until("Wallet open", [&] { return window.abyssWalletMenu->isVisible(); });
        script.then("onto Tools", [&] { PointAt(&window, Centre(buttons[1])); });
        script.until("Tools open", [&] { return window.abyssToolsMenu->isVisible() && !window.abyssWalletMenu->isVisible(); });
        script.then("onto Help", [&] { PointAt(&window, Centre(buttons[2])); });
        script.until("Help open", [&] { return help_menu->isVisible() && !window.abyssToolsMenu->isVisible(); });
        script.then("away from the menus", [&] { PointAt(&window, away); });
        script.until("closed after the pointer left", [&] { return !help_menu->isVisible(); });
        script.then("rest on Tools after the close", [&] {
            expect(window.isActiveWindow(), "the window is not active after a menu closed");
            PointAt(&window, Centre(buttons[1]));
        });
        script.until("Tools open again", [&] { return window.abyssToolsMenu->isVisible(); });
        failure = script.run();
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
    }

    Theme("Light");
    Settle();
    for (auto* button : buttons) QVERIFY(!button->isVisible());
    QMenu* help = window.classicMenus.last();
    QVERIFY(help->menuAction()->isVisible());
    script.then("away again", [&] { PointAt(&window, away + QPoint(5, 5)); });
    script.then("open Help from the menu bar", [&] {
        const QRect item = window.appMenuBar->actionGeometry(help->menuAction());
        QTest::mouseClick(window.windowHandle(), Qt::LeftButton, {}, window.windowHandle()->mapFromGlobal(window.appMenuBar->mapToGlobal(item.center())));
    });
    script.until("Help open", [&] { return help->isVisible(); });
    script.then("pointer away from it", [&] {
        expect(!window.abyssMenuHover->openButton(), "the watcher took the classic Help menu");
        PointAt(&window, away);
    });
    script.pause("pointer away", 3 * AbyssMenus::HoverMenus::CLOSE_DELAY_MS);
    script.then("still open", [&] { expect(help->isVisible(), "the classic Help menu closed when the pointer left"); });
    failure = script.run();
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join("; ")));
}

void AbyssTests::skyGeometry()
{
    ControlledSky sky;
    sky.m_epoch_utc_ms = 1792173600000LL;
    sky.resize(1280, 720);
    QVERIFY(sky.m_stars.isEmpty()); // A classic theme starts without a catalogue.
    sky.setSkyVisible(true);
    const auto catalogue = sky.m_stars;
    QVERIFY(!catalogue.isEmpty());
    sky.setSkyVisible(false);
    QVERIFY(sky.m_stars.isEmpty());
    QVERIFY(sky.m_steady.isNull() && sky.m_moon.isNull());
    sky.setSkyVisible(true);
    QCOMPARE(sky.m_stars.size(), catalogue.size());
    for (int i = 0; i < catalogue.size(); ++i) {
        QCOMPARE(sky.m_stars[i].pos, catalogue[i].pos);
        QCOMPARE(sky.m_stars[i].colour, catalogue[i].colour);
    }
    // The sky stands still on a fixed plane: a larger window shows more of the
    // same stars, never different ones, and every quadrant has its share.
    QVector<int> smaller;
    for (const QSize size : {QSize(958, 600), QSize(1280, 720), QSize(1920, 1080), QSize(5120, 2880), QSize(7680, 4320)}) {
        sky.resize(size);
        // Hidden widgets may receive their resize event only when shown.
        sky.updateVisibleStars();
        int quadrants[4]{};
        int inside = 0;
        for (const int index : sky.m_visible) {
            const QPointF p = sky.m_stars.at(index).pos;
            if (!sky.rect().contains(p.toPoint())) continue;
            ++inside;
            ++quadrants[(p.x() >= size.width() / 2 ? 1 : 0) + (p.y() >= size.height() / 2 ? 2 : 0)];
        }
        for (int count : quadrants) QVERIFY(count >= 5);
        QVERIFY(inside >= size.width() * size.height() / 5200 * 0.8 * 0.6);
        for (const int index : smaller) QVERIFY(sky.m_visible.contains(index));
        smaller = sky.m_visible;
    }
    sky.resize(1280, 720);
    QCOMPARE(sky.moonPosition(), QPointF(1280 * 0.78, 720 * 0.09));
    sky.m_moon_position = sky.moonPosition();
    QVERIFY(sky.moonRect().contains((sky.m_moon_position + QPointF(95, 0)).toPoint()));
    QVERIFY(sky.moonRect().contains((sky.m_moon_position - QPointF(95, 0)).toPoint()));
    sky.resize(2560, 1440);
    QCOMPARE(sky.moonPosition().y(), qreal(70)); // Tall windows keep the Moon in the header.
    QCOMPARE(sky.heroPosition(), QPointF(2560 * 0.78, 58)); // A level's scene takes the Moon's place.
}

void AbyssTests::skyCulling()
{
    ControlledSky sky;
    sky.m_epoch_utc_ms = 1792173600000LL;
    sky.resize(1280, 720);
    sky.setSkyVisible(true);
    sky.show();
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();

    // Grow then shrink without regenerating the catalogue. Pruning must never
    // remove a visible star or halo, at any level or physical-pixel scale.
    const auto catalogue = sky.m_stars;
    for (const QSize size : {QSize(1280, 720), QSize(1920, 1080), QSize(958, 600)}) {
        sky.resize(size);
        QApplication::processEvents();
        QCOMPARE(sky.m_stars.size(), catalogue.size());
        const auto visible = sky.m_visible;
        const auto twinklers = sky.m_twinklers;
        QVERIFY(!visible.isEmpty());
        QVERIFY(visible.size() < sky.m_stars.size() / 4);
        for (const int tier : {0, 1, 5}) {
            sky.setSkyTier(tier);
            sky.elapsed = 21541022LL * tier;
            sky.invalidateImages();
            const QImage culled = sky.grab().toImage();

            // Oracle: render every original star, with no pruning.
            sky.m_visible.clear();
            sky.m_twinklers.clear();
            for (int i = 0; i < sky.m_stars.size(); ++i) {
                sky.m_visible.append(i);
                if (sky.m_stars.at(i).amplitude > 0) sky.m_twinklers.append(i);
            }
            sky.invalidateImages();
            const QImage unculled = sky.grab().toImage();
            sky.m_visible = visible;
            sky.m_twinklers = twinklers;
            QCOMPARE(culled, unculled);
        }
    }
    sky.setSkyVisible(false);
    QVERIFY(sky.m_visible.isEmpty() && sky.m_twinklers.isEmpty());
}

void AbyssTests::skyMoon()
{
    const qint64 new_moon = 947182440000LL;
    QCOMPARE(StarfieldWidget::lunarPhase(new_moon), qreal(0));
    QVERIFY(StarfieldWidget::lunarPhase(new_moon - 1000) > 0.99);
    const qint64 half_month = static_cast<qint64>(29.530588 * 86400000 / 2);
    QVERIFY(std::abs(StarfieldWidget::lunarPhase(new_moon + half_month) - 0.5) < 1e-8);
    for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        const QImage waxing = StarfieldWidget::renderMoon(0.25, dpr);
        const QImage waning = StarfieldWidget::renderMoon(0.75, dpr);
        const QImage full = StarfieldWidget::renderMoon(0.5, dpr);
        const QImage dark = StarfieldWidget::renderMoon(0, dpr);
        QCOMPARE(waxing.devicePixelRatioF(), dpr);
        QCOMPARE(waxing.size(), QSize(qCeil(204 * dpr), qCeil(204 * dpr)));
        const int c = waxing.width() / 2, delta = qRound(12 * dpr);
        QVERIFY(waxing.pixelColor(c + delta, c).lightness() > waxing.pixelColor(c - delta, c).lightness() + 30);
        QVERIFY(waning.pixelColor(c - delta, c).lightness() > waning.pixelColor(c + delta, c).lightness() + 30);
        QVERIFY(full.pixelColor(c, c).lightness() > dark.pixelColor(c, c).lightness() + 80);
        QCOMPARE(full.pixelColor(0, 0).alpha(), 0);
        QVERIFY(full.pixelColor(c + qRound(60 * dpr), c).alpha() > 0); // Cached halo.
    }
}

void AbyssTests::skyLifecycle()
{
    ControlledSky sky;
    sky.resize(1280, 720);
    sky.setSkyVisible(true);
    sky.show();
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();
    sky.redrawSteadySky();
    QVERIFY(sky.wantsAnimation());
    QVERIFY(sky.m_timer->isActive());
    sky.elapsed = 1000;
    const qint64 frozen_motion = sky.motionMilliseconds();
    sky.setAnimated(false);
    QVERIFY(!sky.m_timer->isActive());
    const qint64 frozen_phase_time = sky.phaseTimeMilliseconds();
    sky.elapsed += 86400000;
    QCOMPARE(sky.motionMilliseconds(), frozen_motion);
    QCOMPARE(sky.phaseTimeMilliseconds(), frozen_phase_time);
    sky.setAnimated(true);
    QCOMPARE(sky.motionMilliseconds(), frozen_motion); // No position jump on resume.
    QVERIFY(sky.phaseTimeMilliseconds() > frozen_phase_time); // Current date resumes.
    QVERIFY(sky.m_timer->isActive());
    sky.hide();
    QVERIFY(!sky.m_timer->isActive());
    sky.show();
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();
    sky.updateTimerState();
    QVERIFY(sky.m_timer->isActive());
    sky.setWindowState(Qt::WindowMinimized);
    QApplication::processEvents();
    sky.updateTimerState();
    QVERIFY(!sky.m_timer->isActive());
    sky.setWindowState(Qt::WindowNoState);
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();
    sky.updateTimerState();
    QVERIFY(sky.m_timer->isActive());
    QApplication::setActiveWindow(nullptr);
    QApplication::processEvents();
    sky.updateTimerState();
    QVERIFY(!sky.m_timer->isActive());
    sky.setSkyVisible(false);
    QVERIFY(!sky.m_timer->isActive());
    QVERIFY(sky.m_stars.isEmpty() && sky.m_twinklers.isEmpty());
    QVERIFY(sky.m_steady.isNull() && sky.m_moon.isNull());
}

void AbyssTests::skyRegionalRepaint()
{
    ControlledSky sky;
    sky.m_epoch_utc_ms = 1792173600000LL;
    sky.resize(1280, 720);
    sky.setSkyVisible(true);
    sky.setSkyTier(6); // The comet is the one scene element that moves.
    sky.show();
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();
    sky.grab();
    const int redraws = sky.m_steady_redraws;
    sky.elapsed = 10000;
    sky.onTick();
    QVERIFY(!sky.m_steady.isNull()); // A tick never drops the cache.
    QCOMPARE(sky.m_steady_redraws, redraws);
    // What the cache and the live elements compose is what a fresh paint gives.
    const QImage cached = sky.grab().toImage();
    QCOMPARE(sky.m_steady_redraws, redraws);
    sky.m_steady = QPixmap();
    const QImage full = sky.grab().toImage();
    QCOMPARE(cached, full);

    // The comet knows its footprint, and a tick repaints where it was drawn as
    // well as where it is now: after a long pause its old place is erased.
    const auto inside = [&](const QRect& r) { return r.intersected(sky.rect()); };
    const QRect comet = sky.cometRect(sky.m_live_motion_ms);
    QVERIFY(!comet.isEmpty());
    QVERIFY(sky.rect().adjusted(-200, -200, 200, 200).contains(comet));
    sky.grab();
    QVERIFY(sky.m_live_drawn.contains(inside(comet)));
    const qint64 later = sky.elapsed + 120000;
    const QRegion after_pause = sky.tickRegion(later);
    QVERIFY(!sky.cometRect(later).intersects(comet)); // It really moved, so the next check is not vacuous.
    QVERIFY(after_pause.contains(inside(comet)));
    QVERIFY(after_pause.contains(inside(sky.cometRect(later))));

    // A shooting star leaves no trail: every frame's streak is erased by the next tick.
    sky.setSkyTier(1);
    const qint64 t0 = later + 1000;
    QVERIFY(sky.shootingRect(t0).isEmpty());
    sky.startShootingStar(t0);
    QVERIFY(sky.m_next_shooting_ms >= t0 + 600 + 20000); // At most one a minute, never two in twenty seconds.
    QVERIFY(sky.m_next_shooting_ms <= t0 + 600 + 60000);
    sky.tickRegion(t0 + 1);
    sky.grab();
    const QRect first = sky.shootingRect(t0 + 1);
    QVERIFY(!first.isEmpty());
    // The head starts inside the sky; the faint tail may reach past the top edge.
    QVERIFY(!inside(first).isEmpty());
    QVERIFY(sky.m_live_drawn.contains(inside(first)));
    const QRegion next = sky.tickRegion(t0 + 300);
    const QRect middle = sky.shootingRect(t0 + 300);
    QVERIFY(!QRegion(middle).contains(inside(first))); // It really moved.
    QVERIFY(next.contains(inside(first)));
    QVERIFY(next.contains(inside(middle)));
    sky.grab();
    const QRegion over = sky.tickRegion(t0 + 700);
    QCOMPARE(sky.m_shooting_started_ms, qint64(-1)); // Over after 0.6 s.
    QVERIFY(sky.shootingRect(t0 + 700).isEmpty());
    QVERIFY(over.contains(inside(middle)));
    // One starts when it is due, but not in deep space, which looks past the atmosphere.
    sky.tickRegion(sky.m_next_shooting_ms);
    QVERIFY(sky.m_shooting_started_ms >= 0);
    sky.setSkyTier(9);
    QCOMPARE(sky.m_shooting_started_ms, qint64(-1));
    sky.tickRegion(sky.m_next_shooting_ms + 1);
    QCOMPARE(sky.m_shooting_started_ms, qint64(-1));
    // Without a twinkler in view the sky ticks slowly; a streak in flight takes
    // the fast frames, and its landing gives them back. (The field decides where
    // the twinklers are; here none is in view.)
    sky.setSkyTier(1);
    sky.m_twinklers.clear();
    sky.updateTimerState();
    QVERIFY(sky.m_timer->isActive());
    QCOMPARE(sky.m_timer->interval(), 500);
    const qint64 due = sky.m_next_shooting_ms;
    sky.tickRegion(due);
    QVERIFY(sky.m_shooting_started_ms >= 0);
    QCOMPARE(sky.m_timer->interval(), 80);
    sky.tickRegion(due + 300);
    QCOMPARE(sky.m_timer->interval(), 80);
    sky.tickRegion(due + 600);
    QCOMPARE(sky.m_shooting_started_ms, qint64(-1));
    QCOMPARE(sky.m_timer->interval(), 500);
    // A streak in flight is erased when the window stops animating.
    sky.setSkyTier(1);
    sky.startShootingStar(sky.m_next_shooting_ms + 2);
    sky.tickRegion(sky.m_shooting_started_ms + 1);
    sky.grab();
    QVERIFY(!sky.m_live_drawn.isEmpty());
    QApplication::setActiveWindow(nullptr);
    QApplication::processEvents();
    sky.updateTimerState();
    QCOMPARE(sky.m_shooting_started_ms, qint64(-1));
    QVERIFY(sky.m_live_drawn.isEmpty());
    QVERIFY(!sky.m_timer->isActive());
}

void AbyssTests::skyOverviewBackground()
{
    RestoreAppearance restore;
    Theme("Abyss");
    ControlledSky sky;
    sky.setObjectName("walletContainer");
    sky.m_epoch_utc_ms = 1792173600000LL;
    sky.resize(1280, 850);
    sky.setSkyVisible(true);
    sky.setAnimated(false);
    auto* layout = new QVBoxLayout(&sky);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* page = new OverviewPage(&sky);
    layout->addWidget(page);
    sky.show();
    Settle();
    auto* network = page->findChild<QFrame*>("networkCard");
    auto* balances = page->findChild<QFrame*>("frame");
    QVERIFY(network && balances);
    const QRect bar = OnPage(sky, network);
    const QRect card = OnPage(sky, balances);
    const QRect gap(20, bar.bottom() + 2, sky.width() - 40, card.top() - bar.bottom() - 3);
    QVERIFY2(gap.height() >= 4, "Expected an empty gap between network and balance cards");
    const QImage composed = sky.grab().toImage();
    page->hide();
    Settle();
    const QImage bare = sky.grab().toImage();
    const qreal dpr = composed.devicePixelRatioF();
    const QRect pixels(gap.topLeft() * dpr, gap.size() * dpr);
    QCOMPARE(composed.copy(pixels), bare.copy(pixels));
    int star_pixels = 0;
    const QColor backdrop("#04070d");
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            if (bare.pixelColor(x, y) != backdrop) ++star_pixels;
        }
    }
    QVERIFY2(star_pixels > 0, "The tested Overview gap must contain actual sky pixels");
}

void AbyssTests::coinJoinHelp()
{
    // The CoinJoin help came from the chain this one forked from: Dash coins, a
    // link to the Dash documentation, and a promise that masternodes never learn
    // who mixes. The text is the same in every theme; check it in each one and
    // save a picture of the dialog for review.
    RestoreAppearance restore;
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const QString participants = QString("at least %1 participants").arg(Params().PoolMinParticipants());
    QString first;
    for (const char* theme : {"Abyss", "Light", "Dark", "Traditional"}) {
        Theme(theme);
        HelpMessageDialog help(nullptr, HelpMessageDialog::pshelp);
        help.show();
        Settle();
        // The picture first, so a run on the old text still leaves one to compare.
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(help.grab().save(QDir(output).filePath(QString("%1-coinjoin-help.png").arg(theme))));
        }
        auto* message = help.findChild<QLabel*>("aboutMessage");
        QVERIFY(message);
        const QString text = message->text();
        QCOMPARE(help.windowTitle(), QString("CoinJoin information"));
        QVERIFY2(!text.contains("dash", Qt::CaseInsensitive), qPrintable(text));
        QVERIFY2(!text.contains("href", Qt::CaseInsensitive), qPrintable(text));
        QVERIFY2(!text.contains("never know"), qPrintable(text));
        QVERIFY2(!text.contains("%1") && !text.contains("%2"), qPrintable(text));
        QVERIFY2(text.contains(participants), qPrintable(participants));
        for (const char* expected : {"<h3>CoinJoin basics</h3>", "slightly more than 0.001, 0.01, 0.1, 1 and 10 DFCN",
                                     "10.0001 DFCN", "on the main network, mixed coins are smaller than the minimum stake",
                                     "staking never combines them or the mixing collaterals with your other coins",
                                     "not guaranteed to be different people",
                                     "if the wallet is locked", "unlocks for mixing only", "automatic wallet backups"}) {
            QVERIFY2(text.contains(expected), expected);
        }
        if (first.isEmpty()) first = text; else QCOMPARE(text, first);
    }
}

namespace {
//! The staking states the card can be handed, by name.
OverviewPage::StakingStatus StakingCase(const char* name)
{
    OverviewPage::StakingStatus status;
    const QString which(name);
    status.enabled = which != "off";
    if (which == "locked") status.locked = true;
    if (which == "syncing") status.syncing = true;
    if (which == "ready") status.wallet_state = NOT_STAKING_BALANCE;
    if (which == "depth") status.wallet_state = NOT_STAKING_DEPTH;
    if (which == "staking") status.wallet_state = IS_STAKING;
    if (which == "keyless") { status.keyless = true; status.wallet_state = IS_STAKING; }
    if (which == "disabled") { status.minter = false; status.configured = false; }
    if (which == "stopped") status.minter = false;
    return status;
}
//! The color at the centre of the card's dot.
QRgb DotColor(const QLabel* icon)
{
    const QImage image = icon->pixmap(Qt::ReturnByValue).toImage();
    return image.pixelColor(image.width() / 2, image.height() / 2).rgb();
}
} // namespace

void AbyssTests::stakingCard()
{
    // One word for the state, one line about it, one button; the states in
    // the order the thread itself gives up, so the card never says "Staking"
    // about a wallet that cannot sign. On a locked wallet the word alone: the
    // banner says why and holds the unlock.
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setWalletModel(fixture.model.get());
    page.resize(1280, 850);
    page.show(); Settle();
    auto* card = page.findChild<QFrame*>("frameStaking");
    auto* balances = page.findChild<QFrame*>("frame");
    auto* state = page.findChild<QLabel*>("stakingState");
    auto* detail = page.findChild<QLabel*>("stakingDetail");
    auto* button = page.findChild<QPushButton*>("stakingButton");
    auto* icon = page.findChild<QLabel*>("stakingIcon");
    auto* banner = page.findChild<QFrame*>("stakingBanner");
    QVERIFY(card && balances && state && detail && button && icon && banner);
    QVERIFY(card->isVisible());
    QVERIFY(OnPage(page, card).top() > OnPage(page, balances).bottom()); // Under the balances.
    QSignalSpy toggles(&page, &OverviewPage::stakingToggleRequested);
    QSignalSpy unlocks(&page, &OverviewPage::stakingUnlockRequested);
    using State = OverviewPage::StakingState;
    using Color = GUIUtil::ThemedColor;
    struct Case { const char* name; State state; const char* word; const char* button; Color color; };
    const std::vector<Case> cases{
        {"off", State::Off, "Off", "Start staking", Color::UNCONFIRMED},
        {"locked", State::Locked, "Paused: wallet locked", "", Color::ORANGE},
        {"syncing", State::Syncing, "Waiting for sync", "Pause staking", Color::ORANGE},
        {"ready", State::Ready, "Ready", "Pause staking", Color::BLUE},
        {"depth", State::Ready, "Ready", "Pause staking", Color::BLUE},
        {"staking", State::Staking, "Staking", "Pause staking", Color::GREEN},
        {"keyless", State::Unavailable, "Not available", "", Color::UNCONFIRMED},
        {"disabled", State::Disabled, "Off", "", Color::UNCONFIRMED},
        {"stopped", State::Disabled, "Off", "", Color::UNCONFIRMED},
    };
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    int widest = 0;
    for (const Case& which : cases) {
        page.setStakingStatus(StakingCase(which.name)); Settle();
        QVERIFY2(page.stakingState() == which.state, which.name);
        QVERIFY2(state->text() == QString(which.word), qPrintable(QString("%1: %2").arg(which.name, state->text())));
        QVERIFY2(detail->isHidden() == (which.state == State::Locked), which.name);
        QVERIFY2(detail->isHidden() == detail->text().isEmpty(), which.name);
        QVERIFY2(button->isHidden() == QString(which.button).isEmpty(), which.name);
        if (!button->isHidden()) {
            QCOMPARE(button->text(), QString(which.button));
            QCOMPARE(button->accessibleName(), button->text());
            QVERIFY2(button->height() >= 40, qPrintable(QString("%1: button %2 px").arg(which.name).arg(button->height())));
        }
        QVERIFY2(DotColor(icon) == GUIUtil::getThemedQColor(which.color).rgb(), which.name);
        QVERIFY2(banner->isVisible() == (which.state == State::Locked), which.name);
        // The thread's coarse reasons are not promised as waits that end by themselves,
        // and a thread that is down is not blamed on a setting it may not have.
        if (QString(which.name) == "depth") {
            QVERIFY2(!detail->text().contains("mature") && detail->text().contains("None of the coins"), qPrintable(detail->text()));
            QVERIFY(detail->toolTip().contains("collateral"));
        }
        if (QString(which.name) == "disabled") QVERIFY2(detail->text().contains("-staking=0"), qPrintable(detail->text()));
        if (QString(which.name) == "stopped") QVERIFY2(!detail->text().contains("-staking") && detail->text().contains("not running"), qPrintable(detail->text()));
        widest = std::max(widest, card->minimumSizeHint().width());
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(page.grab().save(QDir(output).filePath(QString("Abyss-staking-%1.png").arg(which.name))));
        }
    }
    // The card asks for no more room than the balances card has anyway.
    QVERIFY2(widest <= balances->minimumSizeHint().width(), qPrintable(QString("card %1, balances %2").arg(widest).arg(balances->minimumSizeHint().width())));
    // The button: the switch, by mouse and by keyboard; the unlock is the banner's.
    page.setStakingStatus(StakingCase("off")); Settle();
    QVERIFY(button->focusPolicy() != Qt::NoFocus);
    button->click();
    QCOMPARE(toggles.count(), 1);
    QVERIFY(toggles.last().at(0).toBool()); // "Start staking" asks for on, never for a flip.
    QCOMPARE(unlocks.count(), 0);
    QTest::keyClick(button, Qt::Key_Space);
    QCOMPARE(toggles.count(), 2);
    QVERIFY(toggles.last().at(0).toBool());
    page.setStakingStatus(StakingCase("staking")); Settle();
    button->click();
    QCOMPARE(toggles.count(), 3);
    QVERIFY(!toggles.last().at(0).toBool()); // "Pause staking" asks for off.
    page.setStakingStatus(StakingCase("syncing")); Settle();
    button->click();
    QVERIFY(!toggles.last().at(0).toBool());
    page.setStakingStatus(StakingCase("locked")); Settle();
    QVERIFY(button->isHidden());
    banner->findChild<QPushButton*>("stakingBannerUnlock")->click();
    QCOMPARE(unlocks.count(), 1);
    QCOMPARE(toggles.count(), 4);
    // Discreet mode masks amounts, not states: the card and the banner stay.
    page.setPrivacy(true); Settle();
    QVERIFY(card->isVisible());
    QVERIFY(banner->isVisible());
    page.setPrivacy(false); Settle();
    // A classic theme has neither, whatever the state; Abyss brings them back as they were.
    Theme("Light"); Settle();
    QVERIFY(card->isHidden());
    QVERIFY(banner->isHidden());
    page.setStakingStatus(StakingCase("locked")); Settle();
    QVERIFY(banner->isHidden());
    Theme("Abyss"); Settle();
    QVERIFY(card->isVisible());
    QVERIFY(banner->isVisible());
    QCOMPARE(state->text(), QString("Paused: wallet locked"));
    QVERIFY(detail->isHidden());
    QVERIFY(button->isHidden());
    // Unlocked, the line and the button come back.
    page.setStakingStatus(StakingCase("ready")); Settle();
    QVERIFY(detail->isVisible());
    QVERIFY(button->isVisible());
}

void AbyssTests::stakingBanner()
{
    // Over the network bar, only while the wallet is locked, with its own way out.
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setWalletModel(fixture.model.get());
    page.resize(1280, 850);
    page.show(); Settle();
    auto* banner = page.findChild<QFrame*>("stakingBanner");
    auto* text = page.findChild<QLabel*>("stakingBannerText");
    auto* unlock = page.findChild<QPushButton*>("stakingBannerUnlock");
    auto* bar = page.findChild<QFrame*>("networkCard");
    auto* header = page.findChild<QWidget*>("modernOverviewHeader");
    QVERIFY(banner && text && unlock && bar && header);
    QVERIFY(banner->isHidden());
    QSignalSpy unlocks(&page, &OverviewPage::stakingUnlockRequested);
    page.setStakingStatus(StakingCase("locked")); Settle();
    QVERIFY(banner->isVisible());
    QVERIFY(text->text().contains("locked"));
    QVERIFY(OnPage(page, banner).top() >= OnPage(page, header).bottom());
    QVERIFY(OnPage(page, banner).bottom() <= OnPage(page, bar).top());
    QVERIFY(unlock->height() >= 34);
    unlock->click();
    QCOMPARE(unlocks.count(), 1);
    // A wallet unlocked for mixing (and so for staking) is not locked for the banner.
    OverviewPage::StakingStatus mixing = StakingCase("ready");
    page.setStakingStatus(mixing); Settle();
    QVERIFY(banner->isHidden());
    // A wallet without keys has nothing to unlock for.
    OverviewPage::StakingStatus keyless = StakingCase("locked");
    keyless.keyless = true;
    page.setStakingStatus(keyless); Settle();
    QVERIFY(banner->isHidden());
    page.setStakingStatus(StakingCase("locked")); Settle();
    QVERIFY(banner->isVisible());
    page.setPrivacy(true); Settle();
    QVERIFY(banner->isVisible());
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) QVERIFY(page.grab().save(QDir(output).filePath("Abyss-staking-banner-discreet.png")));
    page.setPrivacy(false); Settle();
    page.setStakingStatus(StakingCase("off")); Settle();
    QVERIFY(banner->isHidden());
}

void AbyssTests::coinJoinCardConditional()
{
    // In Abyss the CoinJoin card stays out of the way until it has something
    // to say -- mixing, or mixed coins -- and a one-line row with a link
    // stands in for it, since the card is where mixing is started. The
    // classic themes show the card whenever CoinJoin is on, as before.
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    QVERIFY(fixture.client->coinJoinOptions().isEnabled());
    const auto open = [&](OverviewPage& page) {
        page.setClientModel(fixture.client.get());
        page.setWalletModel(fixture.model.get());
        page.resize(1280, 850);
        page.show(); Settle();
        page.coinJoinStatus(true); Settle();
    };
    OverviewPage page;
    open(page);
    auto* card = page.findChild<QFrame*>("frameCoinJoin");
    auto* row = page.findChild<QWidget*>("coinJoinRow");
    auto* show = page.findChild<QPushButton*>("coinJoinRowShow");
    auto* staking = page.findChild<QFrame*>("frameStaking");
    QVERIFY(card && row && show && staking);
    QVERIFY(card->isHidden());
    QVERIFY(row->isVisible());
    QVERIFY(OnPage(page, row).top() >= OnPage(page, staking).bottom()); // Where the card stood.
    QVERIFY(show->focusPolicy() != Qt::NoFocus);
    show->click(); Settle();
    QVERIFY(card->isVisible());
    QVERIFY(row->isHidden());
    QVERIFY(OnPage(page, card).top() >= OnPage(page, staking).bottom());
    page.coinJoinStatus(true); Settle(); // The timer's next pass keeps it open.
    QVERIFY(card->isVisible());
    // Mixed coins open it on a fresh page without the link.
    OverviewPage mixed;
    open(mixed);
    QVERIFY(mixed.findChild<QFrame*>("frameCoinJoin")->isHidden());
    interfaces::WalletBalances balances;
    balances.anonymized_balance = 5 * COIN;
    mixed.setBalance(balances); Settle();
    QVERIFY(mixed.findChild<QFrame*>("frameCoinJoin")->isVisible());
    QVERIFY(mixed.findChild<QWidget*>("coinJoinRow")->isHidden());
    balances.anonymized_balance = 0;
    mixed.setBalance(balances); Settle();
    QVERIFY(mixed.findChild<QFrame*>("frameCoinJoin")->isHidden());
    QVERIFY(mixed.findChild<QWidget*>("coinJoinRow")->isVisible());
    // A classic theme: the card, never the row.
    Theme("Light"); Settle();
    OverviewPage classic;
    open(classic);
    QVERIFY(classic.findChild<QFrame*>("frameCoinJoin")->isVisible());
    QVERIFY(classic.findChild<QWidget*>("coinJoinRow")->isHidden());
    QVERIFY(classic.findChild<QFrame*>("frameStaking")->isHidden());
    // And the Abyss page that had the card hidden shows it in a classic theme too.
    QVERIFY(mixed.findChild<QFrame*>("frameCoinJoin")->isVisible());
    QVERIFY(mixed.findChild<QWidget*>("coinJoinRow")->isHidden());
    // Disabled by the node (automatic backups off): Abyss keeps the card behind
    // the link, where it explains itself; a classic page leaves it as it stands.
    const int backups = nWalletBackups;
    nWalletBackups = 0;
    const auto restore_backups = interfaces::MakeHandler([backups] { nWalletBackups = backups; });
    Theme("Abyss"); Settle();
    OverviewPage disabled;
    open(disabled);
    QVERIFY(disabled.findChild<QFrame*>("frameCoinJoin")->isHidden());
    QVERIFY(disabled.findChild<QWidget*>("coinJoinRow")->isVisible());
    disabled.findChild<QPushButton*>("coinJoinRowShow")->click(); Settle();
    QVERIFY(disabled.findChild<QFrame*>("frameCoinJoin")->isVisible());
    QVERIFY(!disabled.findChild<QFrame*>("frameCoinJoin")->isEnabled());
    QVERIFY(disabled.findChild<QPushButton*>("toggleCoinJoin")->text().contains("Disabled"));
    Theme("Light"); Settle();
    // The Abyss page whose link opened it: Light has it as the base would,
    // and the base never shows a card disabled from the start.
    QVERIFY(disabled.findChild<QFrame*>("frameCoinJoin")->isHidden());
    QVERIFY(disabled.findChild<QWidget*>("coinJoinRow")->isHidden());
    OverviewPage classic_disabled;
    open(classic_disabled);
    QVERIFY(classic_disabled.findChild<QFrame*>("frameCoinJoin")->isHidden()); // As before: never shown.
    QVERIFY(classic_disabled.findChild<QWidget*>("coinJoinRow")->isHidden());
    // The other way: a classic card that stood when the backups failed keeps
    // standing, through Abyss (behind the row there) and back.
    nWalletBackups = backups;
    OverviewPage stood;
    open(stood);
    auto* stood_card = stood.findChild<QFrame*>("frameCoinJoin");
    auto* stood_row = stood.findChild<QWidget*>("coinJoinRow");
    QVERIFY(stood_card->isVisible());
    nWalletBackups = 0;
    stood.coinJoinStatus(true); Settle();
    QVERIFY(stood_card->isVisible());
    QVERIFY(!stood_card->isEnabled());
    Theme("Abyss"); Settle();
    QVERIFY(stood_card->isHidden());
    QVERIFY(stood_row->isVisible());
    Theme("Light"); Settle();
    QVERIFY(stood_card->isVisible());
    QVERIFY(stood_row->isHidden());
}

void AbyssTests::nextLevelLine()
{
    // Under the total: the balance the next sky level starts at, as the
    // badge's tooltip has it, or that this is the top. Never in discreet
    // mode, nor for a wallet whose balance does not count.
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setWalletModel(fixture.model.get());
    page.resize(1280, 850);
    page.show(); Settle();
    auto* line = page.findChild<QLabel*>("nextLevelLabel");
    auto* total = page.findChild<QLabel*>("labelTotal");
    QVERIFY(line && total);
    const int unit = fixture.options->getDisplayUnit();
    const auto at = [unit](int tier) {
        QString amount = BitcoinUnits::formatWithUnit(unit, SkyTiers::threshold(tier), false, BitcoinUnits::SeparatorStyle::ALWAYS);
        amount.remove(amount.indexOf('.'), BitcoinUnits::decimals(unit) + 1);
        return QString("Next level at %1").arg(amount);
    };
    interfaces::WalletBalances balances;
    page.setBalance(balances); Settle();
    QVERIFY(line->isVisible());
    QCOMPARE(line->text(), at(1));
    QVERIFY(OnPage(page, line).top() >= OnPage(page, total).bottom());
    QVERIFY(std::abs(OnPage(page, line).right() - OnPage(page, total).right()) <= 2);
    // The whole line is on the card, not clipped to the amounts' column.
    QVERIFY2(QFontMetrics(line->font()).horizontalAdvance(line->text()) <= line->contentsRect().width(),
             qPrintable(QString("%1 px of text in %2 px").arg(QFontMetrics(line->font()).horizontalAdvance(line->text())).arg(line->contentsRect().width())));
    balances.balance = 1000000 * COIN;
    page.setBalance(balances); Settle();
    QCOMPARE(page.skyTier(), 4);
    QCOMPARE(line->text(), at(5));
    balances.balance = 25000000 * COIN;
    page.setBalance(balances); Settle();
    QCOMPARE(page.skyTier(), 10);
    QCOMPARE(line->text(), QString("Top level"));
    // It takes the column's width and never sets it.
    QCOMPARE(line->minimumWidth(), 0);
    QCOMPARE(line->sizePolicy().horizontalPolicy(), QSizePolicy::Ignored);
    page.setPrivacy(true); Settle();
    QVERIFY(line->isHidden());
    page.setPrivacy(false); Settle();
    QVERIFY(line->isVisible());
    fixture.wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    page.setBalance(balances); Settle();
    QVERIFY(line->isHidden());
    fixture.wallet->UnsetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    page.setBalance(balances); Settle();
    QVERIFY(line->isVisible());
    Theme("Light"); Settle();
    QVERIFY(line->isHidden());
    Theme("Abyss"); Settle();
    QVERIFY(line->isVisible());
    QCOMPARE(line->text(), QString("Top level"));
}

void AbyssTests::stakingWindow()
{
    // The window's staking tick feeds the card: the node's part from the
    // thread's flags, the wallet's part from its own switch and lock; the
    // card's button works the real switch.
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    const std::string name = fixture.wallet->GetName();
    const bool minter_before = fMinterRunning;
    const bool try_before = fTryToSync;
    const auto restore_flags = interfaces::MakeHandler([&] {
        if (IsWalletStaking(name)) ToggleWalletStaking(name);
        fMinterRunning = minter_before;
        fTryToSync = try_before;
        fixture.wallet->m_is_staking = NOT_STAKING;
    });
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850);
    window.show(); Settle();
    window.addWallet(fixture.model.get()); Settle();
    auto* page = window.findChild<OverviewPage*>();
    QVERIFY(page);
    auto* button = page->findChild<QPushButton*>("stakingButton");
    QVERIFY(button);
    // No thread in this process: the card says so and offers nothing.
    fMinterRunning = false;
    window.setStakingStatus(); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Disabled);
    QVERIFY(button->isHidden());
    // With the thread: the node's own gates are the card's. This node never
    // finishes the masternode sync, so the card waits -- whatever the
    // thread's fTryToSync flag happens to say, which is not a state.
    fMinterRunning = true;
    window.setStakingStatus(); Settle();
    QCOMPARE(page->stakingStatus().configured, gArgs.GetBoolArg("-staking", DEFAULT_STAKING));
    QVERIFY(!m_node.masternodeSync().isSynced());
    QVERIFY(page->stakingStatus().syncing);
    fTryToSync = !try_before;
    window.setStakingStatus(); Settle();
    QVERIFY(page->stakingStatus().syncing);
    fTryToSync = try_before;
    QVERIFY(!page->stakingStatus().locked);
    QVERIFY(!page->stakingStatus().keyless);
    QVERIFY(!page->stakingStatus().enabled); // Not in the staking registry yet.
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Off);
    // Register the wallet as the node does at start, then work the switch from the card.
    MultiwalletInitialize();
    QVERIFY(!IsWalletStaking(name));
    window.setStakingStatus(); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Off);
    QCOMPARE(button->text(), QString("Start staking"));
    button->click(); Settle();
    QVERIFY(IsWalletStaking(name));
    QVERIFY(page->stakingStatus().enabled); // At once, not at the next tick.
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Syncing);
    QCOMPARE(button->text(), QString("Pause staking"));
    // A stale "staking" word from the thread does not outrank the node's gates.
    fixture.wallet->m_is_staking = IS_STAKING;
    window.setStakingStatus(); Settle();
    QCOMPARE(page->stakingStatus().wallet_state, int(IS_STAKING));
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Syncing);
    fixture.wallet->m_is_staking = NOT_STAKING;
    button->click(); Settle();
    QVERIFY(!IsWalletStaking(name));
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Off);
    // The button asks for the position it offered, not for a flip: the switch
    // moved under it (the menu, the RPC) and the card has not ticked yet.
    QCOMPARE(button->text(), QString("Start staking"));
    QVERIFY(ToggleWalletStaking(name)); // on, behind the card's back
    button->click(); Settle();           // the stale "Start staking"
    QVERIFY(IsWalletStaking(name));      // still on
    QCOMPARE(button->text(), QString("Pause staking"));
    QVERIFY(!ToggleWalletStaking(name)); // off, behind the card's back
    button->click(); Settle();           // the stale "Pause staking"
    QVERIFY(!IsWalletStaking(name));     // still off
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Off);
    // The wallet's part follows the model's lock state: unencrypted is never locked.
    QCOMPARE(fixture.model->getEncryptionStatus(), WalletModel::Unencrypted);
    QVERIFY(!page->stakingStatus().locked);
    // A wallet without keys: the card says so, and the switch is not offered.
    fixture.wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    window.setStakingStatus(); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Unavailable);
    QVERIFY(button->isHidden());
    fixture.wallet->UnsetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    window.setStakingStatus(); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Off);
    // The lock's tooltips name staking in Abyss, whose card unlocks for it;
    // the classic themes keep their own text.
    auto* lock_icon = window.findChild<QLabel*>("labelWalletEncryptionIcon");
    auto* lock_button = window.findChild<QToolButton*>("walletLockButton");
    QVERIFY(lock_icon && lock_button);
    window.setEncryptionStatus(WalletModel::UnlockedForMixingOnly);
    QCOMPARE(lock_icon->toolTip(), QString("Wallet is <b>encrypted</b> and currently <b>unlocked</b> for mixing and staking only"));
    QCOMPARE(lock_button->toolTip(), QString("Wallet is <b>unlocked for mixing and staking only</b>. Click to lock it."));
    Theme("Light"); Settle();
    window.setEncryptionStatus(WalletModel::UnlockedForMixingOnly);
    QCOMPARE(lock_icon->toolTip(), QString("Wallet is <b>encrypted</b> and currently <b>unlocked</b> for mixing only"));
    QCOMPARE(lock_button->toolTip(), QString("Wallet is <b>unlocked for mixing only</b>. Click to lock it."));
    Theme("Abyss"); Settle();
    window.setEncryptionStatus(fixture.model->getEncryptionStatus());
    // The unlock dialog the card opens asks for the mixing-grade unlock under its own name.
    AskPassphraseDialog dialog(AskPassphraseDialog::UnlockStaking, nullptr);
    QCOMPARE(dialog.windowTitle(), QString("Unlock wallet for staking"));
    QVERIFY(dialog.findChild<QLabel*>("warningLabel")->text().contains("Sending stays locked"));
    QVERIFY(dialog.findChild<QLineEdit*>("passEdit2")->isHidden());
    QVERIFY(dialog.findChild<QLineEdit*>("passEdit3")->isHidden());
    // The tick reaches the card only while the window has its client model.
    // Without it -- before the node's start ends, and from the start of its
    // shutdown, which deletes the wallet models under the views -- the card
    // keeps its last state, and the tick asks the node nothing.
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Off);
    detach->disconnect(); // setClientModel(nullptr), as the shutdown does first
    fMinterRunning = false;
    window.setStakingStatus(); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Off);
}

void AbyssTests::stakingSyncGates()
{
    // The card waits exactly where the thread does (pos/minter.cpp): the
    // peers and the initial download count on the main network only; the
    // proof-of-work end and the masternode data on every network.
    OverviewPage::StakingGates ready;
    ready.masternodes_synced = true;
    ready.peers = STAKING_MIN_PEERS_MAINNET;
    ready.height = 1000;
    ready.last_pow_block = 500;
    for (const bool mainnet : {false, true}) {
        const auto with = [&](const std::function<void(OverviewPage::StakingGates&)>& change) {
            OverviewPage::StakingGates gates = ready;
            gates.mainnet = mainnet;
            change(gates);
            return OverviewPage::stakingWaitsForSync(gates);
        };
        const char* net = mainnet ? "main" : "other";
        QVERIFY2(!with([](auto&) {}), net);
        QVERIFY2(with([](auto& g) { g.initial_download = true; }) == mainnet, net);
        QVERIFY2(with([](auto& g) { g.peers = STAKING_MIN_PEERS_MAINNET - 1; }) == mainnet, net);
        QVERIFY2(with([](auto& g) { g.peers = 0; }) == mainnet, net);
        QVERIFY2(with([](auto& g) { g.initial_download = true; g.peers = 0; }) == mainnet, net);
        QVERIFY2(with([](auto& g) { g.masternodes_synced = false; }), net);
        QVERIFY2(with([](auto& g) { g.height = g.last_pow_block - 1; }), net);
        QVERIFY2(!with([](auto& g) { g.height = g.last_pow_block; }), net); // The thread stakes from that height on.
        // A tip stamped ahead of the node's clock holds the thread on every network.
        QVERIFY2(with([](auto& g) { g.adjusted_time = 1000000; g.tip_time = 1000001; }), net);
        QVERIFY2(!with([](auto& g) { g.adjusted_time = 1000000; g.tip_time = 1000000; }), net);
    }
    // The thread's last word "staking" does not outrank that wait: after a tip
    // from the future, the card waits as the thread does, then stakes again.
    OverviewPage page;
    OverviewPage::StakingStatus status;
    status.enabled = true;
    status.wallet_state = IS_STAKING;
    OverviewPage::StakingGates ahead = ready;
    ahead.adjusted_time = 1000000;
    ahead.tip_time = ahead.adjusted_time + 30;
    status.syncing = OverviewPage::stakingWaitsForSync(ahead);
    page.setStakingStatus(status);
    QCOMPARE(page.stakingState(), OverviewPage::StakingState::Syncing);
    ahead.adjusted_time = ahead.tip_time;
    status.syncing = OverviewPage::stakingWaitsForSync(ahead);
    page.setStakingStatus(status);
    QCOMPARE(page.stakingState(), OverviewPage::StakingState::Staking);
    // Nor does it outrank the thread's own flag, which a pass that failed
    // before the search (no block template) drops.
    status.searching = false;
    page.setStakingStatus(status);
    QCOMPARE(page.stakingState(), OverviewPage::StakingState::Ready);
    QVERIFY(!page.findChild<QLabel*>("stakingDetail")->text().contains("A reward comes"));
}

void AbyssTests::stakingUnlockFlow()
{
    // The banner's unlock on an encrypted wallet, through the real dialog:
    // cancelled, nothing moves; the wrong passphrase, nothing moves; the right
    // one unlocks for mixing and staking only -- the wallet stakes and still
    // refuses to send -- and switches staking on; done again with the switch
    // already on, the switch stays on and nothing is said. The card offers no
    // second button for it.
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node, /*dummy_database=*/true);
    const std::string name = fixture.wallet->GetName();
    const SecureString passphrase("staking-trial");
    QVERIFY(fixture.wallet->EncryptWallet(passphrase));
    QCOMPARE(fixture.model->getEncryptionStatus(), WalletModel::Locked);
    MultiwalletInitialize();
    const bool minter_before = fMinterRunning;
    const auto cleanup = interfaces::MakeHandler([&] {
        if (IsWalletStaking(name)) ToggleWalletStaking(name);
        fMinterRunning = minter_before;
    });
    WalletView view(nullptr);
    view.setClientModel(fixture.client.get());
    view.setWalletModel(fixture.model.get());
    view.resize(1280, 850);
    view.show(); Settle();
    auto* page = view.findChild<OverviewPage*>();
    QVERIFY(page);
    OverviewPage::StakingStatus node; // the thread up, the chain in
    view.setStakingStatus(node); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Locked);
    auto* button = page->findChild<QPushButton*>("stakingButton");
    auto* banner = page->findChild<QFrame*>("stakingBanner");
    auto* banner_button = page->findChild<QPushButton*>("stakingBannerUnlock");
    QVERIFY(button && banner && banner_button);
    QVERIFY(banner->isVisible());
    QVERIFY(button->isHidden());
    QSignalSpy messages(&view, &WalletView::message);
    // The dialog the button opens is answered from the event loop, as the
    // send confirmation is in wallettests. Two timers, because a timer does
    // not fire again while its own slot is still running: the answerer types
    // and accepts (a failed unlock then runs its message box from inside that
    // slot), the closer closes any message box and then cancels a dialog that
    // was answered and stayed open for another try. No step can leave a modal
    // window waiting, so a failure is a failed check, never a hang.
    std::optional<std::pair<QString, bool>> pending;
    bool answered = false;
    QStringList seen_titles, seen_boxes;
    QTimer answerer;
    answerer.setInterval(20);
    QObject::connect(&answerer, &QTimer::timeout, [&] {
        if (!pending) return;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<AskPassphraseDialog*>(widget);
            if (dialog == nullptr || !dialog->isVisible()) continue;
            const auto [text, accept] = *pending;
            pending.reset();
            answered = true;
            seen_titles << dialog->windowTitle();
            dialog->findChild<QLineEdit*>("passEdit1")->setText(text);
            if (accept) dialog->accept(); else dialog->reject();
            return;
        }
    });
    QTimer closer;
    closer.setInterval(20);
    QObject::connect(&closer, &QTimer::timeout, [&] {
        bool box_open = false;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(widget);
            if (box == nullptr || !box->isVisible()) continue;
            seen_boxes << box->windowTitle();
            box->close();
            box_open = true;
        }
        if (box_open || !answered) return;
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<AskPassphraseDialog*>(widget);
            if (dialog != nullptr && dialog->isVisible()) dialog->reject();
        }
    });
    answerer.start();
    closer.start();
    const auto step = [&](const QString& text, bool accept, QPushButton* via) {
        pending = std::make_pair(text, accept);
        answered = false;
        seen_titles.clear();
        seen_boxes.clear();
        via->click(); Settle();
        QVERIFY(!pending); // The dialog opened and was answered.
    };
    const QString right = QString::fromStdString(std::string(passphrase.begin(), passphrase.end()));
    // Cancelled.
    step(QString(), false, banner_button);
    QCOMPARE(seen_titles, QStringList{"Unlock wallet for staking"});
    QVERIFY(seen_boxes.isEmpty());
    QCOMPARE(fixture.model->getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(!IsWalletStaking(name));
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Locked);
    QCOMPARE(messages.count(), 0);
    // The wrong passphrase: the dialog's own message, the dialog cancelled
    // after it, and still locked.
    step("not-the-passphrase", true, banner_button);
    seen_boxes.removeDuplicates();
    QCOMPARE(seen_boxes, QStringList{"Wallet unlock failed"});
    QCOMPARE(fixture.model->getEncryptionStatus(), WalletModel::Locked);
    QVERIFY(!IsWalletStaking(name));
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Locked);
    QCOMPARE(messages.count(), 0);
    // The right one: unlocked for mixing and staking, and staking is on.
    step(right, true, banner_button);
    QVERIFY2(seen_boxes.isEmpty(), qPrintable(seen_boxes.join(", ")));
    QCOMPARE(fixture.model->getEncryptionStatus(), WalletModel::UnlockedForMixingOnly);
    QVERIFY(fixture.model->wallet().isLocked());       // sending still refused
    QVERIFY(!fixture.model->wallet().isLocked(true));  // the keys stake
    QVERIFY(IsWalletStaking(name));
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Ready);
    QVERIFY(banner->isHidden());
    QVERIFY(!button->isHidden()); // the switch again
    QCOMPARE(messages.count(), 0);
    // Locked again with the switch on: the banner's button unlocks, the switch stays on, nothing is said.
    QVERIFY(fixture.model->setWalletLocked(true));
    view.setStakingStatus(node); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Locked);
    QVERIFY(banner->isVisible());
    QVERIFY(button->isHidden());
    step(right, true, banner_button);
    QVERIFY2(seen_boxes.isEmpty(), qPrintable(seen_boxes.join(", ")));
    QCOMPARE(fixture.model->getEncryptionStatus(), WalletModel::UnlockedForMixingOnly);
    QVERIFY(IsWalletStaking(name));
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Ready);
    QCOMPARE(messages.count(), 0);
    QVERIFY(fixture.model->setWalletLocked(true));
    // A quit request while the dialog is open: BitcoinApplication turns it
    // into the shutdown in whatever loop runs, the dialog's too, and the
    // shutdown unsets the client model and then deletes the wallet model. The
    // dialog goes with its model, and the unlock reads nothing of it after.
    answerer.stop();
    closer.stop();
    view.setStakingStatus(node); Settle();
    QCOMPARE(page->stakingState(), OverviewPage::StakingState::Locked);
    bool dialog_seen = false, shut_down = false, checked = false, outlived = false;
    // Closes a dialog that outlived its model, so that is a failed check, not a hang.
    QTimer guard;
    guard.setSingleShot(true);
    guard.setInterval(300);
    QObject::connect(&guard, &QTimer::timeout, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<AskPassphraseDialog*>(widget);
            if (dialog == nullptr || !dialog->isVisible()) continue;
            outlived = true;
            dialog->reject();
        }
        checked = true;
    });
    QTimer quit;
    quit.setInterval(20);
    QObject::connect(&quit, &QTimer::timeout, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<AskPassphraseDialog*>(widget);
            if (dialog == nullptr || !dialog->isVisible()) continue;
            quit.stop();
            dialog_seen = true;
            view.setClientModel(nullptr); // the shutdown's order: the client model first,
            fixture.model.reset();        // then the wallet controller deletes its models
            shut_down = true;
            guard.start();
            return;
        }
    });
    quit.start();
    banner_button->click();
    QTRY_VERIFY_WITH_TIMEOUT(checked, 5000);
    QVERIFY(dialog_seen);
    QVERIFY(shut_down);
    QVERIFY(!outlived);
    QCOMPARE(messages.count(), 0);
}

void AbyssTests::stakingTipTime()
{
    // The card's clock gate reads the active tip's time from every block
    // notice, also from one the GUI's throttling drops: in the initial
    // download a notice within 250 ms of the last one shown never reaches the
    // window, and nothing repeats it. Both ways: a later tip stamped earlier
    // after one ahead of the clock, and a tip ahead of the clock dropped.
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QVERIFY(style);
    WalletFixture fixture(m_node);
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    int shown = 0;
    QObject::connect(fixture.client.get(), &ClientModel::numBlocksChanged, [&] { ++shown; });
    const int height = fixture.client->getNumBlocks();
    // A new active tip as validation announces it while still in the initial download.
    const auto notice = [&](int64_t time, int at) {
        uint256 hash{};
        std::memcpy(hash.begin(), &time, sizeof(time));
        CBlockIndex index;
        index.nHeight = at;
        index.nTime = uint32_t(time);
        index.nChainTx = 1;
        index.phashBlock = &hash;
        uiInterface.NotifyBlockTip(SynchronizationState::INIT_DOWNLOAD, &index);
    };
    const int64_t now = GetAdjustedTime();
    QTest::qWait(300); // past the throttle window of any notice before
    shown = 0;
    notice(now + 120, height + 1);
    notice(now - 600, height + 2);
    Settle();
    QCOMPARE(shown, 1); // the second one was dropped
    QCOMPARE(window.stakingTipTime(), now - 600);
    QTest::qWait(300);
    shown = 0;
    notice(now - 600, height + 3);
    notice(now + 120, height + 4);
    Settle();
    QCOMPARE(shown, 1);
    QCOMPARE(window.stakingTipTime(), now + 120);
    // The first fills, before any notice: one that lands between the node's
    // answer and the store wins, for the height and the tip time alike. A
    // plain store would put the older value over it until the next block.
    QTest::qWait(300);
    fixture.client->m_cached_num_blocks = -1;
    fixture.client->m_cached_tip_time = -1;
    fixture.client->m_test_hook_fill = [&] { notice(now + 30, height + 7); };
    QCOMPARE(fixture.client->getNumBlocks(), height + 7);
    fixture.client->m_cached_tip_time = -1; // the notice set it too: fill it anew
    fixture.client->m_test_hook_fill = [&] { notice(now - 30, height + 8); };
    QCOMPARE(window.stakingTipTime(), now - 30);
    fixture.client->m_test_hook_fill = nullptr;
    Settle();
}

void AbyssTests::overviewColumnsAligned()
{
    // The two columns end on one line at every size: the left column's last
    // card shown stretches down to the right card's bottom, the CoinJoin row
    // stands under both, the stretched card keeps its content at the top, and
    // no card is squeezed below its natural height. Where the page is short,
    // its minimum grows instead, which the window follows (up to the screen);
    // the page is measured at its minimum too, as the window would hold it.
    if (!GUIUtil::fontsLoaded()) QSKIP("Card heights require a font-capable platform; run minimal:enable_fonts");
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setClientModel(fixture.client.get());
    page.setWalletModel(fixture.model.get());
    page.resize(1280, 850);
    page.show(); Settle();
    page.coinJoinStatus(true); Settle();
    auto* balances = page.findChild<QFrame*>("frame");
    auto* staking = page.findChild<QFrame*>("frameStaking");
    auto* coinjoin = page.findChild<QFrame*>("frameCoinJoin");
    auto* right = page.findChild<QFrame*>("frame_2");
    auto* row = page.findChild<QWidget*>("coinJoinRow");
    auto* banner = page.findChild<QFrame*>("stakingBanner");
    auto* staking_button = page.findChild<QPushButton*>("stakingButton");
    auto* staking_word = page.findChild<QLabel*>("stakingState");
    auto* coinjoin_button = page.findChild<QPushButton*>("toggleCoinJoin");
    auto* show = page.findChild<QPushButton*>("coinJoinRowShow");
    QVERIFY(balances && staking && coinjoin && right && row && banner && staking_button && staking_word && coinjoin_button && show);
    QVERIFY(coinjoin->isHidden());
    QVERIFY(row->isVisible());
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    QStringList found;
    std::map<QString, int> minimum_height;
    const auto check = [&](const QString& state) {
        QWidget* last = coinjoin->isHidden() ? static_cast<QWidget*>(staking) : static_cast<QWidget*>(coinjoin);
        // The card's lowest content: a locked wallet's staking card has only its word.
        QWidget* content = last == staking ? (staking_button->isHidden() ? static_cast<QWidget*>(staking_word) : staking_button)
                                           : static_cast<QWidget*>(coinjoin_button);
        std::optional<int> content_offset;
        minimum_height[state] = page.minimumSizeHint().height();
        // No card of the column may be shrunk by its layout below its size hint
        // (no ShrinkFlag). On this platform the balances card's minimum equals
        // its size hint anyway; natively (the #378 trial at 175%) it was smaller,
        // and the window's minimum let the card go down to it.
        for (QWidget* card : {static_cast<QWidget*>(balances), static_cast<QWidget*>(staking), static_cast<QWidget*>(coinjoin)}) {
            if (card->isHidden()) continue;
            qInfo().noquote() << "OVERVIEW_COLUMNS_CARD" << state << card->objectName() << "hint" << card->sizeHint().height()
                              << "minimum" << card->minimumSizeHint().height() << "policy" << int(card->sizePolicy().verticalPolicy());
            if (card->sizePolicy().verticalPolicy() & QSizePolicy::ShrinkFlag) {
                found << QString("%1: %2 may be shrunk below its natural height (vertical policy %3)")
                             .arg(state, card->objectName()).arg(int(card->sizePolicy().verticalPolicy()));
            }
        }
        for (int width : {0, 1280, 1920, 2560}) {
            for (int height : {0, 850, 1100}) {
                page.resize(std::max(width, page.minimumWidth()), std::max(height, page.minimumSizeHint().height()));
                Settle();
                const QString at = QString("%1 at %2x%3").arg(state).arg(page.width()).arg(page.height());
                const int left_bottom = OnPage(page, last).bottom();
                const int right_bottom = OnPage(page, right).bottom();
                if (qAbs(left_bottom - right_bottom) > 1) {
                    found << QString("%1: the left column ends at %2, the right card at %3").arg(at).arg(left_bottom).arg(right_bottom);
                }
                // The last card may only grow; every other one keeps exactly its natural height,
                // which for a card with wrapping text (the staking card's line) is its height at
                // its width, not the size hint's guess at a narrower one.
                for (QWidget* card : {static_cast<QWidget*>(balances), static_cast<QWidget*>(staking), static_cast<QWidget*>(coinjoin)}) {
                    if (card->isHidden()) continue;
                    const int natural = card->hasHeightForWidth() ? card->heightForWidth(card->width()) : card->sizeHint().height();
                    if (card->height() < natural) {
                        found << QString("%1: %2 squeezed to %3 of %4").arg(at, card->objectName()).arg(card->height()).arg(natural);
                    } else if (card != last && card->height() != natural) {
                        found << QString("%1: %2 stretched to %3 of %4, not the last card").arg(at, card->objectName()).arg(card->height()).arg(natural);
                    }
                }
                if (row->isVisible() && OnPage(page, row).top() <= std::max(left_bottom, right_bottom)) {
                    found << QString("%1: the CoinJoin row at %2 is not under the columns (%3)").arg(at).arg(OnPage(page, row).top()).arg(right_bottom);
                }
                const int offset = content->mapTo(last, QPoint()).y();
                if (!content_offset) content_offset = offset;
                if (offset != *content_offset) {
                    found << QString("%1: %2 moved inside the stretched card, %3 not %4").arg(at, content->objectName()).arg(offset).arg(*content_offset);
                }
                if (!output.isEmpty() && (width == 1280 || width == 2560) && height != 1100) {
                    QVERIFY(QDir().mkpath(output));
                    QVERIFY(PageImage(page).save(QDir(output).filePath(QString("Abyss-columns-%1-%2x%3.png").arg(state).arg(width).arg(height ? height : 0))));
                }
            }
        }
    };
    page.setStakingStatus(StakingCase("off")); Settle();
    QVERIFY(banner->isHidden());
    check("staking-unlocked");
    page.setStakingStatus(StakingCase("locked")); Settle();
    QVERIFY(banner->isVisible());
    check("staking-locked");
    // The banner's room comes out of the staking card's line and button, not
    // out of the balances card: the locked page asks for no more height than
    // the unlocked one (overviewLockedFits has the window's side of it).
    if (minimum_height["staking-locked"] > minimum_height["staking-unlocked"]) {
        found << QString("the locked page's minimum %1 is above the unlocked one's %2")
                     .arg(minimum_height["staking-locked"]).arg(minimum_height["staking-unlocked"]);
    }
    show->click(); Settle();
    QVERIFY(coinjoin->isVisible());
    QVERIFY(row->isHidden());
    check("coinjoin-locked");
    page.setStakingStatus(StakingCase("off")); Settle();
    check("coinjoin-unlocked");
    for (const QString& line : found) qInfo().noquote() << "OVERVIEW_COLUMNS" << line;
    QCOMPARE(found.join("\n"), QString());
}

void AbyssTests::overviewLockedFits()
{
    // The window holds the page under the screen (applyModernWindowMinimum),
    // and a page given less than its minimum is squeezed, the biggest card
    // first: at 175% on a 2560x1600 display the locked page, banner and all,
    // asked for some 70 pixels more than the screen had, and the balances
    // card gave them. The banner's room now comes out of the staking card,
    // which keeps to its word on a locked wallet: held at the unlocked page's
    // minimum, as that window held it, the locked page squeezes no card. The
    // total's row follows its font both ways instead of keeping a size the
    // total once had. Also run at QT_SCALE_FACTOR=1.75; the family is the
    // one the native trial showed.
    if (!GUIUtil::fontsLoaded()) QSKIP("Card heights require a font-capable platform; run minimal:enable_fonts");
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    GUIUtil::setFontFamily(GUIUtil::FontFamily::Roboto);
    GUIUtil::setFontScale(0);
    // A child page, as in the wallet's stack: its own minimum height stays 0,
    // so the host can give it less than its layout asks for, as the window can.
    QWidget host;
    auto* page = new OverviewPage(&host);
    page->setClientModel(fixture.client.get());
    page->setWalletModel(fixture.model.get());
    const auto hold = [&](int width, int height) {
        width = std::max(width, page->minimumWidth());
        host.resize(width, height);
        page->setGeometry(0, 0, width, height);
        Settle();
    };
    hold(1280, 850);
    host.show(); Settle();
    page->coinJoinStatus(true); Settle();
    auto* balances = page->findChild<QFrame*>("frame");
    auto* staking = page->findChild<QFrame*>("frameStaking");
    auto* banner = page->findChild<QFrame*>("stakingBanner");
    auto* unlock = page->findChild<QPushButton*>("stakingBannerUnlock");
    auto* word = page->findChild<QLabel*>("stakingState");
    auto* detail = page->findChild<QLabel*>("stakingDetail");
    auto* button = page->findChild<QPushButton*>("stakingButton");
    auto* total = page->findChild<QLabel*>("labelTotal");
    auto* next = page->findChild<QLabel*>("nextLevelLabel");
    QVERIFY(balances && staking && banner && unlock && word && detail && button && total && next);
    QVERIFY(page->findChild<QFrame*>("frameCoinJoin")->isHidden());
    interfaces::WalletBalances zero{};
    interfaces::WalletBalances large{};
    large.balance = 50'820'975 * COIN + 99 * COIN / 100;
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    QStringList found;
    struct Case { const char* name; const interfaces::WalletBalances* balances; bool privacy; };
    const std::vector<Case> cases{{"zero", &zero, false}, {"large", &large, false}, {"discreet", &large, true}};
    for (const Case& which : cases) {
        page->setPrivacy(which.privacy);
        page->setBalance(*which.balances); Settle();
        for (int width : {0, 1280, 2560}) {
            page->setStakingStatus(StakingCase("off")); Settle();
            hold(width, page->minimumSizeHint().height());
            const int open_minimum = page->minimumSizeHint().height();
            page->setStakingStatus(StakingCase("locked")); Settle();
            const QString at = QString("%1 at width %2").arg(which.name).arg(page->width());
            if (banner->isHidden() || unlock->isHidden() || word->isHidden() || !detail->isHidden() || !button->isHidden()) {
                found << QString("%1: locked, the card is not the word alone under the banner (banner %2, unlock %3, word %4, line %5, button %6)")
                             .arg(at).arg(!banner->isHidden()).arg(!unlock->isHidden()).arg(!word->isHidden()).arg(!detail->isHidden()).arg(!button->isHidden());
            }
            const int locked_minimum = page->minimumSizeHint().height();
            qInfo().noquote() << "OVERVIEW_LOCKED_FITS" << at << "unlocked minimum" << open_minimum << "locked minimum" << locked_minimum
                              << "banner" << banner->height() << "DPR" << page->devicePixelRatioF();
            if (locked_minimum > open_minimum) {
                found << QString("%1: the locked page asks for %2, the unlocked one for %3").arg(at).arg(locked_minimum).arg(open_minimum);
            }
            // Held where the unlocked page stood, as the window under the screen holds it.
            hold(page->width(), open_minimum);
            for (QWidget* card : {static_cast<QWidget*>(balances), static_cast<QWidget*>(staking)}) {
                const int natural = card->hasHeightForWidth() ? card->heightForWidth(card->width()) : card->sizeHint().height();
                if (card->height() < natural) {
                    found << QString("%1: %2 squeezed to %3 of %4").arg(at, card->objectName()).arg(card->height()).arg(natural);
                }
            }
            // The whole amount, and the line under it, inside the card and apart.
            const QRect card_inside = balances->contentsRect().translated(balances->mapTo(page, QPoint()));
            for (QLabel* label : {total, next}) {
                if (label->isHidden()) continue;
                if (OnPage(*page, label).bottom() > card_inside.bottom()) {
                    found << QString("%1: %2 ends at %3, under the card's inside (%4)").arg(at, label->objectName())
                                 .arg(OnPage(*page, label).bottom()).arg(card_inside.bottom());
                }
            }
            if (!next->isHidden() && OnPage(*page, next).top() <= OnPage(*page, total).bottom()) {
                found << QString("%1: the next-level line at %2 is not under the total (%3)").arg(at).arg(OnPage(*page, next).top()).arg(OnPage(*page, total).bottom());
            }
            // A line's box may round up from the font's height by one pixel either way.
            if (total->contentsRect().height() + 1 < total->fontMetrics().height()) {
                found << QString("%1: the total has %2 of %3 pixels").arg(at).arg(total->contentsRect().height()).arg(total->fontMetrics().height());
            }
            if (!output.isEmpty() && width == 1280) {
                QVERIFY(QDir().mkpath(output));
                QVERIFY(PageImage(*page).save(QDir(output).filePath(QString("Abyss-locked-fits-%1.png").arg(which.name))));
            }
        }
    }
    // The total's row: under a smaller font it comes down, and it goes back up with the hero size.
    page->setPrivacy(false);
    page->setStakingStatus(StakingCase("off"));
    page->setBalance(zero); Settle();
    hold(1280, 850);
    const qreal hero_font = total->font().pointSizeF();
    const int hero_row = total->minimumHeight();
    const int hero_card = balances->sizeHint().height();
    page->setBalance(large); Settle();
    const QString row = QString("hero %1 pt, row %2, card %3; large %4 pt, row %5, card %6")
                            .arg(hero_font).arg(hero_row).arg(hero_card)
                            .arg(total->font().pointSizeF()).arg(total->minimumHeight()).arg(balances->sizeHint().height());
    qInfo().noquote() << "OVERVIEW_TOTAL_ROW" << row;
    QVERIFY2(total->font().pointSizeF() < hero_font, qPrintable(row));
    if (total->minimumHeight() >= hero_row) found << "the total's row kept its hero height: " + row;
    if (balances->sizeHint().height() >= hero_card) found << "the balances card kept its hero height: " + row;
    // Its contents, inside the margins (general.css gives the total a negative bottom margin), hold the text.
    const QMargins margins = total->contentsMargins();
    if (total->minimumHeight() - margins.top() - margins.bottom() + 1 < total->fontMetrics().height()) {
        found << QString("the total's row is lower than its text (margins %1, %2): ").arg(margins.top()).arg(margins.bottom()) + row;
    }
    page->setBalance(zero); Settle();
    if (total->minimumHeight() != hero_row) found << QString("back at zero, the total's row is %1, not %2").arg(total->minimumHeight()).arg(hero_row);
    // The recent transactions list does not hold the row open in Abyss (the
    // locked page above relies on it); a classic theme keeps its room for
    // every row, and Abyss takes it back off after a switch. (The empty
    // list's message may raise the Abyss figure to what it needs.)
    auto* list = page->findChild<QListView*>("listTransactions");
    QVERIFY(list);
    const int abyss_list = list->minimumHeight();
    Theme("Light"); Settle();
    const int classic_list = list->minimumHeight();
    Theme("Abyss"); Settle();
    qInfo().noquote() << "OVERVIEW_LOCKED_FITS list minimum: Abyss" << abyss_list << "Light" << classic_list << "Abyss again" << list->minimumHeight();
    if (abyss_list >= classic_list || list->minimumHeight() >= classic_list) {
        found << QString("the list's minimum: Abyss %1, Light %2, Abyss again %3").arg(abyss_list).arg(classic_list).arg(list->minimumHeight());
    }
    for (const QString& line : found) qInfo().noquote() << "OVERVIEW_LOCKED_FITS" << line;
    QCOMPARE(found.join("\n"), QString());
}

namespace {
//! The wallet's receiving addresses, as its address list has them.
QStringList ReceivingAddresses(AddressTableModel* table)
{
    QStringList found;
    for (int row = 0; row < table->rowCount({}); ++row) {
        const QModelIndex address = table->index(row, AddressTableModel::Address, QModelIndex());
        if (address.data(AddressTableModel::TypeRole).toString() == AddressTableModel::Receive) {
            found << address.data(Qt::EditRole).toString();
        }
    }
    return found;
}
//! A payment of one coin (or `amount`) to `address` from nowhere in particular:
//! the wallet's transaction list names the address from then on. Unconfirmed,
//! so it does not count towards the balance yet.
bool PayTo(CWallet& wallet, const QString& address, uint32_t n, CAmount amount = COIN)
{
    CMutableTransaction payment;
    payment.vin.emplace_back(COutPoint(uint256S("0100000000000000000000000000000000000000000000000000000000000000"), n));
    payment.vout.emplace_back(amount, GetScriptForDestination(DecodeDestination(address.toStdString())));
    return wallet.AddToWallet(MakeTransactionRef(std::move(payment)), CWalletTx::Confirmation{}) != nullptr;
}
//! The "digits" setting for one test, put back afterwards.
struct Digits {
    QVariant before{QSettings().value("digits")};
    explicit Digits(int digits) { QSettings().setValue("digits", QString::number(digits)); }
    ~Digits()
    {
        if (before.isValid()) QSettings().setValue("digits", before); else QSettings().remove("digits");
    }
};
} // namespace

void AbyssTests::abyssUnitsFormat_data()
{
    QTest::addColumn<int>("unit");
    QTest::addColumn<qint64>("amount");
    QTest::addColumn<bool>("plus");
    QTest::addColumn<bool>("floor");
    QTest::addColumn<int>("digits");
    QTest::addColumn<QString>("number");
    const int D = BitcoinUnits::DASH, M = BitcoinUnits::mDASH, U = BitcoinUnits::uDASH, S = BitcoinUnits::duffs;
    const auto row = [](const char* name, int unit, qint64 amount, bool plus, bool floor, int digits, const char* number) {
        QTest::newRow(name) << unit << amount << plus << floor << digits << Thin(number);
    };
    row("zero", D, 0, false, true, 2, "0");
    row("half", D, COIN / 2, false, true, 2, "0.5");
    row("one", D, COIN, false, true, 2, "1");
    row("twelve point three", D, 1230000000, false, true, 2, "12.3");
    row("one and a duff, floored", D, COIN + 1, false, true, 2, "1");
    row("below the floor, exact", D, 300000, false, true, 2, "0.003");
    row("935 million", D, 935000000 * COIN, false, true, 2, "935 000 000");
    row("plus sign", D, COIN / 2, true, true, 2, "+0.5");
    row("negative", D, -COIN / 2, true, true, 2, "-0.5");
    row("negative below the floor", D, -300000, false, true, 2, "-0.003");
    row("cut, not rounded", D, 129900000, false, true, 2, "1.29");
    row("digits 8", D, COIN + 1, false, true, 8, "1.00000001");
    row("digits 8, half", D, COIN / 2, false, true, 8, "0.5");
    row("digits 0", D, 3 * COIN / 2, false, true, 0, "1");
    row("digits 0, below", D, COIN / 2, false, true, 0, "0.5");
    row("no floor", D, COIN + 1, false, false, 2, "1.00000001");
    row("no floor, whole", D, -COIN, false, false, 2, "-1");
    row("no floor, zero", D, 0, false, false, 2, "0");
    row("mDFCN", M, COIN, false, true, 2, "1 000");
    row("mDFCN, half", M, 150000, false, true, 2, "1.5");
    row("mDFCN, below the floor", M, 123, false, true, 2, "0.00123");
    row("uDFCN", U, 150, false, true, 2, "1.5");
    row("uDFCN, whole", U, 100, false, true, 2, "1");
    row("uDFCN, smallest", U, 1, false, true, 2, "0.01");
    row("duffs", S, 100, false, true, 2, "100");
    row("duffs, thousands", S, 1000, false, true, 2, "1 000");
    row("duffs, zero", S, 0, false, true, 2, "0");
}

void AbyssTests::abyssUnitsFormat()
{
    // Abyss's amounts (U3-C): no trailing zeros, no decimal point left alone,
    // cut to the "digits" setting where asked as BitcoinUnits::floorWithUnit
    // cuts, but a non-zero amount never cut to zero; the tooltip's text is
    // BitcoinUnits' own, every decimal.
    QFETCH(int, unit);
    QFETCH(qint64, amount);
    QFETCH(bool, plus);
    QFETCH(bool, floor);
    QFETCH(int, digits);
    QFETCH(QString, number);
    Digits setting(digits);
    QCOMPARE(AbyssUnits::number(unit, amount, plus, floor), number);
    QCOMPARE(AbyssUnits::format(unit, amount, plus, floor), number + QLatin1Char(' ') + BitcoinUnits::name(unit));
    QCOMPARE(AbyssUnits::exact(unit, amount, plus), BitcoinUnits::formatWithUnit(unit, amount, plus, BitcoinUnits::SeparatorStyle::ALWAYS));
    if (qstrcmp(QTest::currentDataTag(), "one and a duff, floored") == 0) {
        QCOMPARE(AbyssUnits::exact(unit, amount), QString("1.00000001 ") + BitcoinUnits::name(unit));
    }
}

void AbyssTests::overviewPlainAmounts()
{
    // The Overview's nine amounts in Abyss: no trailing zeros, cut to the
    // "digits" setting as before but never to zero; each tooltip has the exact
    // amount above the form's explanation (labelAnonymized has none). Discreet
    // mode masks as before and leaves only the explanation. A classic theme
    // shows exactly what it showed, text and tooltip, and a switch back to
    // Abyss formats both again.
    RestoreAppearance restore;
    Digits digits(2);
    Theme("Light");
    WalletFixture fixture(m_node);
    OverviewPage page;
    page.setWalletModel(fixture.model.get());
    page.resize(1280, 850);
    page.show(); Settle();
    const int unit = fixture.options->getDisplayUnit();
    interfaces::WalletBalances balances;
    balances.balance = 935000000 * COIN;
    balances.unconfirmed_balance = COIN / 2;
    balances.immature_balance = COIN + 1;
    balances.anonymized_balance = 300000;
    balances.watch_only_balance = 2 * COIN;
    balances.unconfirmed_watch_only_balance = 0;
    balances.immature_watch_only_balance = COIN / 4;
    struct Amount { const char* label; CAmount amount; const char* abyss; };
    const std::vector<Amount> amounts{
        {"labelBalance", balances.balance, "935 000 000"},
        {"labelUnconfirmed", balances.unconfirmed_balance, "0.5"},
        {"labelImmature", balances.immature_balance, "1"},
        {"labelAnonymized", balances.anonymized_balance, "0.003"},
        {"labelTotal", balances.balance + balances.unconfirmed_balance + balances.immature_balance, "935 000 001.5"},
        {"labelWatchAvailable", balances.watch_only_balance, "2"},
        {"labelWatchPending", balances.unconfirmed_watch_only_balance, "0"},
        {"labelWatchImmature", balances.immature_watch_only_balance, "0.25"},
        {"labelWatchTotal", balances.watch_only_balance + balances.unconfirmed_watch_only_balance + balances.immature_watch_only_balance, "2.25"}};
    page.setBalance(balances); Settle();

    // The classic theme first: today's text, and the form's tooltip, which
    // the checks below compare against.
    std::map<QString, QString> form;
    const auto classic = [&](const QString& what) {
        for (const Amount& a : amounts) {
            auto* label = page.findChild<QLabel*>(a.label);
            QVERIFY2(label, a.label);
            QCOMPARE(label->text(), BitcoinUnits::floorHtmlWithPrivacy(unit, a.amount, BitcoinUnits::SeparatorStyle::ALWAYS, false));
            if (form.count(a.label)) QVERIFY2(label->toolTip() == form[a.label], qPrintable(what + ": " + a.label + " " + label->toolTip()));
            form[a.label] = label->toolTip();
        }
    };
    classic("Light");
    QCOMPARE(form["labelBalance"], QString("Your current spendable balance"));
    QVERIFY(form["labelAnonymized"].isEmpty());
    const auto abyss = [&](const QString& what) {
        for (const Amount& a : amounts) {
            auto* label = page.findChild<QLabel*>(a.label);
            QVERIFY2(label->text() == Thin(a.abyss) + QLatin1Char(' ') + BitcoinUnits::name(unit),
                     qPrintable(what + ": " + a.label + " reads " + label->text()));
            const QString exact = BitcoinUnits::formatWithUnit(unit, a.amount, false, BitcoinUnits::SeparatorStyle::ALWAYS);
            const QString tip = form[a.label].isEmpty() ? exact : exact + QLatin1Char('\n') + form[a.label];
            QVERIFY2(label->toolTip() == tip, qPrintable(what + ": " + a.label + " tooltip " + label->toolTip()));
        }
    };
    Theme("Abyss"); Settle();
    abyss("Abyss");
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) QVERIFY(QDir().mkpath(output) && page.grab().save(QDir(output).filePath("Abyss-overview-plain.png")));

    // Discreet mode: the mask as before, and the explanation alone.
    page.setPrivacy(true); Settle();
    for (const Amount& a : amounts) {
        auto* label = page.findChild<QLabel*>(a.label);
        QCOMPARE(label->text(), BitcoinUnits::floorWithPrivacy(unit, a.amount, BitcoinUnits::SeparatorStyle::ALWAYS, true));
        QCOMPARE(label->toolTip(), form[a.label]);
    }
    page.setPrivacy(false); Settle();
    abyss("discreet mode off");

    Theme("Light"); Settle();
    classic("back in Light");
    Theme("Abyss"); Settle();
    abyss("back in Abyss");
}

void AbyssTests::overviewRecentTooltip()
{
    // A recent-transactions row shows its amount cut to the "digits" setting.
    // In Abyss its tooltip is the model's own text with one more line, the
    // exact amount. Discreet mode hides the list. A classic theme shows the
    // model's tooltip alone.
    RestoreAppearance restore;
    Digits digits(2);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    const QString own = fixture.model->getAddressTableModel()->addRow(AddressTableModel::Receive, "", "");
    QVERIFY(!own.isEmpty());
    QVERIFY(PayTo(*fixture.wallet, own, 0, COIN + 1));
    OverviewPage page;
    // The recent list is set up from the client model's CoinJoin state.
    page.setClientModel(fixture.client.get());
    page.setWalletModel(fixture.model.get());
    page.resize(1280, 850);
    page.show(); Settle();
    auto* list = page.findChild<QListView*>("listTransactions");
    QVERIFY(list && list->isVisible());
    QVERIFY(list->model() && list->model()->rowCount() == 1);
    const QModelIndex row = list->model()->index(0, list->modelColumn());
    QCOMPARE(row.data(TransactionTableModel::AmountRole).toLongLong(), COIN + 1);
    const QString name = BitcoinUnits::name(fixture.options->getDisplayUnit());
    // What the row's tooltip says, read as soon as the view has handled the
    // help event: on minimal no window is active and there is no pointer, so
    // a tooltip may close again on the next pass of the event loop.
    const auto tip = [&] {
        QToolTip::hideText(); Settle();
        const QPoint at = list->visualRect(row).center();
        QHelpEvent help(QEvent::ToolTip, at, list->viewport()->mapToGlobal(at));
        QCoreApplication::sendEvent(list->viewport(), &help);
        const QString shown = QToolTip::text();
        QToolTip::hideText();
        return shown;
    };
    const auto own_tip = [&] { return row.data(Qt::ToolTipRole).toString(); };
    const auto says = [&](const QString& expected, const char* what) {
        const QString shown = tip();
        QVERIFY2(shown == expected, qPrintable(QString("%1: shown \"%2\", expected \"%3\"").arg(what, shown, expected)));
    };
    QVERIFY(!own_tip().isEmpty() && !own_tip().contains("Amount"));
    says(own_tip() + QString("\nAmount: +1.00000001 ") + name, "Abyss");
    if (QTest::currentTestFailed()) return;

    page.setPrivacy(true); Settle();
    QVERIFY(!list->isVisible());
    page.setPrivacy(false); Settle();
    QVERIFY(list->isVisible());

    Theme("Light"); Settle();
    says(own_tip(), "Light");
    Theme("Abyss"); Settle();
    says(own_tip() + QString("\nAmount: +1.00000001 ") + name, "back in Abyss");
    Settle();
}

void AbyssTests::transactionsPlainAmounts()
{
    // The Transactions table's Amount column in Abyss: every decimal the
    // amount has, without the trailing zeros, the brackets of an amount that
    // does not count yet kept, the exact text in the cell's tooltip. What
    // leaves the screen (copy, export, sorting, the amount filter) stays
    // exact. A classic theme shows all eight decimals; a theme switch changes
    // what the view shows without resetting the model.
    RestoreAppearance restore;
    Theme("Light");
    WalletFixture fixture(m_node);
    const QString own = fixture.model->getAddressTableModel()->addRow(AddressTableModel::Receive, "", "");
    QVERIFY(!own.isEmpty());
    struct Case { CAmount amount; const char* abyss; };
    const std::vector<Case> cases{{COIN + 1, "1.00000001"}, {2 * COIN, "2"}, {COIN / 2, "0.5"}, {1000 * COIN, "1 000"}};
    for (uint32_t n = 0; n < cases.size(); ++n) QVERIFY(PayTo(*fixture.wallet, own, n, cases[n].amount));
    Settle();
    TransactionTableModel* model = fixture.model->getTransactionTableModel();
    QCOMPARE(model->rowCount({}), int(cases.size()));
    const int unit = fixture.options->getDisplayUnit();
    TransactionView view;
    view.setModel(fixture.model.get());
    view.resize(1280, 700);
    view.show(); Settle();
    auto* table = view.findChild<QTableView*>("transactionView");
    QVERIFY(table && table->model());
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    // A row of the view and of the model, by amount.
    const auto cell = [](QAbstractItemModel* m, CAmount amount) {
        for (int r = 0; r < m->rowCount({}); ++r) {
            const QModelIndex index = m->index(r, TransactionTableModel::Amount);
            if (index.data(TransactionTableModel::AmountRole).toLongLong() == amount) return index;
        }
        return QModelIndex();
    };
    // A text without the brackets formatTxAmount would put around an amount
    // that does not count yet (they never show today: its test reads the
    // MasternodeReward enumerator), and the classic text without the trailing
    // zeros of its fraction, brackets kept: what Abyss must show.
    const auto unbracketed = [](const QString& text) {
        return text.startsWith(QLatin1Char('[')) && text.endsWith(QLatin1Char(']')) ? text.mid(1, text.size() - 2) : text;
    };
    const auto trimmed = [&](const QString& classic_text) {
        QString number = unbracketed(classic_text);
        const bool bracketed = number.size() != classic_text.size();
        if (number.contains(QLatin1Char('.'))) {
            while (number.endsWith(QLatin1Char('0'))) number.chop(1);
            if (number.endsWith(QLatin1Char('.'))) number.chop(1);
        }
        return bracketed ? QLatin1Char('[') + number + QLatin1Char(']') : number;
    };
    std::map<CAmount, QString> classic_texts;
    const auto shows = [&](bool modern, const QString& what) {
        for (const Case& c : cases) {
            const QModelIndex index = cell(model, c.amount);
            const QModelIndex shown = cell(table->model(), c.amount);
            QVERIFY2(index.isValid() && shown.isValid(), qPrintable(what));
            const QString plain = BitcoinUnits::format(unit, c.amount, false, BitcoinUnits::SeparatorStyle::ALWAYS);
            const QString text = index.data(Qt::DisplayRole).toString();
            if (modern) {
                QVERIFY2(classic_texts.count(c.amount), qPrintable(what));
                QVERIFY2(text == trimmed(classic_texts[c.amount]) && unbracketed(text) == Thin(c.abyss), qPrintable(what + ": " + text));
            } else {
                QVERIFY2(unbracketed(text) == plain, qPrintable(what + ": " + text));
                classic_texts[c.amount] = text;
            }
            QCOMPARE(shown.data(Qt::DisplayRole).toString(), text);
            const QString row_tip = index.sibling(index.row(), TransactionTableModel::Date).data(Qt::ToolTipRole).toString();
            QVERIFY(!row_tip.isEmpty());
            const QString exact = BitcoinUnits::formatWithUnit(unit, c.amount, false, BitcoinUnits::SeparatorStyle::ALWAYS);
            QCOMPARE(index.data(Qt::ToolTipRole).toString(), modern ? exact : row_tip);
            QCOMPARE(shown.data(Qt::ToolTipRole).toString(), modern ? exact : row_tip);
            // What leaves the screen does not change with the theme.
            const QString unseparated = BitcoinUnits::format(unit, c.amount, false, BitcoinUnits::SeparatorStyle::NEVER);
            QCOMPARE(index.data(TransactionTableModel::FormattedAmountRole).toString(), unseparated);
            QVERIFY(index.data(TransactionTableModel::TxPlainTextRole).toString().contains(unseparated));
            QCOMPARE(index.data(TransactionTableModel::AmountRole).toLongLong(), qint64(c.amount));
            QCOMPARE(index.data(Qt::EditRole).toLongLong(), qint64(c.amount));
        }
    };
    shows(false, "Light");
    Theme("Abyss"); Settle();
    shows(true, "Abyss");
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) QVERIFY(QDir().mkpath(output) && view.grab().save(QDir(output).filePath("Abyss-transactions-plain.png")));
    Theme("Dark"); Settle();
    shows(false, "Dark");
    Theme("Abyss"); Settle();
    shows(true, "back in Abyss");
    QCOMPARE(reset.count(), 0);
}

void AbyssTests::receiveAddressFirst()
{
    // Abyss opens Receive on an address to give out: its QR, the address in
    // full on one line, Copy and New address, the label if it has one. It is
    // the one shown before while nothing has been paid to it; else one of the
    // wallet's own unlabeled, unused addresses; else a new one, which the
    // wallet gives without asking for anything. None of them becomes a
    // payment request. A classic theme shows its page as it was and makes no
    // address.
    RestoreAppearance restore;
    const int flash = QApplication::cursorFlashTime();
    QApplication::setCursorFlashTime(0);
    const auto restore_flash = interfaces::MakeHandler([flash] { QApplication::setCursorFlashTime(flash); });
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    WalletFixture fixture(m_node);
    // As a loaded wallet has them: taking a key from the pool signals, from inside the
    // request, that the wallet's addresses changed; the card hears it there and then.
    fixture.wallet->ConnectScriptPubKeyManNotifiers();
    AddressTableModel* table = fixture.model->getAddressTableModel();
    RecentRequestsTableModel* requests = fixture.model->getRecentRequestsTableModel();
    const auto own = [&](const QString& address) {
        const CTxDestination dest = DecodeDestination(address.toStdString());
        return IsValidDestination(dest) && fixture.model->wallet().isSpendable(dest);
    };

    Theme("Light");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    // The pictures compared compare the layout, not where the focus was left.
    const auto unfocus = [&receive] {
        if (QWidget* focused = receive.focusWidget()) focused->clearFocus();
        Settle();
    };
    unfocus();
    const QImage classic = receive.grab().toImage();
    QCOMPARE(ReceivingAddresses(table), QStringList());

    Theme("Abyss"); Settle();
    auto* card = receive.findChild<QFrame*>("abyssReceiveCard");
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    auto* label = receive.findChild<QLabel*>("abyssReceiveLabel");
    auto* qr = receive.findChild<QRImageWidget*>("abyssReceiveQR");
    auto* copy = receive.findChild<QPushButton*>("abyssReceiveCopy");
    auto* fresh = receive.findChild<QPushButton*>("abyssReceiveNewAddress");
    auto* edit = receive.findChild<QPushButton*>("abyssReceiveEditLabel");
    QVERIFY(card && text && label && qr && copy && fresh && edit);
    QVERIFY(card->isVisible() && copy->isVisible() && fresh->isVisible() && edit->isVisible());
    QVERIFY(fresh->isEnabled());
    const QString first = text->text();
    QVERIFY2(own(first), qPrintable(first));
    QCOMPARE(ReceivingAddresses(table), QStringList{first});
    QCOMPARE(requests->rowCount({}), 0);
    QVERIFY(!label->isVisible());
    // The whole address, on one line, inside the card.
    QVERIFY(AmountFits(text));
    QVERIFY(card->rect().contains(QRect(text->mapTo(card, QPoint()), text->size())));
#ifdef USE_QRCODE
    // The QR is the address's payment link, drawn as the request dialog draws one, at the card's size.
    QVERIFY(qr->isVisible() && GUIUtil::HasPixmap(qr));
    QRImageWidget reference;
    reference.setImageSize(ReceiveCoinsDialog::ADDRESS_QR_SIZE);
    QVERIFY(reference.setQR(GUIUtil::formatBitcoinURI(SendCoinsRecipient(first, QString(), 0, QString())), first));
    QCOMPARE(qr->exportImage(), reference.exportImage());
    QCOMPARE(qr->width(), ReceiveCoinsDialog::ADDRESS_QR_SIZE);
#endif
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        QVERIFY(receive.grab().save(QDir(output).filePath("Abyss-receive-address.png")));
    }

    // Shown again, or after a theme round trip, it is the same address, and nothing new is made.
    receive.hide(); Settle(); receive.show(); Settle();
    QCOMPARE(text->text(), first);
    Theme("Dark"); Theme("Abyss"); Settle();
    QCOMPARE(text->text(), first);
    QCOMPARE(ReceivingAddresses(table).size(), 1);

    QApplication::clipboard()->clear();
    copy->click(); Settle();
    QCOMPARE(QApplication::clipboard()->text(), first);

    // New address: another one; the first stays the wallet's.
    fresh->click(); Settle();
    const QString second = text->text();
    QVERIFY2(second != first, qPrintable(second));
    QVERIFY(own(second));
    const QStringList both = ReceivingAddresses(table);
    QCOMPARE(both.size(), 2);
    QVERIFY(both.contains(first) && both.contains(second));
    QCOMPARE(requests->rowCount({}), 0);
    copy->click(); Settle();
    QCOMPARE(QApplication::clipboard()->text(), second);

    // A label given elsewhere shows at once, and the card keeps its address.
    const int second_row = table->lookupAddress(second);
    QVERIFY(second_row >= 0);
    QVERIFY(table->setData(table->index(second_row, AddressTableModel::Label, QModelIndex()), QString("Savings"), Qt::EditRole));
    Settle();
    QVERIFY(label->isVisible());
    QCOMPARE(label->text(), QString("Savings"));
    QCOMPARE(text->text(), second);
    // Edit label opens the address book's own dialog on this address.
    edit->click(); Settle();
    QPointer<EditAddressDialog> dialog = receive.findChild<EditAddressDialog*>();
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(dialog->findChild<QLineEdit*>("addressEdit")->text(), second);
    QCOMPARE(dialog->findChild<QLineEdit*>("labelEdit")->text(), QString("Savings"));
    dialog->reject(); Settle();
    QVERIFY(!dialog || !dialog->isVisible());

    // Two the card must never offer: an address given out with a label, and
    // one in the address book that the wallet cannot spend from.
    const QString alice = table->addRow(AddressTableModel::Receive, "Alice", "");
    QVERIFY(own(alice));
    CKey other_key;
    other_key.MakeNewKey(true);
    const QString foreign = QString::fromStdString(EncodeDestination(PKHash(other_key.GetPubKey())));
    fixture.wallet->SetAddressBook(PKHash(other_key.GetPubKey()), "", "receive");
    Settle();
    QVERIFY(ReceivingAddresses(table).contains(alice) && ReceivingAddresses(table).contains(foreign));
    QCOMPARE(text->text(), second);
    int count = ReceivingAddresses(table).size();

    // Paid, the address is done with: the card goes to the unlabeled first one,
    // and when that is paid too, to a new one.
    QVERIFY(PayTo(*fixture.wallet, second, 0));
    Settle();
    QCOMPARE(text->text(), first);
    QVERIFY(PayTo(*fixture.wallet, first, 1));
    Settle();
    const QString third = text->text();
    QVERIFY2(third != first && third != second && third != alice && third != foreign && own(third), qPrintable(third));
    QCOMPARE(ReceivingAddresses(table).size(), ++count);
    // Paid while the page is away: it moves on when it is shown again, not before.
    receive.hide(); Settle();
    QVERIFY(PayTo(*fixture.wallet, third, 2));
    Settle();
    QCOMPARE(ReceivingAddresses(table).size(), count);
    receive.show(); Settle();
    const QString fourth = text->text();
    QVERIFY2(fourth != third && fourth != alice && fourth != foreign && own(fourth), qPrintable(fourth));
    QCOMPARE(ReceivingAddresses(table).size(), ++count);
    QCOMPARE(requests->rowCount({}), 0);

    // The request form, opened from the card: a label and an amount, and no
    // message, not even one typed earlier where the field was shown.
    auto* toggle = receive.findChild<QPushButton*>("abyssReceiveRequestToggle");
    auto* form = receive.findChild<QFrame*>("frame2");
    auto* message = receive.findChild<QLineEdit*>("reqMessage");
    auto* request_label = receive.findChild<QLineEdit*>("reqLabel");
    auto* amount = receive.findChild<BitcoinAmountField*>("reqAmount");
    auto* create = receive.findChild<QPushButton*>("receiveButton");
    QVERIFY(toggle && form && message && request_label && amount && create);
    QVERIFY(!form->isVisible());
    toggle->click(); Settle();
    QVERIFY(form->isVisible() && request_label->isVisible() && amount->isVisible() && create->isVisible());
    QVERIFY(!message->isVisible());
    message->setText("typed in a classic theme");
    request_label->setText("Bob");
    amount->setValue(5 * COIN);
    create->click(); Settle();
    for (ReceiveRequestDialog* shown : receive.findChildren<ReceiveRequestDialog*>()) shown->close();
    QCOMPARE(requests->rowCount({}), 1);
    QCOMPARE(requests->entry(0).recipient.label, QString("Bob"));
    QCOMPARE(requests->entry(0).recipient.amount, 5 * COIN);
    QCOMPARE(requests->entry(0).recipient.message, QString());
    QCOMPARE(text->text(), fourth);
    QVERIFY(requests->removeRows(0, 1));
    toggle->click(); Settle();
    QVERIFY(!form->isVisible());

    // The classic page is the one it was before Abyss, card and all gone.
    Theme("Light"); Settle();
    QVERIFY(!card->isVisible());
    QVERIFY(form->isVisible() && message->isVisible());
    unfocus();
    QCOMPARE(receive.grab().toImage(), classic);
}

void AbyssTests::receiveUsedAddresses()
{
    // "Paid" is read from the wallet's own transactions, output by output. The
    // transaction list is not a record of them: a payment to self shows its
    // addresses joined on one line ("A, B"), and an outgoing payment does not
    // list what it pays to the wallet's own address. Both count, on the page
    // already open and on one opened afterwards.
    RestoreAppearance restore;
    WalletFixture fixture(m_node);
    fixture.wallet->ConnectScriptPubKeyManNotifiers();
    AddressTableModel* table = fixture.model->getAddressTableModel();
    TransactionTableModel* list = fixture.model->getTransactionTableModel();
    const auto own = [&](const QString& address) {
        const CTxDestination dest = DecodeDestination(address.toStdString());
        return IsValidDestination(dest) && fixture.model->wallet().isSpendable(dest);
    };
    const auto script = [](const QString& address) { return GetScriptForDestination(DecodeDestination(address.toStdString())); };
    const auto listed = [&](const QString& address) {
        QStringList rows;
        for (int row = 0; row < list->rowCount({}); ++row) {
            const QString text = list->index(row, 0).data(TransactionTableModel::AddressRole).toString();
            if (text.contains(address)) rows << text;
        }
        return rows;
    };
    Theme("Abyss");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    QVERIFY(text);
    const QString first = text->text();
    QVERIFY(own(first));
    // Two given out for something (labeled), so the card never offers them.
    const QString funding = table->addRow(AddressTableModel::Receive, "funding", "");
    const QString savings = table->addRow(AddressTableModel::Receive, "savings", "");
    Settle();
    QVERIFY(own(funding) && own(savings));
    QCOMPARE(text->text(), first);

    // Coins for the wallet to spend, from nowhere in particular.
    CMutableTransaction fund;
    fund.vin.emplace_back(COutPoint(uint256S("0200000000000000000000000000000000000000000000000000000000000000"), 0));
    fund.vout.emplace_back(10 * COIN, script(funding));
    const CTransactionRef fund_tx = MakeTransactionRef(std::move(fund));
    QVERIFY(fixture.wallet->AddToWallet(fund_tx, CWalletTx::Confirmation{}));
    Settle();
    QCOMPARE(text->text(), first);

    // The wallet pays itself, to the shown address and to "savings": one row names both.
    CMutableTransaction self;
    self.vin.emplace_back(COutPoint(fund_tx->GetHash(), 0));
    self.vout.emplace_back(4 * COIN, script(first));
    self.vout.emplace_back(5 * COIN, script(savings));
    const CTransactionRef self_tx = MakeTransactionRef(std::move(self));
    QVERIFY(fixture.wallet->AddToWallet(self_tx, CWalletTx::Confirmation{}));
    Settle();
    const QStringList joined = listed(first);
    QVERIFY2(joined.size() == 1 && joined.front() != first && joined.front().contains(savings), qPrintable(joined.join(" | ")));
    const QString second = text->text();
    QVERIFY2(second != first && second != funding && second != savings && own(second), qPrintable(second));

    // An outgoing payment that also pays the shown address: the list has no row for that part.
    CKey other_key;
    other_key.MakeNewKey(true);
    const QString elsewhere = QString::fromStdString(EncodeDestination(PKHash(other_key.GetPubKey())));
    CMutableTransaction out;
    out.vin.emplace_back(COutPoint(self_tx->GetHash(), 0));
    out.vout.emplace_back(1 * COIN, script(elsewhere));
    out.vout.emplace_back(2 * COIN, script(second));
    QVERIFY(fixture.wallet->AddToWallet(MakeTransactionRef(std::move(out)), CWalletTx::Confirmation{}));
    Settle();
    QVERIFY2(listed(second).isEmpty(), qPrintable(listed(second).join(" | ")));
    QVERIFY(!listed(elsewhere).isEmpty());
    const QString third = text->text();
    QVERIFY2(third != first && third != second && third != funding && third != savings && own(third), qPrintable(third));

    // The address paid while it was somebody else's becomes the wallet's own: its key
    // imported, without a label, as importprivkey does with no rescan. It has been paid,
    // so when the shown one is paid too the card takes a new address, not that one.
    {
        auto spk_man = fixture.wallet->GetOrCreateLegacyScriptPubKeyMan();
        LOCK2(fixture.wallet->cs_wallet, spk_man->cs_KeyStore);
        QVERIFY(spk_man->AddKeyPubKey(other_key, other_key.GetPubKey()));
    }
    fixture.wallet->SetAddressBook(PKHash(other_key.GetPubKey()), "", "receive");
    Settle();
    QVERIFY(own(elsewhere) && ReceivingAddresses(table).contains(elsewhere));
    QCOMPARE(text->text(), third);
    CMutableTransaction pay_third;
    pay_third.vin.emplace_back(COutPoint(uint256S("0300000000000000000000000000000000000000000000000000000000000000"), 0));
    pay_third.vout.emplace_back(COIN, script(third));
    QVERIFY(fixture.wallet->AddToWallet(MakeTransactionRef(std::move(pay_third)), CWalletTx::Confirmation{}));
    Settle();
    const QString fourth = text->text();
    QVERIFY2(fourth != elsewhere && fourth != third && fourth != second && fourth != first && own(fourth), qPrintable(fourth));

    // A page opened afterwards reads the same from the wallet: of the unlabeled
    // addresses only the fourth is unpaid.
    ReceiveCoinsDialog later;
    later.setModel(fixture.model.get());
    later.resize(1280, 850); later.show(); Settle();
    QCOMPARE(later.findChild<QLabel*>("abyssReceiveAddress")->text(), fourth);
}

void AbyssTests::receiveKeyless()
{
    // A wallet without private keys can still hand out addresses: public keys
    // imported into its pool. The card offers none of them, makes none, and
    // its New address is off; the request form keeps the classic way.
    RestoreAppearance restore;
    WalletFixture fixture(m_node, /*dummy_database=*/false, WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    CKey key;
    key.MakeNewKey(true);
    const CPubKey pubkey = key.GetPubKey();
    {
        LOCK(fixture.wallet->cs_wallet);
        QVERIFY(fixture.wallet->ImportPubKeys({pubkey.GetID()}, {{pubkey.GetID(), pubkey}}, {}, /*add_keypool=*/true, /*internal=*/false, /*timestamp=*/1));
    }
    const auto pool = [&] { LOCK(fixture.wallet->cs_wallet); return fixture.wallet->KeypoolCountExternalKeys(); };
    QCOMPARE(pool(), size_t{1});
    QVERIFY(fixture.model->wallet().canGetAddresses());
    QVERIFY(fixture.model->wallet().privateKeysDisabled());
    Theme("Abyss");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    auto* copy = receive.findChild<QPushButton*>("abyssReceiveCopy");
    auto* fresh = receive.findChild<QPushButton*>("abyssReceiveNewAddress");
    auto* qr = receive.findChild<QRImageWidget*>("abyssReceiveQR");
    QVERIFY(text && copy && fresh && qr);
    QCOMPARE(text->text(), ReceiveCoinsDialog::tr("No receiving address yet."));
    QVERIFY(!copy->isVisible() && !qr->isVisible());
    QVERIFY(!fresh->isEnabled());
    QCOMPARE(pool(), size_t{1});
    // Shown again, or the wallet's addresses signalled as changed: still nothing taken.
    receive.hide(); Settle(); receive.show(); Settle();
    Q_EMIT fixture.model->canGetAddressesChanged();
    Settle();
    QCOMPARE(pool(), size_t{1});
    QCOMPARE(text->text(), ReceiveCoinsDialog::tr("No receiving address yet."));
    QCOMPARE(fixture.model->getAddressTableModel()->rowCount({}), 0);
}

void AbyssTests::receiveKeylessDescriptor()
{
    // A descriptor wallet without private keys reports what its descriptors cover
    // as its own and spendable, and importing a single-key descriptor gives it an
    // unlabeled receiving address. The card shows none of it.
    RestoreAppearance restore;
    WalletFixture fixture(m_node, /*dummy_database=*/false, WALLET_FLAG_DESCRIPTORS | WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    CKey key;
    key.MakeNewKey(true);
    FlatSigningProvider provider;
    std::string error;
    const CPubKey pubkey = key.GetPubKey();
    std::unique_ptr<Descriptor> parsed = Parse("pkh(" + HexStr(pubkey) + ")", provider, error, /*require_checksum=*/false);
    QVERIFY2(parsed, error.c_str());
    WalletDescriptor descriptor(std::move(parsed), /*creation_time=*/1, /*range_start=*/0, /*range_end=*/0, /*next_index=*/0);
    QVERIFY(fixture.wallet->AddWalletDescriptor(descriptor, provider, "", /*internal=*/false));
    Settle();
    const QString watched = QString::fromStdString(EncodeDestination(PKHash(pubkey)));
    AddressTableModel* table = fixture.model->getAddressTableModel();
    QVERIFY(ReceivingAddresses(table).contains(watched));
    QVERIFY(fixture.model->wallet().privateKeysDisabled());
    // The case: the wallet calls it spendable.
    QVERIFY(fixture.model->wallet().isSpendable(DecodeDestination(watched.toStdString())));
    Theme("Abyss");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    auto* copy = receive.findChild<QPushButton*>("abyssReceiveCopy");
    auto* fresh = receive.findChild<QPushButton*>("abyssReceiveNewAddress");
    QVERIFY(text && copy && fresh);
    QCOMPARE(text->text(), ReceiveCoinsDialog::tr("No receiving address yet."));
    QVERIFY(!copy->isVisible() && !fresh->isEnabled());
}

void AbyssTests::receiveMadeAddressPaid()
{
    // A request for a new address may return one a wallet transaction has paid
    // already: a descriptor imported again from its first index does. Here the
    // legacy pool's next key is paid by a transaction added the direct way, which
    // leaves the key in the pool. The card's own request does not show it and asks
    // no more by itself; New address does not show it either, says so, and keeps
    // the card's address, and its next press gets a fresh one.
    RestoreAppearance restore;
    WalletFixture fixture(m_node);
    fixture.wallet->ConnectScriptPubKeyManNotifiers();
    AddressTableModel* table = fixture.model->getAddressTableModel();
    const auto own = [&](const QString& address) {
        const CTxDestination dest = DecodeDestination(address.toStdString());
        return IsValidDestination(dest) && fixture.model->wallet().isSpendable(dest);
    };
    // The pool's next key, given back when the reservation ends.
    const auto next_key = [&] {
        ReserveDestination peek(fixture.wallet.get());
        CTxDestination dest;
        return peek.GetReservedDestination(dest, /*internal=*/false) ? QString::fromStdString(EncodeDestination(dest)) : QString();
    };
    const QString paid = next_key();
    QVERIFY(!paid.isEmpty());
    QVERIFY(PayTo(*fixture.wallet, paid, 0));
    Settle();
    QCOMPARE(next_key(), paid);

    Theme("Abyss");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    auto* fresh = receive.findChild<QPushButton*>("abyssReceiveNewAddress");
    QVERIFY(text && fresh);
    // The wallet did give it out, and the card did not show it.
    QVERIFY(ReceivingAddresses(table).contains(paid));
    QCOMPARE(text->text(), ReceiveCoinsDialog::tr("No receiving address yet."));
    const int count = ReceivingAddresses(table).size();
    receive.hide(); Settle(); receive.show(); Settle();
    Q_EMIT fixture.model->canGetAddressesChanged();
    Settle();
    QCOMPARE(ReceivingAddresses(table).size(), count);

    // New address, asked by hand: the pool's next key is a fresh one.
    QVERIFY(fresh->isEnabled());
    fresh->click(); Settle();
    const QString made = text->text();
    QVERIFY2(made != paid && own(made), qPrintable(made));

    // New address that gets a paid one: an error, and the card keeps its address.
    const QString paid_next = next_key();
    QVERIFY(!paid_next.isEmpty() && paid_next != made);
    QVERIFY(PayTo(*fixture.wallet, paid_next, 1));
    Settle();
    QCOMPARE(text->text(), made);
    QString shown_error;
    QTimer closer;
    closer.setInterval(50);
    QObject::connect(&closer, &QTimer::timeout, [&shown_error] {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(window);
            if (box && box->isVisible()) {
                shown_error = box->text();
                box->accept();
            }
        }
    });
    closer.start();
    fresh->click(); Settle();
    closer.stop();
    QCOMPARE(shown_error, ReceiveCoinsDialog::tr("Could not generate new address"));
    QCOMPARE(text->text(), made);
    QVERIFY(ReceivingAddresses(table).contains(paid_next));
    QVERIFY(fresh->isEnabled());
}

void AbyssTests::receivePartialKeys()
{
    // A descriptor wallet with private keys takes a multisig descriptor with only
    // some of its keys (the import only warns), calls its script its own and
    // spendable, and gives it an unlabeled receiving address, though it cannot
    // spend from it alone. The card shows only a single key's address: not that
    // one; a single-key descriptor's, imported with its key, yes, as soon as it
    // is in the list.
    RestoreAppearance restore;
    WalletFixture fixture(m_node, /*dummy_database=*/false, WALLET_FLAG_DESCRIPTORS);
    const auto add = [&](const std::string& text) {
        FlatSigningProvider provider;
        std::string error;
        std::unique_ptr<Descriptor> parsed = Parse(text, provider, error, /*require_checksum=*/false);
        if (!parsed) {
            qWarning() << "RECEIVE_PARTIAL_KEYS" << error.c_str();
            return false;
        }
        WalletDescriptor descriptor(std::move(parsed), /*creation_time=*/1, /*range_start=*/0, /*range_end=*/0, /*next_index=*/0);
        return fixture.wallet->AddWalletDescriptor(descriptor, provider, "", /*internal=*/false) != nullptr;
    };
    AddressTableModel* table = fixture.model->getAddressTableModel();
    const auto spendable = [&](const QString& address) { return fixture.model->wallet().isSpendable(DecodeDestination(address.toStdString())); };
    CKey mine, theirs, single;
    mine.MakeNewKey(true);
    theirs.MakeNewKey(true);
    single.MakeNewKey(true);
    const CPubKey mine_pub = mine.GetPubKey(), theirs_pub = theirs.GetPubKey(), single_pub = single.GetPubKey();
    QVERIFY(add("sh(multi(2," + EncodeSecret(mine) + "," + HexStr(theirs_pub) + "))"));
    Settle();
    const QString shared = QString::fromStdString(EncodeDestination(ScriptHash(GetScriptForMultisig(2, {mine_pub, theirs_pub}))));
    // The case: an unlabeled receiving address the wallet calls spendable, in a wallet with private keys.
    QVERIFY2(ReceivingAddresses(table).contains(shared), qPrintable(ReceivingAddresses(table).join(", ")));
    QVERIFY(spendable(shared));
    QVERIFY(!fixture.model->wallet().privateKeysDisabled());
    Theme("Abyss");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    auto* copy = receive.findChild<QPushButton*>("abyssReceiveCopy");
    auto* fresh = receive.findChild<QPushButton*>("abyssReceiveNewAddress");
    QVERIFY(text && copy && fresh);
    QCOMPARE(text->text(), ReceiveCoinsDialog::tr("No receiving address yet."));
    QVERIFY(!copy->isVisible());
    // No active descriptor to give addresses from: nothing to make either.
    QVERIFY(!fresh->isEnabled());

    // A single key's descriptor with its key: the card takes it when the list gets it.
    QVERIFY(add("pkh(" + EncodeSecret(single) + ")"));
    Settle();
    const QString own = QString::fromStdString(EncodeDestination(PKHash(single_pub)));
    QVERIFY(ReceivingAddresses(table).contains(own) && spendable(own));
    QCOMPARE(text->text(), own);
    QVERIFY(copy->isVisible());
}

void AbyssTests::receivePurposeChanged()
{
    // An entry of the address list changes in place for a new purpose as for a
    // new label (addmultisigaddress turns a receiving script into a sending one).
    // The card looks again at every change: a label given elsewhere keeps its
    // address; a purpose no longer "receive" sends it on to another address, on
    // the page shown and, when shown again, on one away.
    RestoreAppearance restore;
    WalletFixture fixture(m_node);
    fixture.wallet->ConnectScriptPubKeyManNotifiers();
    const auto own = [&](const QString& address) {
        const CTxDestination dest = DecodeDestination(address.toStdString());
        return IsValidDestination(dest) && fixture.model->wallet().isSpendable(dest);
    };
    Theme("Abyss");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    auto* label = receive.findChild<QLabel*>("abyssReceiveLabel");
    QVERIFY(text && label);
    const QString first = text->text();
    QVERIFY2(own(first), qPrintable(first));
    const CTxDestination first_dest = DecodeDestination(first.toStdString());
    fixture.wallet->SetAddressBook(first_dest, "Rent", "receive");
    Settle();
    QCOMPARE(text->text(), first);
    QVERIFY(label->isVisible());
    QCOMPARE(label->text(), QString("Rent"));
    fixture.wallet->SetAddressBook(first_dest, "Rent", "send");
    Settle();
    const QString second = text->text();
    QVERIFY2(second != first && own(second), qPrintable(second));
    receive.hide(); Settle();
    fixture.wallet->SetAddressBook(DecodeDestination(second.toStdString()), "", "send");
    Settle();
    receive.show(); Settle();
    const QString third = text->text();
    QVERIFY2(third != second && third != first && own(third), qPrintable(third));
}

void AbyssTests::receiveWalletGone()
{
    // At shutdown the wallet models go before their pages, in whatever event loop
    // runs, a box's of this page too. Here New address gets a paid address and
    // says so in a box, while the label dialog is open; the wallet model goes
    // while the box is up, in the shutdown's order. The label dialog goes with
    // it, the card offers nothing of it, and nothing reads it after the box.
    RestoreAppearance restore;
    WalletFixture fixture(m_node);
    fixture.wallet->ConnectScriptPubKeyManNotifiers();
    Theme("Abyss");
    ReceiveCoinsDialog receive;
    receive.setModel(fixture.model.get());
    receive.resize(1280, 850); receive.show(); Settle();
    auto* text = receive.findChild<QLabel*>("abyssReceiveAddress");
    auto* copy = receive.findChild<QPushButton*>("abyssReceiveCopy");
    auto* fresh = receive.findChild<QPushButton*>("abyssReceiveNewAddress");
    auto* edit = receive.findChild<QPushButton*>("abyssReceiveEditLabel");
    QVERIFY(text && copy && fresh && edit);
    const QString first = text->text();
    QVERIFY(!first.isEmpty() && copy->isVisible() && fresh->isEnabled());
    edit->click(); Settle();
    QPointer<EditAddressDialog> dialog = receive.findChild<EditAddressDialog*>();
    QVERIFY(dialog && dialog->isVisible());
    // The pool's next key, paid: New address gets it, and says it could not make one.
    QString paid;
    {
        ReserveDestination peek(fixture.wallet.get());
        CTxDestination dest;
        QVERIFY(peek.GetReservedDestination(dest, /*internal=*/false));
        paid = QString::fromStdString(EncodeDestination(dest));
    }
    QVERIFY(PayTo(*fixture.wallet, paid, 0));
    Settle();
    QCOMPARE(text->text(), first);
    bool gone = false, closed = false;
    QString shown_error;
    QTimer shutdown;
    shutdown.setInterval(20);
    QObject::connect(&shutdown, &QTimer::timeout, [&] {
        for (QWidget* window : QApplication::topLevelWidgets()) {
            auto* box = qobject_cast<QMessageBox*>(window);
            if (box == nullptr || !box->isVisible()) continue;
            if (!gone) {
                shown_error = box->text();
                fixture.model.reset(); // as the shutdown's wallet controller deletes its models
                gone = true;
                return;
            }
            closed = true;
            shutdown.stop();
            box->accept();
            return;
        }
    });
    shutdown.start();
    fresh->click(); Settle();
    shutdown.stop();
    QVERIFY(gone && closed);
    QCOMPARE(shown_error, ReceiveCoinsDialog::tr("Could not generate new address"));
    QVERIFY(!dialog || !dialog->isVisible());
    QCOMPARE(text->text(), ReceiveCoinsDialog::tr("No receiving address yet."));
    QVERIFY(!copy->isVisible() && !edit->isVisible() && !fresh->isEnabled());
    // Shown again, its buttons pressed: nothing to read, nothing done.
    receive.hide(); Settle(); receive.show(); Settle();
    fresh->click(); copy->click(); Settle();
    QCOMPARE(text->text(), ReceiveCoinsDialog::tr("No receiving address yet."));
}

void AbyssTests::receiveToast()
{
    // Copy says "Address copied" under the card's buttons for two seconds,
    // then the line goes. It moves nothing, takes no focus, and stops at once
    // when the page is left (the window stays, as when the wallet's stack
    // shows another page) or the window loses the focus.
    RestoreAppearance restore;
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    WalletFixture fixture(m_node);
    Theme("Abyss");
    // A child page, as in the wallet's stack: hiding it leaves its window shown and active.
    QWidget host;
    auto* receive = new ReceiveCoinsDialog;
    receive->setParent(&host);
    receive->setModel(fixture.model.get());
    host.resize(1280, 850); receive->setGeometry(0, 0, 1280, 850);
    host.show(); receive->show(); Settle();
    auto* card = receive->findChild<QFrame*>("abyssReceiveCard");
    auto* copy = receive->findChild<QPushButton*>("abyssReceiveCopy");
    auto* toggle = receive->findChild<QPushButton*>("abyssReceiveRequestToggle");
    auto* toast = receive->findChild<AbyssToast*>("abyssToast");
    QVERIFY(card && copy && toggle && toast);
    QVERIFY(copy->isVisible() && !toast->isVisible());
    QCOMPARE(toast->focusPolicy(), Qt::NoFocus);
    QVERIFY(!toast->isWindow());
    const QRect button = OnPage(*receive, copy);
    const QRect card_rect = OnPage(*receive, card);
    toggle->setFocus(Qt::OtherFocusReason); Settle();
    QWidget* focused = host.focusWidget();
    QCOMPARE(focused, toggle);

    QElapsedTimer clock;
    clock.start();
    copy->click();
    QVERIFY(toast->isVisible() && toast->isShowing());
    QCOMPARE(toast->text(), ReceiveCoinsDialog::tr("Address copied"));
    Settle();
    QCOMPARE(host.focusWidget(), focused);
    QCOMPARE(OnPage(*receive, copy), button);
    QCOMPARE(OnPage(*receive, card), card_rect);
    QVERIFY(card_rect.contains(OnPage(*receive, toast)));
    QVERIFY(OnPage(*receive, toast).top() >= button.bottom());
    if (!output.isEmpty()) {
        QTest::qWait(400); // past the fade in
        QVERIFY(QDir().mkpath(output));
        QVERIFY(receive->grab().save(QDir(output).filePath("Abyss-receive-toast.png")));
    }
    QTRY_VERIFY_WITH_TIMEOUT(!toast->isVisible(), AbyssToast::DURATION_MS + 2000);
    const qint64 shown = clock.elapsed();
    qInfo() << "RECEIVE_TOAST shown for" << shown << "ms";
    QVERIFY2(shown >= AbyssToast::DURATION_MS - 250, qPrintable(QString::number(shown)));
    QVERIFY(!toast->isShowing());
    QCOMPARE(host.focusWidget(), focused);
    QCOMPARE(OnPage(*receive, copy), button);

    // A second copy while it shows starts the two seconds again.
    copy->click();
    QTest::qWait(AbyssToast::DURATION_MS / 2);
    copy->click();
    QTest::qWait(AbyssToast::DURATION_MS * 3 / 4);
    QVERIFY(toast->isShowing());
    QTRY_VERIFY_WITH_TIMEOUT(!toast->isVisible(), AbyssToast::DURATION_MS + 2000);

    // The page left while the window stays: it stops; back on the page, no toast is left over.
    copy->click();
    QVERIFY(toast->isShowing());
    receive->hide(); Settle();
    QVERIFY(host.isVisible());
    QVERIFY(!toast->isShowing());
    receive->show(); Settle();
    QVERIFY(!toast->isVisible() && !toast->isShowing());
    // The window losing the focus ends it too.
    copy->click();
    QVERIFY(toast->isShowing());
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&host, &deactivate);
    QVERIFY(!toast->isShowing() && !toast->isVisible());
    // No toast on a page that is not on screen.
    receive->hide(); Settle();
    toast->showMessage("unseen");
    QVERIFY(!toast->isShowing());
    receive->show(); Settle();
    QVERIFY(!toast->isVisible());
    // A classic theme has no card, and so no toast.
    Theme("Light"); Settle();
    QVERIFY(!toast->isVisible() && !card->isVisible());
}

void AbyssTests::receiveFits()
{
    // List item 18: at 175% on a display some 1600 pixels high, the bottom of
    // the Receive form and of the empty history card were cut off. The page now
    // scrolls rather than asking the window for more, and what it shows first
    // (the address card, the open request form and a folded history) fits in
    // the height the unlocked Overview takes anyway, so the window the Overview
    // gets, as on that display, shows it all. Also run at QT_SCALE_FACTOR=1.75.
    if (!GUIUtil::fontsLoaded()) QSKIP("Page heights require a font-capable platform; run minimal:enable_fonts");
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    GUIUtil::setFontFamily(GUIUtil::FontFamily::Roboto);
    GUIUtil::setFontScale(0);
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    // Both pages as children, as in the wallet's stack.
    QWidget overview_host;
    auto* overview = new OverviewPage(&overview_host);
    overview->setClientModel(fixture.client.get());
    overview->setWalletModel(fixture.model.get());
    overview->setStakingStatus(StakingCase("off"));
    overview_host.resize(1280, 850); overview->setGeometry(0, 0, 1280, 850);
    overview_host.show(); Settle();
    QWidget host;
    auto* receive = new ReceiveCoinsDialog;
    receive->setParent(&host);
    receive->setModel(fixture.model.get());
    host.resize(1280, 850); receive->setGeometry(0, 0, 1280, 850);
    host.show(); receive->show(); Settle();
    auto* scroll = receive->findChild<QScrollArea*>("abyssReceiveScroll");
    auto* card = receive->findChild<QFrame*>("abyssReceiveCard");
    auto* toggle = receive->findChild<QPushButton*>("abyssReceiveRequestToggle");
    auto* form = receive->findChild<QFrame*>("frame2");
    auto* create = receive->findChild<QPushButton*>("receiveButton");
    auto* history = receive->findChild<QFrame*>("frame");
    auto* none = receive->findChild<QLabel*>("abyssReceiveHistoryNone");
    auto* header = receive->findChild<QWidget*>("abyssPageHeader");
    QVERIFY(scroll && card && toggle && form && create && history && none && header);
    toggle->click(); Settle();
    QVERIFY(form->isVisible() && none->isVisible());
    QStringList found;
    interfaces::WalletBalances zero{};
    interfaces::WalletBalances large{};
    large.balance = 50'820'975 * COIN + 99 * COIN / 100;
    for (int width : {704, 1026, 1225}) {
        // The smaller of the unlocked Overview's minimums, with and without a balance.
        int height = std::numeric_limits<int>::max();
        for (const interfaces::WalletBalances* balances : {&zero, &large}) {
            overview->setBalance(*balances); Settle();
            overview->setGeometry(0, 0, std::max(width, overview->minimumWidth()), 850); Settle();
            height = std::min(height, overview->minimumSizeHint().height());
        }
        host.resize(width, height);
        receive->setGeometry(0, 0, width, height); Settle();
        const QString at = QString("at %1x%2").arg(width).arg(height);
        qInfo().noquote() << "RECEIVE_FITS" << at << "header" << header->height() << "card" << card->height()
                          << "link" << toggle->height() << "form" << form->height() << "history" << history->height()
                          << "viewport" << scroll->viewport()->height() << "needs" << (scroll->widget() ? scroll->widget()->minimumSizeHint().height() : -1)
                          << "page minimum" << receive->minimumSizeHint().height() << "DPR" << receive->devicePixelRatioF();
        if (scroll->verticalScrollBar()->maximum() != 0) {
            found << QString("%1: the page scrolls by %2").arg(at).arg(scroll->verticalScrollBar()->maximum());
        }
        // With room to spare: the native trial at 175% (U2) had the history line
        // 20 pixels short with the form open, where this platform measured 36 to
        // spare -- so the native page has some 56 less, and 60 is asked for here.
        const int spare = scroll->viewport()->height() - (scroll->widget() ? scroll->widget()->minimumSizeHint().height() : 0);
        if (spare < 60) found << QString("%1: only %2 px to spare under the history line").arg(at).arg(spare);
        for (QWidget* part : {static_cast<QWidget*>(card), static_cast<QWidget*>(create), static_cast<QWidget*>(none), static_cast<QWidget*>(history)}) {
            const QRect inside = OnPage(*receive, part);
            if (!receive->rect().contains(inside) || part->visibleRegion() != QRegion(part->rect())) {
                found << QString("%1: %2 at %3 is not all on the page").arg(at, part->objectName()).arg(QString("%1,%2 %3x%4").arg(inside.x()).arg(inside.y()).arg(inside.width()).arg(inside.height()));
            }
        }
        // The page never asks the window for more than the Overview does.
        if (receive->minimumSizeHint().height() > height) {
            found << QString("%1: the page asks for %2").arg(at).arg(receive->minimumSizeHint().height());
        }
        if (scroll->horizontalScrollBar()->maximum() != 0) found << QString("%1: the page scrolls sideways").arg(at);
        if (!output.isEmpty() && width == 1225) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(PageImage(*receive).save(QDir(output).filePath("Abyss-receive-fits.png")));
        }
    }
    // Given less than that, the page still asks for no more: it scrolls, and
    // the form's button can be brought into view.
    host.resize(1026, 420);
    receive->setGeometry(0, 0, 1026, 420); Settle();
    qInfo().noquote() << "RECEIVE_FITS at 1026x420: page minimum" << receive->minimumSizeHint().height()
                      << "scrolls by" << scroll->verticalScrollBar()->maximum();
    if (receive->minimumSizeHint().height() > 420) found << QString("at 1026x420 the page asks for %1").arg(receive->minimumSizeHint().height());
    if (scroll->verticalScrollBar()->maximum() == 0) found << "at 1026x420 the page does not scroll";
    scroll->ensureWidgetVisible(create); Settle();
    if (create->visibleRegion() != QRegion(create->rect())) found << "at 1026x420 the form's button cannot be brought into view";
    // A long history takes the room left and scrolls within; the card stays whole above it.
    RecentRequestsTableModel* requests = fixture.model->getRecentRequestsTableModel();
    const QString address = receive->findChild<QLabel*>("abyssReceiveAddress")->text();
    for (int i = 0; i < 30; ++i) requests->addNewRequest(SendCoinsRecipient(address, QString("Request %1").arg(i), (i + 1) * COIN, QString()));
    toggle->click(); Settle();
    QVERIFY(!form->isVisible());
    host.resize(1225, 640); receive->setGeometry(0, 0, 1225, 640); Settle();
    auto* table = receive->findChild<QTableView*>("recentRequestsView");
    QVERIFY(table && table->isVisible() && !none->isVisible());
    qInfo().noquote() << "RECEIVE_FITS history of 30: table" << table->height() << "page scrolls by" << scroll->verticalScrollBar()->maximum();
    if (card->visibleRegion() != QRegion(card->rect())) found << "with 30 requests the card is not whole";
    if (table->verticalScrollBar()->maximum() == 0) found << "the 30 requests do not scroll within the table";
    if (!output.isEmpty()) QVERIFY(PageImage(*receive).save(QDir(output).filePath("Abyss-receive-history.png")));
    for (const QString& line : found) qInfo().noquote() << "RECEIVE_FITS" << line;
    QCOMPARE(found.join("\n"), QString());
}

void AbyssTests::sendInputsFirst()
{
    // U2b: in Abyss the coin control card heads the Send column, above the
    // recipient -- under the recipients and the fee it was a scroll away -- at
    // its own height. With coins chosen its figures take two columns where the
    // column is wide enough, one where it is not. At the height of the unlocked
    // Overview's minimum the Inputs button and the address field show without
    // scrolling. The classic form is the same picture before and after. Also run
    // at QT_SCALE_FACTOR=1.75.
    if (!GUIUtil::fontsLoaded()) QSKIP("Page heights require a font-capable platform; run minimal:enable_fonts");
    RestoreAppearance restore;
    WalletFixture fixture(m_node);
    GUIUtil::setFontFamily(GUIUtil::FontFamily::Roboto);
    GUIUtil::setFontScale(0);
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    // Coins of its own to choose from: the test chain's coinbase key.
    {
        auto spk_man = fixture.wallet->GetOrCreateLegacyScriptPubKeyMan();
        const CBlockIndex* tip = WITH_LOCK(::cs_main, return fixture.chain.m_node.chainman->ActiveChain().Tip());
        LOCK2(fixture.wallet->cs_wallet, spk_man->cs_KeyStore);
        spk_man->AddKeyPubKey(fixture.chain.coinbaseKey, fixture.chain.coinbaseKey.GetPubKey());
        fixture.wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
    }
    {
        WalletRescanReserver reserver(*fixture.wallet);
        reserver.reserve();
        QCOMPARE(fixture.wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, 0, {}, reserver, true).status,
                 CWallet::ScanResult::SUCCESS);
    }
    COutPoint chosen;
    {
        LOCK(fixture.wallet->cs_wallet);
        QVERIFY(!fixture.wallet->mapWallet.empty());
        chosen = COutPoint(fixture.wallet->mapWallet.begin()->first, 0);
    }
    // Made in a classic theme, so Abyss's first pass comes later in its life.
    Theme("Light");
    QWidget overview_host;
    auto* overview = new OverviewPage(&overview_host);
    overview->setClientModel(fixture.client.get());
    overview->setWalletModel(fixture.model.get());
    overview->setStakingStatus(StakingCase("off"));
    QWidget host;
    auto* send = new SendCoinsDialog(false, &host);
    send->setModel(fixture.model.get());
    host.resize(1280, 850); send->setGeometry(0, 0, 1280, 850);
    host.show(); send->show(); Settle();
    QVERIFY(QMetaObject::invokeMethod(send, "coinControlFeatureChanged", Q_ARG(bool, true)));
    send->m_coin_control->Select(chosen);
    QVERIFY(QMetaObject::invokeMethod(send, "coinControlUpdateLabels"));
    Settle();
    auto* control = send->findChild<QFrame*>("frameCoinControl");
    auto* inputs = send->findChild<QPushButton*>("pushButtonCoinControl");
    auto* figures = send->findChild<QWidget*>("widgetCoinControl");
    auto* row = send->findChild<QHBoxLayout*>("horizontalLayoutCoinControl3");
    auto* entry = send->findChild<SendCoinsEntry*>();
    QVERIFY(control && inputs && figures && row && entry);
    auto* pay_to = entry->findChild<QLineEdit*>("payTo");
    QVERIFY(pay_to);
    std::array<QFormLayout*, 4> groups{};
    for (int i = 0; i < 4; ++i) {
        groups[i] = send->findChild<QFormLayout*>(QString("formLayoutCoinControl%1").arg(i + 1));
        QVERIFY(groups[i]);
    }
    QVERIFY(figures->isVisible());
    const auto classic_row = [&] {
        for (int i = 0; i < 4; ++i) {
            if (row->itemAt(i) == nullptr || row->itemAt(i)->layout() != groups[i] || row->stretch(i) != (i == 3 ? 1 : 0)) return false;
        }
        return row->count() == 4;
    };
    QVERIFY(classic_row());
    // Without a focused field: a text cursor blinks.
    const auto classic_picture = [&] {
        if (QWidget* focused = QApplication::focusWidget()) focused->clearFocus();
        Settle();
        return send->grab().toImage();
    };
    const QImage classic = classic_picture();

    Theme("Abyss"); Settle();
    overview_host.resize(1280, 850); overview->setGeometry(0, 0, 1280, 850);
    overview_host.show(); Settle();
    auto* scroll = send->findChild<QScrollArea*>("abyssSendFieldsScroll");
    QVERIFY(scroll && scroll->widget());
    auto* column = qobject_cast<QVBoxLayout*>(scroll->widget()->layout());
    QVERIFY(column);
    QStringList found;
    // First in the column, above the recipient.
    if (column->itemAt(0) == nullptr || column->itemAt(0)->widget() != control) found << "the coin control card is not first in the column";
    // At its own height, however much room the column has.
    host.resize(1280, 1400); send->setGeometry(0, 0, 1280, 1400); Settle();
    if (control->height() != control->sizeHint().height()) {
        found << QString("with room to spare the card is %1 px for %2").arg(control->height()).arg(control->sizeHint().height());
    }
    interfaces::WalletBalances zero{};
    interfaces::WalletBalances large{};
    large.balance = 50'820'975 * COIN + 99 * COIN / 100;
    auto* quantity = send->findChild<QLabel*>("labelCoinControlQuantityText");
    auto* amount = send->findChild<QLabel*>("labelCoinControlAmountText");
    auto* fee = send->findChild<QLabel*>("labelCoinControlFeeText");
    auto* change_box = send->findChild<QWidget*>("checkBoxCoinControlChange");
    auto* change_edit = send->findChild<QWidget*>("lineEditCoinControlChange");
    QVERIFY(quantity && amount && fee && change_box && change_edit);
    for (const bool coins : {false, true}) {
        if (coins) send->m_coin_control->Select(chosen); else send->m_coin_control->UnSelectAll();
        QVERIFY(QMetaObject::invokeMethod(send, "coinControlUpdateLabels"));
        Settle();
        QCOMPARE(figures->isVisible(), coins);
        for (int width : {1280, 1026, 704}) {
            // The smaller of the unlocked Overview's minimums, with and without a balance.
            int height = std::numeric_limits<int>::max();
            for (const interfaces::WalletBalances* balances : {&zero, &large}) {
                overview->setBalance(*balances); Settle();
                overview->setGeometry(0, 0, std::max(width, overview->minimumWidth()), 850); Settle();
                height = std::min(height, overview->minimumSizeHint().height());
            }
            host.resize(width, height); send->setGeometry(0, 0, width, height); Settle();
            scroll->verticalScrollBar()->setValue(0); Settle();
            const QString at = QString("%1 at %2x%3").arg(coins ? "chosen" : "automatic").arg(width).arg(height);
            qInfo().noquote() << "SEND_INPUTS" << at << "columns" << send->m_coin_figure_columns << "card" << control->height()
                              << "recipient at" << OnPage(*send, entry).top() << "address at" << OnPage(*send, pay_to).top()
                              << "viewport" << scroll->viewport()->height() << "DPR" << send->devicePixelRatioF();
            if (scroll->horizontalScrollBar()->maximum() != 0) found << at + ": the column scrolls sideways";
            if (OnPage(*send, control).bottom() >= OnPage(*send, entry).top()) found << at + ": the card is not above the recipient";
            if (inputs->visibleRegion() != QRegion(inputs->rect())) found << at + ": the Inputs button is not in view";
            // With coins chosen the address may scroll in a narrow column; with
            // two columns of figures it must not.
            if ((!coins || send->m_coin_figure_columns == 2) && pay_to->visibleRegion() != QRegion(pay_to->rect())) {
                found << at + ": the address field is not in view";
            }
            if (!coins) continue;
            // Every figure whole inside the card.
            for (QLabel* label : figures->findChildren<QLabel*>()) {
                if (!label->isVisibleTo(send)) continue;
                const QRect inside = OnPage(*send, label);
                if (!OnPage(*send, control).contains(inside) || label->width() < label->minimumSizeHint().width()) {
                    found << QString("%1: %2 is not whole in the card").arg(at, label->objectName());
                }
            }
            const QRect q = OnPage(*send, quantity), a = OnPage(*send, amount), f = OnPage(*send, fee);
            const QRect box = OnPage(*send, change_box), edit = OnPage(*send, change_edit);
            if (send->m_coin_figure_columns == 2) {
                // Amount right of Quantity on its row, Fee under Quantity in its
                // column (each group keeps the form's own margins); the change
                // address box beside its check box.
                if (std::abs(a.top() - q.top()) > 2 || a.left() <= q.right()) found << at + ": Amount is not beside Quantity";
                if (f.top() <= q.bottom() || f.left() >= a.left()) found << at + ": Fee is not under Quantity";
                if (edit.left() <= box.right() || std::abs(edit.center().y() - box.center().y()) > 4) found << at + ": the change address box is not beside its check box";
            } else if (send->m_coin_figure_columns == 1) {
                if (a.top() <= q.bottom() || f.top() <= a.bottom()) found << at + ": the groups are not one under the other";
                if (edit.top() <= box.bottom()) found << at + ": the change address box is not under its check box";
            } else {
                found << at + ": the figures are not arranged";
            }
            if (width == 1280 && send->m_coin_figure_columns != 2) found << at + ": a wide column does not take two columns";
            if (!output.isEmpty() && (width == 1280 || width == 704)) {
                QVERIFY(QDir().mkpath(output));
                QVERIFY(PageImage(*send).save(QDir(output).filePath(QString("Abyss-send-inputs-%1.png").arg(width))));
            }
        }
        if (!coins && !output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            QVERIFY(PageImage(*send).save(QDir(output).filePath("Abyss-send-inputs-automatic.png")));
        }
    }
    for (const QString& line : found) qInfo().noquote() << "SEND_INPUTS" << line;
    QCOMPARE(found.join("\n"), QString());

    // Back in the classic theme: the form's own row and the same picture.
    Theme("Light");
    host.resize(1280, 850); send->setGeometry(0, 0, 1280, 850); Settle();
    QVERIFY(classic_row());
    QCOMPARE(send->m_coin_figure_columns, 0);
    const QImage again = classic_picture();
    if (again != classic && again.size() == classic.size()) {
        // Where they differ, and what is drawn there.
        QRect differs;
        for (int y = 0; y < again.height(); ++y) {
            for (int x = 0; x < again.width(); ++x) {
                if (again.pixel(x, y) != classic.pixel(x, y)) differs |= QRect(x, y, 1, 1);
            }
        }
        const qreal dpr = again.devicePixelRatio();
        QWidget* there = send->childAt((differs.center() / dpr));
        qInfo() << "SEND_INPUTS classic picture differs in" << differs << "at" << (there ? there->objectName() : QString("nothing"))
                << (there && there->parentWidget() ? there->parentWidget()->objectName() : QString());
        if (!output.isEmpty()) {
            QVERIFY(QDir().mkpath(output));
            classic.save(QDir(output).filePath("Light-send-inputs-before.png"));
            again.save(QDir(output).filePath("Light-send-inputs-after.png"));
        }
    }
    QCOMPARE(again, classic);
}

void AbyssTests::overviewDiscreetHeader()
{
    // Discreet mode hides the recent transactions, and their card's title row
    // was then given the whole card: the title sat in its middle and the "out of
    // sync" chip ran its full height. In Abyss the row keeps to the top, as
    // with the list shown; the classic themes keep the form's layout.
    RestoreAppearance restore;
    Theme("Abyss");
    WalletFixture fixture(m_node);
    QWidget host;
    auto* page = new OverviewPage(&host);
    page->setClientModel(fixture.client.get());
    page->setWalletModel(fixture.model.get());
    host.resize(1280, 850); page->setGeometry(0, 0, 1280, 850);
    host.show(); Settle();
    page->showOutOfSyncWarning(true); Settle();
    auto* card = page->findChild<QFrame*>("frame_2");
    auto* title = page->findChild<QLabel*>("label_4");
    auto* chip = page->findChild<QLabel*>("labelTransactionsStatus");
    auto* list = page->findChild<QWidget*>("listTransactions");
    auto* layout = page->findChild<QVBoxLayout*>("verticalLayout");
    QVERIFY(card && title && chip && list && layout);
    QVERIFY(chip->isVisible());
    page->setPrivacy(false); Settle();
    const QRect title_shown = OnPage(*page, title);
    const QRect chip_shown = OnPage(*page, chip);
    page->setPrivacy(true); Settle();
    QVERIFY(!list->isVisible());
    const QRect title_hidden = OnPage(*page, title);
    const QRect chip_hidden = OnPage(*page, chip);
    qInfo() << "DISCREET_HEADER card" << OnPage(*page, card) << "title" << title_shown << title_hidden << "chip" << chip_shown << chip_hidden;
    QVERIFY2(std::abs(title_hidden.top() - title_shown.top()) <= 1 && std::abs(title_hidden.height() - title_shown.height()) <= 1,
             "the title moves when the list is hidden");
    QVERIFY2(std::abs(chip_hidden.top() - chip_shown.top()) <= 1 && std::abs(chip_hidden.height() - chip_shown.height()) <= 1,
             "the out-of-sync chip changes when the list is hidden");
    QVERIFY2(title_hidden.top() - OnPage(*page, card).top() < 40, "the title is not at the card's top");
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        QVERIFY(QDir().mkpath(output));
        QVERIFY(PageImage(*page).save(QDir(output).filePath("Abyss-overview-discreet.png")));
    }
    // The classic themes: the form's layout.
    Theme("Light"); Settle();
    QCOMPARE(layout->itemAt(0)->alignment(), Qt::Alignment());
    Theme("Abyss"); Settle();
    QCOMPARE(layout->itemAt(0)->alignment(), Qt::Alignment(Qt::AlignTop));
    page->setPrivacy(false);
}

void AbyssTests::sidebarLogoCentred()
{
    // The wordmark sits in the middle of its gap: as much room above its ink,
    // to the bar's top under the menu, as below it to the next box -- the
    // wallet selector's frame with two wallets, the first page's button with
    // one. It had 6 above and 20 below (28 with two wallets). Measured on what
    // the bar draws. The window is sized again after each wallet: the minimal
    // platform's small screen would otherwise fold the bar under the wordmark.
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    NodeContext& context = *m_node.context();
    auto second = std::make_shared<CWallet>(context.chain.get(), context.coinjoin_loader.get(), "second", CreateMockWalletDatabase());
    AddWallet(second);
    second->LoadWallet();
    second->GetOrCreateLegacyScriptPubKeyMan();
    const auto forget = interfaces::MakeHandler([&] { RemoveWallet(second, std::nullopt); });
    WalletModel second_model(interfaces::MakeWallet(second), *fixture.client);
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850);
    window.show(); Settle();
    window.addWallet(fixture.model.get()); Settle();
    window.gotoOverviewPage();
    window.resize(1280, 850); Settle();
    auto* bar = window.findChild<QToolBar*>("navigationToolBar");
    QVERIFY(bar);
    auto* logo = bar->findChild<QLabel*>("lblToolbarLogo");
    QVERIFY(logo);
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const auto gaps = [&](const char* what) {
        const QImage shot = bar->grab().toImage().convertToFormat(QImage::Format_RGB32);
        const qreal dpr = shot.devicePixelRatio();
        if (!output.isEmpty()) {
            QDir().mkpath(output);
            shot.save(QDir(output).filePath(QString("Abyss-sidebar-logo-%1.png").arg(what)));
        }
        // The bar's own colour: its top-left corner is padding.
        const QRgb backdrop = shot.pixel(2, 2);
        const auto drawn = [&](int x, int y) {
            const QRgb p = shot.pixel(x, y);
            return std::abs(qRed(p) - qRed(backdrop)) + std::abs(qGreen(p) - qGreen(backdrop)) + std::abs(qBlue(p) - qBlue(backdrop)) > 24;
        };
        const QRect label(logo->mapTo(bar, QPoint()) * dpr, logo->size() * dpr);
        int top = -1, bottom = -1;
        for (int y = std::max(0, label.top()); y <= std::min(shot.height() - 1, label.bottom()); ++y) {
            for (int x = std::max(0, label.left()); x <= std::min(shot.width() - 1, label.right()); ++x) {
                if (drawn(x, y)) {
                    if (top < 0) top = y;
                    bottom = y;
                    break;
                }
            }
        }
        // The next box: the first row under the ink drawn across the bar's middle.
        int next = -1;
        const int middle = shot.width() / 2;
        for (int y = bottom + 1; y < shot.height() && next < 0; ++y) {
            for (int x = middle - 40; x <= middle + 40; ++x) {
                if (drawn(x, y)) { next = y; break; }
            }
        }
        const qreal above = top / dpr;
        const qreal below = (next - bottom - 1) / dpr;
        qInfo() << "SIDEBAR_LOGO" << what << "ink" << top << bottom << "next" << next << "above" << above << "below" << below << "DPR" << dpr;
        return std::make_tuple(top >= 0 && next > bottom, above, below);
    };
    for (const char* what : {"one", "two"}) {
        if (QString(what) == "two") {
            window.addWallet(&second_model); Settle();
            window.resize(1280, 850); Settle();
        }
        const auto [found, above, below] = gaps(what);
        QVERIFY2(found, qPrintable(QString("%1 wallet(s): the wordmark or the box under it draws nothing").arg(what)));
        QVERIFY2(std::abs(above - below) <= 2,
                 qPrintable(QString("%1 wallet(s): %2 px above the wordmark, %3 px below").arg(what).arg(above).arg(below)));
    }
    window.removeWallet(&second_model); Settle();
}

namespace {
QKeySequence PageKey(int n)
{
#ifdef Q_OS_MAC
    return QKeySequence(QString("Ctrl+%1").arg(n));
#else
    return QKeySequence(QString("Alt+%1").arg(n));
#endif
}
//! What the window does when a QShortcut's key is pressed, without depending
//! on the platform delivering a key to an active window.
void PressShortcut(QShortcut* shortcut)
{
    QShortcutEvent press(shortcut->key(), shortcut->id());
    QCoreApplication::sendEvent(shortcut, &press);
    Settle();
}
} // namespace

void AbyssTests::sidebarMoreGroup()
{
    // Abyss folds CoinJoin, Multisig, Masternodes and Governance under one
    // "More" header right under Transactions. It starts closed, a click opens
    // and closes it, QSettings keeps the choice, a page reached while it is
    // closed opens it, and a folded page keeps its Alt+N. Without CoinJoin its
    // page is neither shown nor numbered. The classic themes have no header.
    RestoreAppearance restore;
    QSettings settings;
    std::map<QString, QVariant> saved;
    for (const char* key : {"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab", "fSidebarMoreOpen"}) {
        saved.emplace(key, settings.value(key));
    }
    const auto restore_keys = interfaces::MakeHandler([saved] {
        QSettings settings;
        for (const auto& [key, value] : saved) {
            if (value.isValid()) settings.setValue(key, value); else settings.remove(key);
        }
    });
    for (const char* tab : {"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab"}) settings.setValue(tab, true);
    settings.remove("fSidebarMoreOpen");
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850);
    window.show(); Settle();
    window.addWallet(fixture.model.get()); Settle();
    window.gotoOverviewPage();
    window.resize(1280, 850); Settle();
    // CoinJoin's page follows the node's option; the test sets it both ways.
    window.m_coinjoin_tab_enabled = true;
    window.updateMoreGroup(); Settle();

    auto* bar = window.findChild<QToolBar*>("navigationToolBar");
    auto* more = window.findChild<QToolButton*>("abyssMoreButton");
    QVERIFY(bar && more);
    QVERIFY(!more->group()); // Not a page: never the checked one, no Alt+N of its own.
    const QList<QToolButton*> top{window.overviewButton, window.sendCoinsButton, window.receiveCoinsButton, window.historyButton};
    const QList<QToolButton*> pages{window.coinJoinCoinsButton, window.multisigButton, window.masternodeButton, window.governanceButton};
    for (QToolButton* page : pages) QVERIFY(page);
    QVERIFY(window.moreGroupPages() == pages);
    const auto folded = [&window](QToolButton* page) { return window.m_folded_shortcuts.value(page); };
    for (QToolButton* page : pages) QVERIFY(folded(page));
    const auto view_shows = [&window](const char* type) {
        QWidget* current = window.walletFrame->currentWalletView()->currentWidget();
        return current && current->inherits(type);
    };
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const auto save = [&](const char* name) {
        return output.isEmpty() || (QDir().mkpath(output) && bar->grab().save(QDir(output).filePath(name)));
    };
    const auto arrow = [&] { return more->icon().pixmap(window.windowHandle(), more->iconSize()).toImage(); };

    // Closed at first: the header right under Transactions, its pages hidden;
    // Alt+1 to Alt+4 on the pages in the bar, Alt+5 on through the window.
    QVERIFY(!window.m_more_open);
    QVERIFY(more->isVisible());
    QVERIFY(OnPage(window, more).top() > OnPage(window, window.historyButton).bottom());
    for (int i = 0; i < top.size(); ++i) {
        QVERIFY(top[i]->isVisible());
        QCOMPARE(top[i]->shortcut(), PageKey(i + 1));
    }
    for (int i = 0; i < pages.size(); ++i) {
        QVERIFY2(pages[i]->isHidden(), qPrintable(pages[i]->text()));
        QVERIFY(pages[i]->shortcut().isEmpty());
        QVERIFY(folded(pages[i])->isEnabled());
        QCOMPARE(folded(pages[i])->key(), PageKey(5 + i));
    }
    const QImage closed_arrow = arrow();
    QVERIFY(!closed_arrow.isNull());
    QVERIFY(save("Abyss-sidebar-more-closed.png"));

    // Open: the four pages under the header, in order, with their own Alt+N.
    more->click(); Settle();
    QVERIFY(window.m_more_open);
    QVERIFY(QSettings().contains("fSidebarMoreOpen"));
    QCOMPARE(QSettings().value("fSidebarMoreOpen").toBool(), true);
    QVERIFY2(arrow() != closed_arrow, "the arrow does not say the header is open");
    int above = OnPage(window, more).bottom();
    for (int i = 0; i < pages.size(); ++i) {
        QVERIFY2(pages[i]->isVisible(), qPrintable(pages[i]->text()));
        QVERIFY(OnPage(window, pages[i]).top() > above);
        above = OnPage(window, pages[i]).bottom();
        QCOMPARE(pages[i]->shortcut(), PageKey(5 + i));
        QVERIFY(!folded(pages[i])->isEnabled());
    }
    QVERIFY(save("Abyss-sidebar-more-open.png"));

    // Closed again by a click, and that is remembered too.
    more->click(); Settle();
    QVERIFY(!window.m_more_open);
    QVERIFY(QSettings().contains("fSidebarMoreOpen"));
    QCOMPARE(QSettings().value("fSidebarMoreOpen").toBool(), false);
    for (QToolButton* page : pages) QVERIFY(page->isHidden());
    QCOMPARE(arrow(), closed_arrow);

    // A folded page's Alt+N opens the header and the page.
    PressShortcut(folded(window.masternodeButton));
    QVERIFY(window.m_more_open);
    QVERIFY(window.masternodeButton->isChecked());
    QVERIFY(window.masternodeButton->isVisible());
    QVERIFY(view_shows("MasternodeList"));

    // Closing it on such a page is allowed; the header then says, in bold,
    // that it holds the page on display. Alt+N still opens it, although the
    // page is already the current one.
    more->click(); Settle();
    QVERIFY(!window.m_more_open);
    QVERIFY(window.masternodeButton->isHidden());
    QVERIFY(more->property("currentInside").toBool());
    // GUIUtil sets fonts only once they are loaded, in the weights it is set to.
    if (GUIUtil::fontsLoaded()) QCOMPARE(more->font().weight(), int(GUIUtil::getFontWeightBold()));
    QVERIFY(save("Abyss-sidebar-more-current.png"));
    // A theme switch polishes the bar again, which puts back each widget's first
    // font; the header keeps its weight through it. The minimal platform's small
    // screen folds the bar after a switch, so the window is sized again.
    Theme("Light"); Settle();
    Theme("Abyss"); Settle();
    window.resize(1280, 850); Settle();
    QVERIFY(window.masternodeButton->isHidden() && window.masternodeButton->isChecked());
    QVERIFY(more->property("currentInside").toBool());
    if (GUIUtil::fontsLoaded()) QCOMPARE(more->font().weight(), int(GUIUtil::getFontWeightBold()));
    PressShortcut(folded(window.masternodeButton));
    QVERIFY(window.m_more_open);
    QVERIFY(window.masternodeButton->isVisible());
    QVERIFY(!more->property("currentInside").toBool());
    if (GUIUtil::fontsLoaded()) QCOMPARE(more->font().weight(), int(GUIUtil::getFontWeightNormal()));

    // A page reached another way -- the slot its sidebar button or menu entry calls -- opens it as well.
    window.gotoOverviewPage(); Settle();
    more->click(); Settle();
    QVERIFY(!window.m_more_open);
    QVERIFY(!more->property("currentInside").toBool());
    window.gotoGovernancePage(); Settle();
    QVERIFY(window.m_more_open);
    QVERIFY(window.governanceButton->isChecked() && window.governanceButton->isVisible());
    QVERIFY(view_shows("GovernanceList"));

    // Astra r1 R1: the current page, folded away by closing the header on it,
    // chosen again from its menu entry (the same slot, the checked page does not
    // change) shows again.
    window.gotoMultisigPage(); Settle();
    more->click(); Settle();
    QVERIFY(!window.m_more_open && window.multisigButton->isHidden());
    QVERIFY(more->property("currentInside").toBool());
    window.gotoMultisigPage(); Settle();
    QVERIFY(window.m_more_open);
    QVERIFY(window.multisigButton->isVisible() && window.multisigButton->isChecked());
    QVERIFY(!more->property("currentInside").toBool());
    QVERIFY(view_shows("MultisigPage"));
    // Astra r1 R1: a page chosen in a classic theme, with the header closed on
    // another page, is not folded away when Abyss comes back.
    window.gotoOverviewPage(); Settle();
    more->click(); Settle();
    QVERIFY(!window.m_more_open);
    Theme("Light"); Settle();
    QVERIFY(window.masternodeButton->isVisible());
    window.masternodeButton->click(); Settle();
    Theme("Abyss"); Settle();
    window.resize(1280, 850); Settle();
    QVERIFY(window.m_more_open);
    QVERIFY(window.masternodeButton->isVisible() && window.masternodeButton->isChecked());
    QVERIFY(!more->property("currentInside").toBool());
    QVERIFY(view_shows("MasternodeList"));
    window.gotoOverviewPage(); Settle();

    // Without CoinJoin its page is neither shown nor numbered, folded or not.
    window.m_coinjoin_tab_enabled = false;
    window.updateMoreGroup(); Settle();
    QVERIFY(window.coinJoinCoinsButton->isHidden());
    for (int i = 1; i < pages.size(); ++i) QCOMPARE(pages[i]->shortcut(), PageKey(4 + i));
    more->click(); Settle();
    QVERIFY(!folded(window.coinJoinCoinsButton)->isEnabled());
    QVERIFY(folded(window.coinJoinCoinsButton)->key().isEmpty());
    for (int i = 1; i < pages.size(); ++i) QCOMPARE(folded(pages[i])->key(), PageKey(4 + i));
    window.m_coinjoin_tab_enabled = true;
    window.updateMoreGroup(); Settle();
    QCOMPARE(folded(window.coinJoinCoinsButton)->key(), PageKey(5));

    // The classic themes: no header, every page in the bar, numbered in order,
    // whatever the header's state.
    for (const char* classic : {"Light", "Dark", "Traditional"}) {
        Theme(classic); Settle();
        QVERIFY2(more->isHidden(), classic);
        int n = 1;
        for (QToolButton* page : top + pages) {
            QVERIFY2(page->isVisible(), qPrintable(QString("%1: %2").arg(classic, page->text())));
            QCOMPARE(page->shortcut(), PageKey(n++));
        }
        for (QToolButton* page : pages) QVERIFY(!folded(page)->isEnabled());
    }
    Theme("Abyss"); Settle();
    window.resize(1280, 850); Settle();
    QVERIFY(more->isVisible());
    QVERIFY(!window.m_more_open);
    for (QToolButton* page : pages) QVERIFY(page->isHidden());

    // The choice outlives the window. With no page to fold there is no header.
    more->click(); Settle();
    for (const char* tab : {"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab"}) QSettings().setValue(tab, false);
    {
        BitcoinGUI reopened(m_node, style.get());
        QVERIFY(reopened.m_more_open);
        QVERIFY(reopened.moreGroupPages() == QList<QToolButton*>{reopened.coinJoinCoinsButton});
        QVERIFY(reopened.moreToolbarAction->isVisible());
        reopened.m_coinjoin_tab_enabled = false;
        reopened.updateMoreGroup();
        QVERIFY(!reopened.moreToolbarAction->isVisible());
    }
}

void AbyssTests::sidebarMoreWalletGate()
{
    // Astra r2 R3: a page that needs a wallet (Multisig; CoinJoin, which needs
    // CoinJoin on as well) stays disabled without one, however it comes into
    // view -- the More header opening, a classic theme -- and its folded
    // shortcut does not fire, as a disabled button's own does not. Showing a
    // hidden toolbar action enables it, and its button, unless the action was
    // disabled itself. Masternodes and Governance need no wallet. Checked
    // before the first wallet, with it (the positive control), and after the
    // last one is closed.
    RestoreAppearance restore;
    QSettings settings;
    std::map<QString, QVariant> saved;
    for (const char* key : {"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab", "fSidebarMoreOpen"}) {
        saved.emplace(key, settings.value(key));
    }
    const auto restore_keys = interfaces::MakeHandler([saved] {
        QSettings settings;
        for (const auto& [key, value] : saved) {
            if (value.isValid()) settings.setValue(key, value); else settings.remove(key);
        }
    });
    for (const char* tab : {"fShowMultisigTab", "fShowMasternodesTab", "fShowGovernanceTab"}) settings.setValue(tab, true);
    settings.remove("fSidebarMoreOpen");
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850);
    window.show(); Settle();
    window.resize(1280, 850); Settle();
    // CoinJoin through the node's own option, as the Options dialog sets it.
    interfaces::CoinJoin::Options& coinjoin_option = fixture.client->coinJoinOptions();
    const bool coinjoin_was = coinjoin_option.isEnabled();
    const auto restore_coinjoin = interfaces::MakeHandler([&coinjoin_option, coinjoin_was] { coinjoin_option.setEnabled(coinjoin_was); });
    const auto set_coinjoin = [&](bool on) {
        coinjoin_option.setEnabled(on);
        window.updateCoinJoinVisibility(); Settle();
        QCOMPARE(window.m_coinjoin_tab_enabled, on);
    };
    set_coinjoin(true);

    auto* more = window.findChild<QToolButton*>("abyssMoreButton");
    QVERIFY(more);
    QToolButton* multisig = window.multisigButton;
    QToolButton* coinjoin = window.coinJoinCoinsButton;
    QVERIFY(multisig && coinjoin && window.masternodeButton && window.governanceButton);
    const auto folded = [&window](QToolButton* page) { return window.m_folded_shortcuts.value(page); };
    // What a wallet decides, page by page; the rest of the bar as the base has it.
    const auto gate = [&](bool wallet, const QString& when) {
        QVERIFY2(multisig->isEnabled() == wallet, qPrintable(when + ": Multisig"));
        QVERIFY2(coinjoin->isEnabled() == wallet, qPrintable(when + ": CoinJoin"));
        QVERIFY2(window.masternodeButton->isEnabled(), qPrintable(when + ": Masternodes"));
        QVERIFY2(window.governanceButton->isEnabled(), qPrintable(when + ": Governance"));
        QVERIFY2(window.sendCoinsButton->isEnabled() == wallet, qPrintable(when + ": Send"));
    };
    // The folded shortcuts, with the header closed: numbered as ever, live only where the page is usable.
    const auto folded_live = [&](bool wallet, const QString& when) {
        QVERIFY2(!window.m_more_open && multisig->isHidden(), qPrintable(when));
        QVERIFY2(!folded(multisig)->key().isEmpty(), qPrintable(when + ": Multisig keeps its number"));
        QVERIFY2(folded(multisig)->isEnabled() == wallet, qPrintable(when + ": Multisig's folded shortcut"));
        QVERIFY2(folded(coinjoin)->isEnabled() == wallet, qPrintable(when + ": CoinJoin's folded shortcut"));
        QVERIFY2(folded(window.masternodeButton)->isEnabled(), qPrintable(when + ": Masternodes' folded shortcut"));
        QVERIFY2(folded(window.governanceButton)->isEnabled(), qPrintable(when + ": Governance's folded shortcut"));
    };

    // Before the first wallet: the header opens over disabled wallet pages,
    // in Abyss and in a classic theme, and closes again with dead shortcuts.
    QVERIFY(!window.m_wallet_actions_enabled);
    folded_live(false, "no wallet, closed");
    more->click(); Settle();
    QVERIFY(window.m_more_open && multisig->isVisible() && coinjoin->isVisible());
    gate(false, "no wallet, header opened");
    more->click(); Settle();
    folded_live(false, "no wallet, closed again");
    Theme("Light"); Settle();
    QVERIFY(multisig->isVisible());
    gate(false, "no wallet, Light");
    Theme("Abyss"); Settle();
    window.resize(1280, 850); Settle();
    folded_live(false, "no wallet, back in Abyss");
    more->click(); Settle();
    gate(false, "no wallet, opened after a theme switch");
    more->click(); Settle();

    // The positive control: with a wallet, the same ways bring the pages
    // enabled. Astra r3 R5: the first wallet loads with CoinJoin off, and
    // CoinJoin turned on afterwards brings its page enabled too.
    set_coinjoin(false);
    window.addWallet(fixture.model.get()); Settle();
    window.resize(1280, 850); Settle();
    QVERIFY(window.m_wallet_actions_enabled);
    QVERIFY(!coinjoin->isEnabled());
    QVERIFY(!folded(coinjoin)->isEnabled() && folded(coinjoin)->key().isEmpty());
    set_coinjoin(true);
    folded_live(true, "a wallet, CoinJoin turned on, closed");
    more->click(); Settle();
    gate(true, "a wallet, header opened");
    more->click(); Settle();
    Theme("Light"); Settle();
    gate(true, "a wallet, Light");
    Theme("Abyss"); Settle();
    window.resize(1280, 850); Settle();
    folded_live(true, "a wallet, back in Abyss");

    // The last wallet closed: disabled again, through every way into view.
    window.removeWallet(fixture.model.get()); Settle();
    window.resize(1280, 850); Settle();
    QVERIFY(!window.m_wallet_actions_enabled);
    folded_live(false, "last wallet closed");
    more->click(); Settle();
    gate(false, "last wallet closed, header opened");
    more->click(); Settle();
    Theme("Light"); Settle();
    gate(false, "last wallet closed, Light");
    Theme("Abyss"); Settle();
}

void AbyssTests::quickButtonsExpand()
{
    // In Abyss the four quick buttons say their name while pointed at or
    // reached with Tab. They open to the left, so the one under the pointer
    // keeps its right edge, and the row stays clear of the menus; they close
    // when the pointer and the keyboard have left. Focus from a click opens
    // nothing. The padlock of a locked wallet stays open on "Unlock Wallet";
    // a classic theme's padlock never opens.
    const bool high_dpi = QCoreApplication::testAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    const auto restore_dpi = interfaces::MakeHandler([high_dpi] { QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, high_dpi); });
    if (!GUIUtil::fontsLoaded()) QSKIP("Quick-button widths require minimal:enable_fonts or a native platform");
    RestoreAppearance restore;
    const QString app_name = QCoreApplication::applicationName();
    std::unique_ptr<const NetworkStyle> style(NetworkStyle::instantiate(Params().NetworkIDString()));
    QCoreApplication::setApplicationName(app_name);
    QVERIFY(style);
    Theme("Abyss");
    WalletFixture fixture(m_node);
    BitcoinGUI window(m_node, style.get());
    interfaces::BlockAndHeaderTipInfo tip{};
    window.setClientModel(fixture.client.get(), &tip);
    const auto detach = interfaces::MakeHandler([&window] { window.setClientModel(nullptr); });
    window.resize(1280, 850);
    window.show(); Settle();
    auto* row = window.findChild<QWidget*>("abyssQuickControls");
    auto* lock = window.findChild<QToolButton*>("walletLockButton");
    auto* selector = window.findChild<QWidget*>("walletSelector");
    QVERIFY(row && lock && selector);
    QMenuBar* menus = window.menuBar();
    const auto send = [](QWidget* widget, QEvent::Type type) {
        QEvent event(type);
        QCoreApplication::sendEvent(widget, &event);
        Settle();
    };
    const auto focus = [](QWidget* widget, QEvent::Type type, Qt::FocusReason reason) {
        QFocusEvent event(type, reason);
        QCoreApplication::sendEvent(widget, &event);
        Settle();
    };
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    const auto save = [&](const QString& name) {
        const QRect top(0, 0, window.width(), OnPage(window, row).bottom() + 6);
        return output.isEmpty() || (QDir().mkpath(output) && window.grab(top).save(QDir(output).filePath(name + ".png")));
    };
    // The row inside the window, clear of the menus, every button whole: as wide
    // as it asks to be, or as its stylesheet cap allows (a closed button's 40 px).
    const auto fits = [&](const QString& what) {
        const QRect place = OnPage(window, row);
        QVERIFY2(place.left() >= 0 && place.right() < window.width(), qPrintable(what));
        if (!window.abyssQuickControlsBar) {
            for (QAction* action : menus->actions()) {
                if (action->isVisible()) QVERIFY2(!row->geometry().intersects(menus->actionGeometry(action)), qPrintable(what));
            }
            QWidget* left = menus->cornerWidget(Qt::TopLeftCorner);
            if (left && left->isVisible()) QVERIFY2(!row->geometry().intersects(left->geometry()), qPrintable(what));
        }
        for (QToolButton* button : row->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!button->isVisible()) continue;
            QVERIFY2(row->rect().contains(button->geometry()), qPrintable(what + ": " + button->objectName()));
            const int whole = std::min(button->sizeHint().width(), button->maximumWidth());
            QVERIFY2(button->width() >= whole,
                     qPrintable(QString("%1: %2 squeezed to %3 of %4").arg(what, button->objectName()).arg(button->width()).arg(whole)));
        }
    };

    // Without a wallet the padlock opens on what it says.
    send(lock, QEvent::Enter);
    QCOMPARE(lock->text(), QString("No wallet loaded"));
    send(lock, QEvent::Leave);
    QVERIFY(lock->text().isEmpty());
    window.addWallet(fixture.model.get()); Settle();
    window.resize(1280, 850); Settle();

    const std::vector<std::pair<const char*, QString>> names{
        {"abyssOptionsButton", "Options"}, {"abyssPrivacyButton", "Discreet mode"},
        {"walletLockButton", "Encrypt Wallet"}, {"abyssConsoleButton", "Debug console"}};
    for (int width : {1280, 958}) {
        window.resize(width, 850); Settle();
        for (const auto& [object, name] : names) {
            auto* button = row->findChild<QToolButton*>(object);
            QVERIFY(button && button->isVisible());
            const QString what = QString("%1 at %2 px").arg(object).arg(width);
            const QRect closed = OnPage(window, button);
            QVERIFY2(closed.width() <= 44, qPrintable(what));
            QVERIFY(button->text().isEmpty());
            send(button, QEvent::Enter);
            QCOMPARE(button->text(), name);
            QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
            const QRect open = OnPage(window, button);
            QVERIFY2(open.width() >= closed.width() + 30, qPrintable(what));
            QVERIFY2(std::abs(open.right() - closed.right()) <= 1, qPrintable(what + ": the button under the pointer moved"));
            QCOMPARE(open.top(), closed.top());
            fits(what);
            if (width == 1280) QVERIFY(save(QString("Abyss-quick-open-%1").arg(object)));
            send(button, QEvent::Leave);
            QVERIFY(button->text().isEmpty());
            QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
            QCOMPARE(OnPage(window, button), closed);
        }
    }
    window.resize(1280, 850); Settle();

    // The keyboard opens one with Tab; the pointer passing over does not close
    // it then, and focus from a click opens nothing.
    auto* options = row->findChild<QToolButton*>("abyssOptionsButton");
    auto* console = row->findChild<QToolButton*>("abyssConsoleButton");
    focus(options, QEvent::FocusIn, Qt::TabFocusReason);
    QCOMPARE(options->text(), QString("Options"));
    send(options, QEvent::Enter);
    send(options, QEvent::Leave);
    QCOMPARE(options->text(), QString("Options"));
    focus(options, QEvent::FocusOut, Qt::TabFocusReason);
    QVERIFY(options->text().isEmpty());
    focus(console, QEvent::FocusIn, Qt::BacktabFocusReason);
    QCOMPARE(console->text(), QString("Debug console"));
    focus(console, QEvent::FocusOut, Qt::MouseFocusReason);
    for (Qt::FocusReason reason : {Qt::MouseFocusReason, Qt::ActiveWindowFocusReason, Qt::PopupFocusReason, Qt::OtherFocusReason}) {
        focus(console, QEvent::FocusIn, reason);
        QVERIFY2(console->text().isEmpty(), qPrintable(QString("focus reason %1").arg(static_cast<int>(reason))));
        focus(console, QEvent::FocusOut, reason);
    }

    // A locked wallet keeps the padlock open on the way to unlock it, through
    // the pointer passing, at the narrowest window too, with another button open.
    window.setEncryptionStatus(WalletModel::Locked); Settle();
    QCOMPARE(lock->text(), QString("Unlock Wallet"));
    QVERIFY(lock->property("expanded").toBool());
    fits("locked");
    send(lock, QEvent::Enter);
    send(lock, QEvent::Leave);
    QCOMPARE(lock->text(), QString("Unlock Wallet"));
    QVERIFY(save("Abyss-quick-locked"));
    window.resize(958, 850); Settle();
    send(console, QEvent::Enter);
    fits("locked, console open, 958 px");
    QVERIFY(save("Abyss-quick-locked-console-958"));
    send(console, QEvent::Leave);
    window.resize(1280, 850); Settle();
    window.setEncryptionStatus(WalletModel::Unlocked); Settle();
    QVERIFY(lock->text().isEmpty());
    send(lock, QEvent::Enter);
    QCOMPARE(lock->text(), QString("Lock Wallet"));
    send(lock, QEvent::Leave);
    QVERIFY(lock->text().isEmpty());

    // A classic theme takes the padlock back into the wallet selector, closed
    // and without a name, even while the wallet is locked. A theme switch reads
    // the fixture's real, unencrypted state again, so it is locked once more.
    for (const char* classic : {"Light", "Dark", "Traditional"}) {
        Theme(classic); Settle();
        window.setEncryptionStatus(WalletModel::Locked); Settle();
        QCOMPARE(lock->parentWidget(), selector);
        QVERIFY2(lock->text().isEmpty(), classic);
        QCOMPARE(lock->toolButtonStyle(), Qt::ToolButtonIconOnly);
        QVERIFY(!lock->property("expanded").toBool());
        send(lock, QEvent::Enter);
        QVERIFY2(lock->text().isEmpty(), classic);
        send(lock, QEvent::Leave);
    }
    Theme("Abyss"); Settle();
    QCOMPARE(lock->parentWidget(), row);
    window.setEncryptionStatus(WalletModel::Locked); Settle();
    QCOMPARE(lock->text(), QString("Unlock Wallet"));
    window.setEncryptionStatus(fixture.model->getEncryptionStatus()); Settle();
    QVERIFY(lock->text().isEmpty());
}
