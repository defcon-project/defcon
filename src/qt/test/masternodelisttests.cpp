// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/masternodelisttests.h>

#include <evo/deterministicmns.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/clientmodel.h>
#include <qt/masternodelist.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <validation.h>
#include <wallet/wallet.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QLineEdit>
#include <QSettings>
#include <QTableWidget>

namespace {

struct Widgets {
    QTableWidget* table;
    QCheckBox* essential;
    QCheckBox* mine;
};

Widgets find(MasternodeList& list)
{
    return {
        list.findChild<QTableWidget*>("tableWidgetMasternodesDIP3"),
        list.findChild<QCheckBox*>("checkBoxEssentialInfoOnly"),
        list.findChild<QCheckBox*>("checkBoxMyMasternodesOnly"),
    };
}

void forget()
{
    QSettings settings;
    settings.remove("fMasternodeEssentialInfoOnly");
    settings.remove("fMasternodeMyMasternodesOnly");
    settings.remove("MasternodeListHeaderState");
}

} // namespace

// Exercise both ownership paths repeatedly. With a leak checker, this also
// detects items abandoned when a row does not match the filter.
void MasternodeListTests::repeatedFiltering()
{
    forget();
    TestChain100Setup test;
    struct RestoreContext {
        interfaces::Node& node;
        NodeContext* previous;
        std::chrono::seconds mock_time;
        ~RestoreContext()
        {
            node.setContext(previous);
            SetMockTime(mock_time);
            forget();
        }
    } restore{m_node, m_node.context(), GetMockTime()};
    m_node.setContext(&test.m_node);

    OptionsModel optionsModel;
    ClientModel clientModel(m_node, &optionsModel);
    MasternodeList view;
    view.setClientModel(&clientModel);
    view.show();
    const Widgets widgets = find(view);
    auto* filter = view.findChild<QLineEdit*>("filterLineEditDIP3");
    QVERIFY(widgets.table && filter);

    const CBlockIndex* tip = test.m_node.chainman->ActiveChain().Tip();
    CDeterministicMNList mnList(uint256S("03"), tip->nHeight, 1);
    auto mn = std::make_shared<CDeterministicMN>(0);
    mn->proTxHash = uint256S("01");
    mn->collateralOutpoint = COutPoint(uint256S("02"), 0);
    auto state = std::make_shared<CDeterministicMNState>();
    state->keyIDOwner = test.coinbaseKey.GetPubKey().GetID();
    // A banned MN remains visible without requiring a payment projection.
    state->BanIfNotBanned(0);
    mn->pdmnState = state;
    mnList.AddMN(mn);
    clientModel.setMasternodeList(mnList, tip);

    for (int iteration = 0; iteration < 128; ++iteration) {
        filter->setText("does-not-match-any-masternode");
        SetMockTime(GetTime() + 4);
        QVERIFY(QMetaObject::invokeMethod(&view, "updateDIP3ListScheduled", Qt::DirectConnection));
        QCOMPARE(widgets.table->rowCount(), 0);

        filter->clear();
        SetMockTime(GetTime() + 4);
        QVERIFY(QMetaObject::invokeMethod(&view, "updateDIP3ListScheduled", Qt::DirectConnection));
        QCOMPARE(widgets.table->rowCount(), 1);
        for (int column = 0; column <= MasternodeList::COLUMN_PROTX_HASH; ++column) {
            QVERIFY(widgets.table->item(0, column));
        }
        QCOMPARE(widgets.table->item(0, MasternodeList::COLUMN_PROTX_HASH)->text(),
                 QString::fromStdString(mn->proTxHash.ToString()));
    }
}

