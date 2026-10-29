// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.
#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif
#include <qt/test/abysstests.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <qt/addressbookpage.h>
#include <qt/addresstablemodel.h>
#include <qt/appearancewidget.h>
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
#include <test/util/setup_common.h>
#include <util/settings.h>
#include <util/system.h>
#include <univalue.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
#include <QApplication>
#include <QAction>
#include <QSignalSpy>
#include <QToolButton>
#include <QToolBar>
#include <QWindow>
#if defined(Q_OS_LINUX)
#include <time.h>
#endif
#include <QCalendarWidget>
#include <QDateTimeEdit>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFontMetricsF>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QLineEdit>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStyleOptionButton>
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
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <map>
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
    explicit WalletFixture(interfaces::Node& n) : node(n), previous(n.context())
    {
        node.setContext(&chain.m_node);
        wallet = std::make_shared<CWallet>(node.context()->chain.get(), node.context()->coinjoin_loader.get(), "", CreateMockWalletDatabase());
        AddWallet(wallet);
        wallet->LoadWallet();
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
    // With CoinJoin on, its card sits under the balances one with the same gap
    // the network card leaves above them.
    auto* coinjoin = page.findChild<QFrame*>("frameCoinJoin");
    QVERIFY(coinjoin);
    coinjoin->show();
    Settle();
    QCOMPARE(OnPage(page, coinjoin).top() - OnPage(page, left).bottom() - 1,
             OnPage(page, left).top() - OnPage(page, network).bottom() - 1);
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
        {"ReceiveCoinsDialog", "frame2", "clearButton"}, {"SignVerifyMessageDialog", "", "clearButton_SM"},
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
        {"RPCConsole", {"Info", "Console", "NetTraffic", "Peers", "Repair"}},
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
    QCOMPARE(part("networkAge", "networkText")->text(), OverviewPage::tr("%1 ago").arg(GUIUtil::formatNiceTimeOffset(2 * 60)));
    QCOMPARE(part("networkPeers", "networkValue")->text(), QString("20"));
    QCOMPARE(part("networkPeers", "networkText")->text(), OverviewPage::tr("peers"));
    QCOMPARE(part("networkMasternodes", "networkValue")->text(), QString("312"));
    QCOMPARE(part("networkMempool", "networkValue")->text(), QString("14"));
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
    QCOMPARE(part("networkAge", "networkText")->text(), OverviewPage::tr("%1 behind").arg(GUIUtil::formatNiceTimeOffset(3 * 24 * 60 * 60)));
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
    QCOMPARE(part("networkSyncing", "networkText")->text(), OverviewPage::tr("Syncing %1%").arg(40));
    QCOMPARE(part("networkAge", "networkText")->text(), OverviewPage::tr("%1 ago").arg(GUIUtil::formatNiceTimeOffset(2 * 60)));
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

    // Receive: the empty payment history points at the form beside it.
    {
        Theme("Abyss");
        ReceiveCoinsDialog receive;
        receive.setModel(fixture.model.get());
        receive.resize(1280, 820); receive.show(); Settle();
        auto* empty = receive.findChild<EmptyStateWidget*>("emptyState");
        auto* label = receive.findChild<QLineEdit*>("reqLabel");
        QVERIFY(empty && label);
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        QCOMPARE(empty->actionButton()->text(), ReceiveCoinsDialog::tr("Create a request"));
        QVERIFY(fits(empty));
        QVERIFY(capture(receive, "receive"));
        empty->actionButton()->click();
        QCOMPARE(receive.focusWidget(), label);
        RecentRequestsTableModel* requests = fixture.model->getRecentRequestsTableModel();
        requests->addNewRequest(SendCoinsRecipient(address, "Sample", 100000000, ""));
        QCOMPARE(requests->rowCount({}), 1);
        QCOMPARE(empty->state(), EmptyStateWidget::State::Hidden);
        QVERIFY(requests->removeRows(0, 1));
        QCOMPARE(empty->state(), EmptyStateWidget::State::Empty);
        Theme("Light"); Settle();
        QVERIFY(!empty->isVisible());
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
protected:
    qint64 clockMilliseconds() const override { return elapsed; }
};
} // namespace


