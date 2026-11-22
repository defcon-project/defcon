// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TEST_COINJOINUITESTS_H
#define BITCOIN_QT_TEST_COINJOINUITESTS_H

#include <QObject>
#include <QTest>

namespace interfaces {
class Node;
} // namespace interfaces

class CoinJoinUiTests : public QObject
{
    Q_OBJECT
public:
    explicit CoinJoinUiTests(interfaces::Node& node) : m_node(node) {}

private Q_SLOTS:
    void initTestCase();
    void targetFieldIsCapped();
    void helpDescribesLargerAmounts();
    void helpStatesThePracticalSize();
    void savedTargetKept();

private:
    interfaces::Node& m_node;
};

#endif // BITCOIN_QT_TEST_COINJOINUITESTS_H