// Batched population must retain numeric sorting, equal-key order, filtering
// and the hidden identity used by the context-menu actions.
void MasternodeListTests::filteredRowsKeepOrderAndIdentity()
{
    forget();
    TestChain100Setup test;
    struct RestoreContext {
        interfaces::Node& node;
        NodeContext* previous;
        std::chrono::seconds mock_time;
        ~RestoreContext() { node.setContext(previous); SetMockTime(mock_time); forget(); }
    } restore{m_node, m_node.context(), GetMockTime()};
    m_node.setContext(&test.m_node);
    OptionsModel optionsModel;
    ClientModel clientModel(m_node, &optionsModel);
    MasternodeList view;
    view.setClientModel(&clientModel);
    view.show();
    const Widgets w = find(view);
    QVERIFY(w.table && w.essential);
    const CBlockIndex* tip = test.m_node.chainman->ActiveChain().Tip();
    CDeterministicMNList mnList(uint256S("abcd"), tip->nHeight, 16);
    for (int i = 0; i < 16; ++i) {
        auto mn = std::make_shared<CDeterministicMN>(i);
        mn->proTxHash = uint256S(strprintf("%064x", i + 1));
        mn->collateralOutpoint = COutPoint(mn->proTxHash, 0);
        auto state = std::make_shared<CDeterministicMNState>();
        std::memcpy(state->keyIDOwner.begin(), mn->proTxHash.begin(), state->keyIDOwner.size());
        state->nRegisteredHeight = i;
        state->BanIfNotBanned(0);
        mn->pdmnState = state;
        mnList.AddMN(mn);
    }
    std::vector<QString> reverse_order;
    mnList.ForEachMN(false, [&](const auto& mn) {
        reverse_order.emplace_back(QString::fromStdString(mn.proTxHash.ToString()));
    });
    std::reverse(reverse_order.begin(), reverse_order.end());
    clientModel.setMasternodeList(mnList, tip);
    const auto refresh = [&](const QString& filter) {
        const bool changed = QMetaObject::invokeMethod(&view, "on_filterLineEditDIP3_textChanged",
            Qt::DirectConnection, Q_ARG(QString, filter));
        SetMockTime(GetTime() + 4);
        return changed && QMetaObject::invokeMethod(&view, "updateDIP3ListScheduled", Qt::DirectConnection);
    };

    w.table->sortItems(MasternodeList::COLUMN_STATUS, Qt::AscendingOrder);
    QVERIFY(refresh(QString()));
    QCOMPARE(w.table->rowCount(), 16);
    for (int row = 0; row < 16; ++row) {
        QCOMPARE(w.table->item(row, MasternodeList::COLUMN_PROTX_HASH)->text(), reverse_order[row]);
        for (int column = 0; column <= MasternodeList::COLUMN_PROTX_HASH; ++column) {
            QVERIFY(w.table->item(row, column));
        }
    }
    w.table->sortItems(MasternodeList::COLUMN_REGISTERED, Qt::AscendingOrder);
    QVERIFY(refresh(QString()));
    for (int row = 0; row < 16; ++row) {
        QCOMPARE(w.table->item(row, MasternodeList::COLUMN_REGISTERED)->text().toInt(), row);
    }
    w.table->sortItems(MasternodeList::COLUMN_REGISTERED, Qt::DescendingOrder);
    QVERIFY(refresh(QString()));
    for (int row = 0; row < 16; ++row) {
        QCOMPARE(w.table->item(row, MasternodeList::COLUMN_REGISTERED)->text().toInt(), 15 - row);
    }
    w.table->selectRow(5);
    const QString selected = w.table->item(5, MasternodeList::COLUMN_PROTX_HASH)->text();
    QVERIFY(QMetaObject::invokeMethod(&view, "copyProTxHash_clicked", Qt::DirectConnection));
    QCOMPARE(QApplication::clipboard()->text(), selected);
    w.essential->setChecked(true);
    QVERIFY(refresh(selected));
    QCOMPARE(w.table->rowCount(), 1);
    QCOMPARE(w.table->item(0, MasternodeList::COLUMN_PROTX_HASH)->text(), selected);
    QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_PROTX_HASH));
    QVERIFY(refresh("no-matching-masternode"));
    QCOMPARE(w.table->rowCount(), 0);
    QVERIFY(refresh(QString()));
    QCOMPARE(w.table->rowCount(), 16);
}

