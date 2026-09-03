// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/masternodelisttests.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <qt/clientmodel.h>
#include <qt/masternodelist.h>
#include <qt/optionsmodel.h>
#include <qt/walletmodel.h>
#include <test/util/setup_common.h>
#include <wallet/wallet.h>

#include <memory>

#include <QCheckBox>
#include <QHeaderView>
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
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_PROTX_HASH));
        QVERIFY(!w.mine->isEnabled());

        w.essential->setChecked(true);
        QVERIFY(w.table->isColumnHidden(MasternodeList::COLUMN_TYPE));
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
