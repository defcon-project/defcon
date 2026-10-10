// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_TEST_ABYSSTESTS_H
#define BITCOIN_QT_TEST_ABYSSTESTS_H
#include <QObject>
namespace interfaces { class Node; }
class AbyssTests : public QObject
{
    Q_OBJECT
public:
    explicit AbyssTests(interfaces::Node& node) : m_node(node) {}
private Q_SLOTS:
    void initTestCase();
    void qrPixels_data();
    void qrPixels();
    void overviewResponsive();
    void themeRestoresLayout();
    void themeSwitchFromUnpolished_data();
    void themeSwitchFromUnpolished();
    void cliFontDoesNotPersist_data();
    void cliFontDoesNotPersist();
    void appearanceFonts();
    void cssStates();
    void zebraRows();
    void screenshots();
private:
    interfaces::Node& m_node;
};
#endif