void MasternodeListTests::hiddenUpdatesWaitForShow()
{
    forget();
    TestChain100Setup test;
    struct RestoreContext {
        interfaces::Node& node;
        NodeContext* previous;
        std::chrono::seconds mock_time;
        ~RestoreContext() { node.setContext(previous); SetMockTime(mock_time); forget(); }
    } restore{m_node, m_node.context(), GetMockTime()};
    m_node.setContext(&test.m_node);
    OptionsModel optionsModel;
    ClientModel clientModel(m_node, &optionsModel);
    QWidget parent;
    MasternodeList view(&parent);
    view.setClientModel(&clientModel);
    view.show(); // The parent is still hidden, as when another tab is selected.
    const Widgets w = find(view);
    auto* filter = view.findChild<QLineEdit*>("filterLineEditDIP3");
    QVERIFY(w.table && filter);
    const CBlockIndex* tip = test.m_node.chainman->ActiveChain().Tip();
    CDeterministicMNList mnList(uint256S("abcd"), tip->nHeight, 2);
    for (int i = 1; i <= 2; ++i) {
        auto mn = std::make_shared<CDeterministicMN>(i);
        mn->proTxHash = uint256S(strprintf("%064x", i));
        mn->collateralOutpoint = COutPoint(mn->proTxHash, 0);
        auto state = std::make_shared<CDeterministicMNState>();
        std::memcpy(state->keyIDOwner.begin(), mn->proTxHash.begin(), state->keyIDOwner.size());
        state->nRegisteredHeight = i;
        state->BanIfNotBanned(0);
        mn->pdmnState = state;
        mnList.AddMN(mn);
    }
    clientModel.setMasternodeList(mnList, tip);
    SetMockTime(GetTime() + 4);
    QVERIFY(QMetaObject::invokeMethod(&view, "updateDIP3ListScheduled", Qt::DirectConnection));
    QCOMPARE(w.table->rowCount(), 0);
    parent.show();
    QCOMPARE(w.table->rowCount(), 2);

    w.table->sortItems(MasternodeList::COLUMN_REGISTERED, Qt::DescendingOrder);
    w.table->selectRow(0);
    const QString selected = w.table->item(0, MasternodeList::COLUMN_PROTX_HASH)->text();
    // No changes while hidden: showing again must not rebuild the rows or
    // discard the user's selection.
    parent.hide();
    parent.show();
    QCOMPARE(w.table->selectedRanges().size(), 1);
    QCOMPARE(w.table->selectedRanges().front().topRow(), 0);
    QCOMPARE(w.table->item(0, MasternodeList::COLUMN_PROTX_HASH)->text(), selected);

    parent.hide();
    filter->setText(selected);
    SetMockTime(GetTime() + 4);
    QVERIFY(QMetaObject::invokeMethod(&view, "updateDIP3ListScheduled", Qt::DirectConnection));
    QCOMPARE(w.table->rowCount(), 2);
    parent.show();
    QCOMPARE(w.table->rowCount(), 1);
    QCOMPARE(w.table->item(0, MasternodeList::COLUMN_PROTX_HASH)->text(), selected);
    QCOMPARE(w.table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);

    parent.hide();
    mnList.RemoveMN(uint256S(selected.toStdString()));
    mnList.SetBlockHash(uint256S("abce"));
    clientModel.setMasternodeList(mnList, tip);
    SetMockTime(GetTime() + 1000);
    QVERIFY(QMetaObject::invokeMethod(&view, "updateDIP3ListScheduled", Qt::DirectConnection));
    QCOMPARE(w.table->rowCount(), 1);
    parent.show();
    QCOMPARE(w.table->rowCount(), 0);
}

// What a user sets on the masternode tab is still set after a restart: the
// column toggle, a column width, the sort column and direction. The dummy
// hash column never becomes visible, whatever an old layout recorded.
void MasternodeListTests::viewSurvivesRestart()
{
    forget();
    {
        MasternodeList first;
        const Widgets w = find(first);
        QVERIFY(w.table && w.essential && w.mine);

        // Fresh settings: the full view, the default sort, the box for "mine"
        // disabled because no wallet is bound yet.
        QVERIFY(!w.essential->isChecked());
        QVERIFY(!w.table->isColumnHidden(MasternodeList::COLUMN_TYPE));
        QVERIFY(!w.table->isColumnHidden(MasternodeList::COLUMN_COLLATERAL_ADDRESS));
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_PROTX_HASH));
        QVERIFY(!w.mine->isEnabled());

        // The essential view drops the type and the owner address, and keeps
        // the collateral address.
        w.essential->setChecked(true);
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_TYPE));
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_OWNER_ADDRESS));
        QVERIFY(!w.table->isColumnHidden(MasternodeList::COLUMN_COLLATERAL_ADDRESS));
        w.table->setColumnWidth(MasternodeList::COLUMN_STATUS, 123);
        w.table->horizontalHeader()->setSortIndicator(MasternodeList::COLUMN_POSE, Qt::DescendingOrder);
    }
    {
        MasternodeList second;
        const Widgets w = find(second);
        QVERIFY(w.table && w.essential);

        QVERIFY(w.essential->isChecked());
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_TYPE));
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_PROTX_HASH));
        QCOMPARE(w.table->columnWidth(MasternodeList::COLUMN_STATUS), 123);
        QCOMPARE(w.table->horizontalHeader()->sortIndicatorSection(), int{MasternodeList::COLUMN_POSE});
        QCOMPARE(w.table->horizontalHeader()->sortIndicatorOrder(), Qt::DescendingOrder);

        // Turning the toggle back off shows the columns again in this session
        // and is what the next one starts from.
        w.essential->setChecked(false);
        QVERIFY(!w.table->isColumnHidden(MasternodeList::COLUMN_TYPE));
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_PROTX_HASH));
    }
    {
        MasternodeList third;
        const Widgets w = find(third);
        QVERIFY(!w.essential->isChecked());
        QVERIFY(!w.table->isColumnHidden(MasternodeList::COLUMN_TYPE));
    }
    forget();
}

