// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_TEST_ABYSSTESTS_H
#define BITCOIN_QT_TEST_ABYSSTESTS_H
#include <QObject>
#include <functional>
namespace interfaces { class Node; }
class AbyssTests : public QObject
{
    Q_OBJECT
public:
    explicit AbyssTests(interfaces::Node& node) : m_node(node) {}
private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanup();
    void qrPixels_data();
    void qrPixels();
    void networkStyleLifetime();
    void overviewTotalHeight_data();
    void overviewTotalHeight();
    void overviewResponsive();
    void themeRestoresLayout();
    void themeSwitchFromUnpolished_data();
    void themeSwitchFromUnpolished();
    void cliFontDoesNotPersist_data();
    void cliFontDoesNotPersist();
    void appearanceFonts();
    void cssStates();
    void zebraRows();
    void networkBar();
    void skySuspendClock();
    void initialWindowGeometry_data();
    void initialWindowGeometry();
    void quickControls();
    void topMenus_data();
    void topMenus();
    void paymentPages();
    void paymentSummaryLifecycle();
    void networkModelLifecycle();
    void emptyStates();
    void presentationIcons();
    void iconPixelRatios();
    void skyGeometry();
    void skyCulling();
    void skyMoon();
    void skyLifecycle();
    void skyRegionalRepaint();
    void skyOverviewBackground();
    void screenshots();
    // Last, after the window-activation checks of paymentPages: run before
    // them, its two short-lived wallet windows were followed by occasional
    // activation failures on xcb without a window manager.
    void coinJoinWidgetsPerPage();
private:
    interfaces::Node& m_node;
    std::function<void()> m_restore_test_settings;
};
#endif // BITCOIN_QT_TEST_ABYSSTESTS_H