void AbyssTests::skySuspendClock()
{
    StarfieldWidget live;
#if defined(Q_OS_LINUX) && defined(CLOCK_BOOTTIME)
    // Check the actual production provider, not only a fake elapsed timer.
    timespec boot{};
    QVERIFY(clock_gettime(CLOCK_BOOTTIME, &boot) == 0);
    const qint64 boot_ms = static_cast<qint64>(boot.tv_sec) * 1000 + boot.tv_nsec / 1000000;
    QVERIFY(std::abs(StarfieldWidget::suspendAwareClockMilliseconds() - boot_ms) < 100);
#endif
    const qint64 before = live.clockMilliseconds();
    QTest::qWait(50);
    QVERIFY(live.clockMilliseconds() - before >= 20);
    // A sleep-aware clock advances while GUI timers are stopped. Simulate an
    // eight-hour suspend, without suspending the shared test machine.
    ControlledSky sky;
    sky.m_epoch_utc_ms = 1792173600000LL;
    sky.resize(1280, 720);
    sky.setSkyVisible(true);
    sky.show();
    QApplication::setActiveWindow(&sky);
    Settle();
    sky.redrawSteadySky();
    sky.onTick();
    const qreal angle = sky.m_angle;
    sky.elapsed += 8 * 3600000LL;
    sky.onTick();
    constexpr qreal tau = 6.283185307179586;
    QVERIFY(std::abs(std::remainder(sky.m_angle - angle, tau) - tau * 8 * 3600 / 86164.09) < 1e-10);
    QCOMPARE(sky.phaseTimeMilliseconds(), sky.m_epoch_utc_ms + sky.elapsed);
    sky.setAnimated(false);
    const qint64 frozen = sky.motionMilliseconds();
    const qint64 phase = sky.phaseTimeMilliseconds();
    sky.elapsed += 8 * 3600000LL;
    QCOMPARE(sky.motionMilliseconds(), frozen);
    QCOMPARE(sky.phaseTimeMilliseconds(), phase);
    sky.setAnimated(true);
    QCOMPARE(sky.motionMilliseconds(), frozen);
    QCOMPARE(sky.phaseTimeMilliseconds(), sky.m_epoch_utc_ms + sky.elapsed);
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
                             "network-off", "leaf", "leaf-off", "clock-pause"}) {
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

void AbyssTests::skyGeometry()
{
    constexpr qreal tau = 6.283185307179586;
    const qint64 epoch = 1792173600000LL;
    const qreal angle = StarfieldWidget::rotationAngle(epoch);
    const qreal minute = std::remainder(StarfieldWidget::rotationAngle(epoch + 60000) - angle, tau);
    QVERIFY(std::abs(minute - tau * 60 / 86164.09) < 1e-10);
    QVERIFY(std::abs(minute * 180 / (tau / 2) - 0.2506844789) < 1e-8);
    ControlledSky sky;
    sky.m_epoch_utc_ms = epoch;
    sky.resize(1280, 720);
    QVERIFY(sky.m_stars.isEmpty()); // A classic theme starts without a catalogue.
    sky.setSkyVisible(true);
    const auto catalogue = sky.m_stars;
    sky.setSkyVisible(false);
    QVERIFY(sky.m_stars.isEmpty());
    QVERIFY(sky.m_steady.isNull() && sky.m_moon.isNull());
    sky.setSkyVisible(true);
    QCOMPARE(sky.m_stars.size(), catalogue.size());
    for (int i = 0; i < catalogue.size(); ++i) {
        QCOMPARE(sky.m_stars[i].pos, catalogue[i].pos);
        QCOMPARE(sky.m_stars[i].colour, catalogue[i].colour);
    }
    for (const QSize size : {QSize(958, 600), QSize(1280, 720), QSize(5120, 2880), QSize(7680, 4320)}) {
        sky.resize(size);
        // Hidden widgets may receive their resize event only when shown.
        sky.buildSky();
        QVERIFY(sky.pole().y() < 0);
        for (int turn = 0; turn < 12; ++turn) {
            sky.setRotationAngle(tau * turn / 12);
            int quadrants[4]{};
            for (const auto& star : sky.m_stars) {
                const QPointF p = sky.widgetPos(star);
                if (sky.rect().contains(p.toPoint())) ++quadrants[(p.x() >= size.width() / 2 ? 1 : 0) + (p.y() >= size.height() / 2 ? 2 : 0)];
            }
            for (int count : quadrants) QVERIFY(count >= 5);
        }
    }
    sky.resize(1280, 720);
    sky.setRotationAngle(0);
    StarfieldWidget::Star star{};
    star.pos = QPointF(100, 0);
    QCOMPARE(sky.widgetPos(star), sky.pole() + QPointF(100, 0));
    sky.setRotationAngle(minute);
    const QPointF p = sky.widgetPos(star) - sky.pole();
    QVERIFY(p.y() < 0); // Counterclockwise in the downward-pointing screen axis.
    QVERIFY(std::abs(std::hypot(p.x(), p.y()) - 100) < 1e-10);
    const QPointF moon = sky.moonPosition(0);
    QCOMPARE(moon, QPointF(1280 * 0.78, 720 * 0.09));
    sky.m_moon_position = moon;
    QVERIFY(sky.moonRect().contains((moon + QPointF(95, 0)).toPoint()));
    QVERIFY(sky.moonRect().contains((moon - QPointF(95, 0)).toPoint()));
    sky.resize(2560, 1440);
    QCOMPARE(sky.moonPosition(0).y(), qreal(70)); // Tall windows keep the Moon in the header.
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
    // remove a visible star or halo, at any rotation or physical-pixel scale.
    const auto catalogue = sky.m_stars;
    for (const QSize size : {QSize(1280, 720), QSize(1920, 1080), QSize(958, 600)}) {
        sky.resize(size);
        QApplication::processEvents();
        QCOMPARE(sky.m_stars.size(), catalogue.size());
        const auto candidates = sky.m_candidates;
        const auto twinklers = sky.m_twinklers;
        QVERIFY(!candidates.isEmpty());
        QVERIFY(candidates.size() < sky.m_stars.size() / 4);
        for (const qint64 elapsed : {0LL, 21541022LL, 43082045LL, 64623067LL}) {
            sky.elapsed = elapsed;
            sky.invalidateImages();
            const QImage culled = sky.grab().toImage();

            // Oracle: render every original star, with no radial pruning.
            sky.m_candidates.clear();
            sky.m_twinklers.clear();
            for (int i = 0; i < sky.m_stars.size(); ++i) {
                sky.m_candidates.append(i);
                if (sky.m_stars.at(i).amplitude > 0) sky.m_twinklers.append(i);
            }
            sky.invalidateImages();
            const QImage unculled = sky.grab().toImage();
            sky.m_candidates = candidates;
            sky.m_twinklers = twinklers;
            QCOMPARE(culled, unculled);
        }
    }
    sky.setSkyVisible(false);
    QVERIFY(sky.m_candidates.isEmpty() && sky.m_twinklers.isEmpty());
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
    sky.show();
    QApplication::setActiveWindow(&sky);
    QApplication::processEvents();
    sky.grab();
    sky.elapsed = 10000;
    sky.onTick();
    QVERIFY(!sky.m_steady.isNull()); // Rotation must retain the large backing store.
    QVERIFY(!sky.m_sky_dirty.isEmpty());
    const QImage regional = sky.grab().toImage();
    QVERIFY(sky.m_sky_dirty.isEmpty());
    sky.m_steady = QPixmap();
    const QImage full = sky.grab().toImage();
    QCOMPARE(regional, full); // Old star/halo footprints were fully erased.
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
