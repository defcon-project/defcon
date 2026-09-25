// Copyright (c) 2009-2020 The Bitcoin Core developers
// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <interfaces/node.h>
#include <qt/bitcoin.h>
#include <qt/initexecutor.h>
#include <qt/test/apptests.h>
#include <qt/test/guiutiltests.h>
#include <qt/test/rpcnestedtests.h>
#include <qt/test/uritests.h>
#include <qt/test/trafficgraphdatatests.h>
#include <test/util/setup_common.h>

#ifdef ENABLE_WALLET
#include <qt/test/addressbooktests.h>
#include <qt/test/masternodelisttests.h>
#include <qt/test/wallettests.h>
#endif // ENABLE_WALLET

#include <QApplication>
#include <QObject>
#include <QTest>
#include <cstdio>
#include <functional>
#include <string>

#if defined(QT_STATIC)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_MINIMAL)
Q_IMPORT_PLUGIN(QMinimalIntegrationPlugin);
#endif
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
#endif

const std::function<void(const std::string&)> G_TEST_LOG_FUN{};

const std::function<std::vector<const char*>()> G_TEST_COMMAND_LINE_ARGUMENTS{};

// This is all you need to run all the tests
int main(int argc, char* argv[])
{
    // Strip our selector before forwarding the remaining arguments to QTest.
    // Without a selector the normal run still includes every compiled suite.
    std::string suite;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if (arg.compare(0, 8, "--suite=") != 0) continue;
        if (!suite.empty() || arg.size() == 8) {
            std::fprintf(stderr, "Use one non-empty --suite=ClassName selector\n");
            return 1;
        }
        suite = arg.substr(8);
        for (int j = i; j < argc; ++j) argv[j] = argv[j + 1];
        --argc;
        --i;
    }

    // Initialize persistent globals with the testing setup state for sanity.
    // E.g. -datadir in gArgs is set to a temp directory dummy value (instead
    // of defaulting to the default datadir), or globalChainParams is set to
    // regtest params.
    //
    // All tests must use their own testing setup (if needed).
    {
        BasicTestingSetup dummy{CBaseChainParams::REGTEST};
    }

    NodeContext node_context;
    std::unique_ptr<interfaces::Node> node = interfaces::MakeNode(&node_context);
    gArgs.ForceSetArg("-listen", "0");
    gArgs.ForceSetArg("-listenonion", "0");
    gArgs.ForceSetArg("-discover", "0");
    gArgs.ForceSetArg("-dnsseed", "0");
    gArgs.ForceSetArg("-fixedseeds", "0");
    gArgs.ForceSetArg("-upnp", "0");
    gArgs.ForceSetArg("-natpmp", "0");

    bool fInvalid = false;
    bool matched = false;
    const auto run = [&](QObject& test) {
        if (!suite.empty() && suite != test.metaObject()->className()) return;
        matched = true;
        if (QTest::qExec(&test, argc, argv) != 0) fInvalid = true;
    };

    // Prefer the "minimal" platform for the test instead of the normal default
    // platform ("xcb", "windows", or "cocoa") so tests can't unintentionally
    // interfere with any background GUIs and don't require extra resources.
    #if defined(WIN32)
        if (getenv("QT_QPA_PLATFORM") == nullptr) _putenv_s("QT_QPA_PLATFORM", "minimal");
    #else
        setenv("QT_QPA_PLATFORM", "minimal", 0 /* overwrite */);
    #endif

    BitcoinApplication app;
    app.setNode(*node);
    app.setApplicationName("Dash-Qt-test");

    app.node().context()->args = &gArgs;     // Make gArgs available in the NodeContext
    AppTests app_tests(app);
    run(app_tests);
    // Pure arithmetic, so it runs on every platform plugin including `minimal`.
    GUIUtilTests guiutil_tests;
    run(guiutil_tests);
#ifdef ENABLE_WALLET
    MasternodeListTests masternode_list_tests(app.node());
    run(masternode_list_tests);
#endif
    URITests test1;
    run(test1);
    RPCNestedTests test3(app.node());
    run(test3);
#ifdef ENABLE_WALLET
    WalletTests test5(app.node());
    run(test5);
    AddressBookTests test6(app.node());
    run(test6);
#endif

    TrafficGraphDataTests test7;
    run(test7);
    if (!matched) {
        std::fprintf(stderr, "Unknown or unavailable Qt test suite: %s\n", suite.c_str());
        return 1;
    }
    return fInvalid;
}
