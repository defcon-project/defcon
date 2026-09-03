// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_MASTERNODELISTTESTS_H
#define BITCOIN_QT_TEST_MASTERNODELISTTESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

class MasternodeListTests : public QObject
{
public:
    explicit MasternodeListTests(interfaces::Node& node) : m_node(node) {}
    interfaces::Node& m_node;

    Q_OBJECT

private Q_SLOTS:
    void viewSurvivesRestart();
    void mineOnlyWaitsForTheWallet();
};

#endif // BITCOIN_QT_TEST_MASTERNODELISTTESTS_H