// A layout saved while the essential view still hid the collateral address
// remembers that section as hidden, and the layout is restored before the
// toggle is applied. The toggle decides: the old layout's widths come back,
// its hidden collateral column does not.
void MasternodeListTests::oldLayoutDoesNotHideTheCollateral()
{
    forget();
    {
        MasternodeList old;
        const Widgets w = find(old);
        QVERIFY(w.table && w.essential);
        w.essential->setChecked(true);
        // What the earlier view did, done by hand; the resize writes the state.
        w.table->setColumnHidden(MasternodeList::COLUMN_COLLATERAL_ADDRESS, true);
        w.table->setColumnWidth(MasternodeList::COLUMN_STATUS, 111);
        QSettings settings;
        QVERIFY(!settings.value("MasternodeListHeaderState").toByteArray().isEmpty());
    }
    {
        MasternodeList upgraded;
        const Widgets w = find(upgraded);
        QVERIFY(w.table && w.essential);
        QVERIFY(w.essential->isChecked());
        // The old layout was restored...
        QCOMPARE(w.table->columnWidth(MasternodeList::COLUMN_STATUS), 111);
        // ...and did not get to keep the column hidden.
        QVERIFY(!w.table->isColumnHidden(MasternodeList::COLUMN_COLLATERAL_ADDRESS));
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_OWNER_ADDRESS));
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_PROTX_HASH));

        w.essential->setChecked(false);
        for (int column = 0; column < MasternodeList::COLUMN_PROTX_HASH; ++column) {
            QVERIFY2(!w.table->isColumnHidden(column), qPrintable(QString::number(column)));
        }
    }
    forget();
}

// "My masternodes only" is remembered like the rest, but it can only mean
// anything once a wallet is bound: until then the box stays disabled and
// unticked, and binding the wallet is what brings the remembered tick back.
void MasternodeListTests::mineOnlyWaitsForTheWallet()
{
    forget();

    TestChain100Setup test;
    m_node.setContext(&test.m_node);
    std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.context()->chain.get(),
                                                                m_node.context()->coinjoin_loader.get(), "",
                                                                CreateMockWalletDatabase());
    wallet->LoadWallet();
    OptionsModel optionsModel;
    ClientModel clientModel(m_node, &optionsModel);
    WalletModel walletModel(interfaces::MakeWallet(wallet), clientModel);

    {
        MasternodeList first;
        const Widgets w = find(first);
        QVERIFY(w.mine);
        QVERIFY(!w.mine->isEnabled());

        first.setWalletModel(&walletModel);
        QVERIFY(w.mine->isEnabled());
        QVERIFY(!w.mine->isChecked());

        w.mine->setChecked(true);
        QSettings settings;
        QCOMPARE(settings.value("fMasternodeMyMasternodesOnly").toBool(), true);
    }
    {
        MasternodeList second;
        const Widgets w = find(second);

        // Remembered, but not yet: no wallet, no tick.
        QVERIFY(!w.mine->isEnabled());
        QVERIFY(!w.mine->isChecked());

        second.setWalletModel(&walletModel);
        QVERIFY(w.mine->isEnabled());
        QVERIFY(w.mine->isChecked());

        // Losing the wallet disables the box; the preference is untouched.
        second.setWalletModel(nullptr);
        QVERIFY(!w.mine->isEnabled());
        QSettings settings;
        QCOMPARE(settings.value("fMasternodeMyMasternodesOnly").toBool(), true);
    }
    forget();
    m_node.setContext(nullptr);
}
