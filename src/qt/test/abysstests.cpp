// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.
#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif
#include <qt/test/abysstests.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/addressbookpage.h>
#include <qt/appearancewidget.h>
#include <qt/bitcoinunits.h>
#include <qt/clientmodel.h>
#include <qt/coincontroldialog.h>
#include <qt/coincontroltreewidget.h>
#include <qt/guiutil.h>
#include <qt/openuridialog.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/overviewpage.h>
#include <qt/qrimagewidget.h>
#include <qt/qrdialog.h>
#include <qt/transactionview.h>
#include <qt/masternodelist.h>
#include <qt/receivecoinsdialog.h>
#include <qt/rpcconsole.h>
#include <qt/sendcoinsdialog.h>
#include <qt/signverifymessagedialog.h>
#include <qt/utilitydialog.h>
#include <qt/walletmodel.h>
#include <test/util/setup_common.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/wallet.h>
#include <QApplication>
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
#include <QLabel>
#include <QSettings>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QPainter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QTest>
#include <QTextStream>
#include <QTextDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
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
    auto* grid = page.findChild<QGridLayout*>("gridLayout");
    auto* total = page.findChild<QLabel*>("labelTotal");
    QVERIFY(row && grid && total);
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
