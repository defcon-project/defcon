// Copyright (c) 2016-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/rpcnestedtests.h>

#include <chainparams.h>
#include <interfaces/node.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <qt/rpcconsole.h>
#include <test/util/setup_common.h>
#include <univalue.h>
#include <util/string.h>
#include <util/system.h>
#ifdef ENABLE_WALLET
#include <wallet/rpcwallet.h>
#endif

#include <QTest>

#include <set>
#include <string>
#include <stdexcept>
#include <tuple>
#include <vector>

static RPCHelpMan rpcNestedTest_rpc()
{
    return RPCHelpMan{
        "rpcNestedTest",
        "echo the passed string(s)",
        {
            {"arg1", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""},
            {"arg2", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""},
            {"arg3", RPCArg::Type::STR, RPCArg::Optional::OMITTED, ""},
        },
        RPCResult{RPCResult::Type::ANY, "", ""},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            return request.params.write(0, 0);
        },
    };
}

static const CRPCCommand vRPCCommands[] = {
    {"rpcNestedTest", &rpcNestedTest_rpc},
};

void RPCNestedTests::rpcNestedTests()
{
    // do some test setup
    // could be moved to a more generic place when we add more tests on QT level
    for (const auto& c : vRPCCommands) {
        tableRPC.appendCommand(c.name, &c);
    }

    TestingSetup test;
    m_node.setContext(&test.m_node);

    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();

    std::string result;
    std::string result2;
    std::string filtered;
    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo()[chain]", &filtered); //simple result filtering with path
    QVERIFY(result=="main");
    QVERIFY(filtered == "getblockchaininfo()[chain]");

    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblock(getbestblockhash())"); //simple 2 level nesting
    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblock(getblock(getbestblockhash())[hash], true)");

    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblock( getblock( getblock(getbestblockhash())[hash] )[hash], true)"); //4 level nesting with whitespace, filtering path and boolean parameter

    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo");
    QVERIFY(result.substr(0,1) == "{");

    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo()");
    QVERIFY(result.substr(0,1) == "{");

    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo "); //whitespace at the end will be tolerated
    QVERIFY(result.substr(0,1) == "{");

    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo()[\"chain\"]"); //Quote path identifier are allowed, but look after a child containing the quotes in the key
    QVERIFY(result == "null");

    RPCConsole::RPCExecuteCommandLine(m_node, result, "createrawtransaction [] {} 0"); //parameter not in brackets are allowed
    RPCConsole::RPCExecuteCommandLine(m_node, result2, "createrawtransaction([],{},0)"); //parameter in brackets are allowed
    QVERIFY(result == result2);
    RPCConsole::RPCExecuteCommandLine(m_node, result2, "createrawtransaction( [],  {} , 0   )"); //whitespace between parameters is allowed
    QVERIFY(result == result2);

    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblock(getbestblockhash())[tx][0]", &filtered);
    QVERIFY(result == Params().GenesisBlock().vtx.at(0)->GetHash().GetHex());
    QVERIFY(filtered == "getblock(getbestblockhash())[tx][0]");

    RPCConsole::RPCParseCommandLine(nullptr, result, "importprivkey", false, &filtered);
    QVERIFY(filtered == "importprivkey(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "signmessagewithprivkey abc", false, &filtered);
    QVERIFY(filtered == "signmessagewithprivkey(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "signmessagewithprivkey abc,def", false, &filtered);
    QVERIFY(filtered == "signmessagewithprivkey(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "signrawtransactionwithkey(abc)", false, &filtered);
    QVERIFY(filtered == "signrawtransactionwithkey(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "walletpassphrase(help())", false, &filtered);
    QVERIFY(filtered == "walletpassphrase(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "walletpassphrasechange(help(walletpassphrasechange(abc)))", false, &filtered);
    QVERIFY(filtered == "walletpassphrasechange(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "help(encryptwallet(abc, def))", false, &filtered);
    QVERIFY(filtered == "help(encryptwallet(…))");
    RPCConsole::RPCParseCommandLine(nullptr, result, "help(importprivkey())", false, &filtered);
    QVERIFY(filtered == "help(importprivkey(…))");
    RPCConsole::RPCParseCommandLine(nullptr, result, "help(importprivkey(help()))", false, &filtered);
    QVERIFY(filtered == "help(importprivkey(…))");
    RPCConsole::RPCParseCommandLine(nullptr, result, "help(importprivkey(abc), walletpassphrase(def))", false, &filtered);
    QVERIFY(filtered == "help(importprivkey(…), walletpassphrase(…))");

    // The other commands that take a secret, and the subcommand of a listed
    // pair, in each syntax the console accepts.
    RPCConsole::RPCParseCommandLine(nullptr, result, "importblsprivkey abc", false, &filtered);
    QVERIFY(filtered == "importblsprivkey(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "importdescriptors '[{\"desc\": \"abc\"}]'", false, &filtered);
    QVERIFY(filtered == "importdescriptors(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "createwallet w false false abc", false, &filtered);
    QVERIFY(filtered == "createwallet(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "upgradetohd \"abc def\" \"\" ghi", false, &filtered);
    QVERIFY(filtered == "upgradetohd(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls fromsecret abc", false, &filtered);
    QVERIFY(filtered == "bls fromsecret(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls fromsecret 'abc' true", false, &filtered);
    QVERIFY(filtered == "bls fromsecret(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls(fromsecret, abc)", false, &filtered);
    QVERIFY(filtered == "bls(fromsecret(…))");
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls(fromsecret abc)[public]", false, &filtered);
    QVERIFY(filtered == "bls(fromsecret(…))[public]");
    RPCConsole::RPCParseCommandLine(nullptr, result, "\"bls fromsecret\" abc", false, &filtered);
    QVERIFY(filtered == "\"bls fromsecret\"(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls(fromsecret(abc))", false, &filtered);
    QVERIFY(filtered == "bls(fromsecret(…))");
    // Quoted names with extra spaces, which no command answers, still hide what follows.
    RPCConsole::RPCParseCommandLine(nullptr, result, "\"protx \" revoke abc def", false, &filtered);
    QVERIFY(filtered == "\"protx \" revoke(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "\"bls  fromsecret\" abc", false, &filtered);
    QVERIFY(filtered == "\"bls  fromsecret\"(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "protx(revoke(abc, def))", false, &filtered);
    QVERIFY(filtered == "protx(revoke(…))");
    // A subcommand computed by a nested command is unknown until it runs.
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls(echo(fromsecret)[0], abc)", false, &filtered);
    QVERIFY(filtered == "bls(echo(fromsecret)[0](…))");
    // Also when the nested command that computes it is hidden itself.
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls(createwallet(fromsecret)[name], abc)", false, &filtered);
    QVERIFY(filtered == "bls(createwallet(…))");
    // A hidden command left open behind an unclosed inner parenthesis.
    QVERIFY(RPCConsole::RPCParseCommandLine(nullptr, result, "walletpassphrase TOPsecret0123456789 echo(60", false, &filtered));
    QVERIFY(filtered == "walletpassphrase(…)");
    // Its descriptor argument is not called "descriptor".
    RPCConsole::RPCParseCommandLine(nullptr, result, "generateblock 'pkh(abc)' []", false, &filtered);
    QVERIFY(filtered == "generateblock(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "protx revoke abc def", false, &filtered);
    QVERIFY(filtered == "protx revoke(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "protx update_service abc 1.2.3.4:8192 def", false, &filtered);
    QVERIFY(filtered == "protx update_service(…)");
    RPCConsole::RPCParseCommandLine(nullptr, result, "help(protx(update_service_compute, abc, def, ghi))", false, &filtered);
    QVERIFY(filtered == "help(protx(update_service_compute(…)))");
    // Other subcommands of the same command, and help about a hidden one, stay as typed.
    RPCConsole::RPCParseCommandLine(nullptr, result, "bls generate", false, &filtered);
    QVERIFY(filtered == "bls generate");
    RPCConsole::RPCParseCommandLine(nullptr, result, "protx info abc", false, &filtered);
    QVERIFY(filtered == "protx info abc");
    RPCConsole::RPCParseCommandLine(nullptr, result, "help protx revoke", false, &filtered);
    QVERIFY(filtered == "help protx revoke");

    // Whether the reply can show a secret, for the command whose reply is shown.
    bool secret_reply{false};
    const auto shows_secrets = [&](const std::string& command) {
        secret_reply = !secret_reply;
        RPCConsole::RPCParseCommandLine(nullptr, result, command, false, &filtered, nullptr, &secret_reply);
        return secret_reply;
    };
    QVERIFY(shows_secrets("dumpprivkey abc"));
    QVERIFY(shows_secrets("dumphdinfo()[mnemonic]"));
    QVERIFY(shows_secrets("bls generate"));
    QVERIFY(shows_secrets("bls(fromsecret, abc)"));
    QVERIFY(shows_secrets("listdescriptors true"));
    QVERIFY(shows_secrets("quorum info 1 abc true"));
    // Other spellings of true, and values only a nested command knows.
    QVERIFY(shows_secrets("listdescriptors ' true '"));
    QVERIFY(shows_secrets("quorum info 1 abc yes"));
    QVERIFY(shows_secrets("listdescriptors(echojson(true)[0])"));
    QVERIFY(shows_secrets("bls(echo(generate)[0])"));
    QVERIFY(!shows_secrets("listdescriptors"));
    QVERIFY(!shows_secrets("listdescriptors false"));
    QVERIFY(!shows_secrets("listdescriptors ' false '"));
    QVERIFY(!shows_secrets("quorum info 1 abc"));
    QVERIFY(!shows_secrets("quorum info 1 abc no"));
    QVERIFY(!shows_secrets("bls"));
    QVERIFY(!shows_secrets("importprivkey(dumpprivkey(abc))"));
    QVERIFY(!shows_secrets("getblockcount"));

    // An error that repeats an argument of a hidden command does not show it.
    // The JSON parse error, which repeats the argument that does not parse, is
    // hidden whole.
    const std::string probe{"PROBEkey0123456789abcdefghijklmnopqrstuvwxyzABCDEF"};
    try {
        RPCConsole::RPCExecuteCommandLine(m_node, result, "signrawtransactionwithkey 00 '[\"" + probe + "\"'");
        QFAIL("a private key array that does not parse was accepted");
    } catch (const std::runtime_error& e) {
        const std::string what{e.what()};
        QVERIFY(what.find("Error parsing JSON") != std::string::npos);
        QVERIFY(what.find(probe) == std::string::npos);
    } catch (const UniValue& error) {
        QFAIL(qPrintable(QString::fromStdString("unexpected RPC error: " + error.write())));
    }
    try {
        // A short passphrase typed where the timeout goes.
        RPCConsole::RPCExecuteCommandLine(m_node, result, "walletpassphrase 60 hunter2");
        QFAIL("a timeout that does not parse was accepted");
    } catch (const std::runtime_error& e) {
        QCOMPARE(QString::fromStdString(e.what()), QString("Error parsing JSON: (…)"));
    } catch (const UniValue& error) {
        QFAIL(qPrintable(QString::fromStdString("unexpected RPC error: " + error.write())));
    }
    // Any other error of a line with a hidden argument shows its code and a
    // fixed text in place of its message. Each of these messages would repeat
    // the typed value: as typed, in lower case (where the scheme flag goes),
    // decoded from a JSON escape, cut by the key path parser (its "h"), short,
    // of punctuation only, not ASCII, or a key with no digit.
    const std::string hidden_error{"The details of this error are hidden, because the command can contain a private key, a recovery phrase or a passphrase."};
    const auto rpc_error = [&](const std::string& command, int& code, std::string& message) {
        try {
            RPCConsole::RPCExecuteCommandLine(m_node, result, command);
        } catch (const UniValue& error) {
            const UniValue& number{find_value(error, "code")};
            const UniValue& text{find_value(error, "message")};
            if (!number.isNum() || !text.isStr()) return false;
            code = number.get_int();
            message = text.get_str();
            return true;
        } catch (const std::exception&) {
        }
        return false;
    };
    const std::string hex_key{"0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF"};
    // Not a valid key, so that no real one is in the source; the parser takes the same path.
    const std::string letter_key{"QPfNgQNyqUdEwMSMKYuKWEddzfJNxCRGapvertugohLfNzyXihkX"};
    int code{0};
    std::string message;
    for (const std::string& command : std::vector<std::string>{
             "getdescriptorinfo 'pkh(" + probe + ")'",
             "bls fromsecret true " + hex_key,
             "bls fromsecret true Hunter2pass",
             "bls fromsecret true hunter2",
             "bls fromsecret true 'p@ss!'",
             "bls fromsecret true '@!?'",
             "bls fromsecret true 'é'",
             "scantxoutset start '[\"pkh(\\u0050" + probe.substr(1) + ")\"]'",
             "getdescriptorinfo 'pkh(" + letter_key + "/0)'",
             "getdescriptorinfo 'pkh([00000000/" + probe + "h]02)'",
             "getdescriptorinfo 'pkh(!@$%^&*)'"}) {
        QVERIFY2(rpc_error(command, code, message), command.c_str());
        QVERIFY2(message == hidden_error, (command + ": " + message).c_str());
    }
    QVERIFY(rpc_error("getdescriptorinfo 'pkh(" + probe + ")'", code, message));
    QCOMPARE(code, static_cast<int>(RPC_INVALID_ADDRESS_OR_KEY));
    // A fixed message from the source, which repeats no argument, stays.
    QVERIFY(rpc_error("signmessagewithprivkey " + probe + " hello", code, message));
    QCOMPARE(QString::fromStdString(message), QString("Invalid private key"));
    QVERIFY(rpc_error("bls fromsecret " + probe, code, message));
    QCOMPARE(QString::fromStdString(message), QString("secretKey must be a valid BLS secret key"));
    // The error of a line without a hidden argument keeps its message.
    QVERIFY(rpc_error("getblockheader " + probe, code, message));
    QVERIFY(message.find(probe) != std::string::npos);
    // The rule holds for the whole line: for a command that fails before the
    // hidden one runs, and for an outer command that cannot parse what a hidden
    // nested command returned.
    QVERIFY(rpc_error("echo(getblockheader(" + probe + "), walletpassphrase(" + probe + "x, 60))", code, message));
    QCOMPARE(QString::fromStdString(message), QString::fromStdString(hidden_error));
    QCOMPARE(code, static_cast<int>(RPC_INVALID_PARAMETER));
    try {
        RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockhash(getdescriptorinfo('raw(deadbeef)')[descriptor])");
        QFAIL("a descriptor was accepted as a block height");
    } catch (const std::runtime_error& e) {
        QCOMPARE(QString::fromStdString(e.what()), QString("Error parsing JSON: (…)"));
    } catch (const UniValue& error) {
        QFAIL(qPrintable(QString::fromStdString("unexpected RPC error: " + error.write())));
    }

    // Every registered command with an argument that can hold a secret is
    // hidden, so a new command or argument cannot be left out of the list.
    // generateblock's "output" takes an address or a descriptor.
    const std::set<std::string> secret_args{"descriptor", "descriptors", "mnemonic", "mnemonicpassphrase",
                                            "newpassphrase", "oldpassphrase", "operatorKey", "output",
                                            "passphrase", "privkey", "privkeys", "requests", "scanobjects",
                                            "secret", "seed", "walletpassphrase"};
    std::vector<std::tuple<std::string, size_t, std::string>> args; // command, position, argument name
    for (const std::string& name : tableRPC.listCommands()) {
        JSONRPCRequest request;
        request.context = test.m_node;
        request.mode = JSONRPCRequest::GET_ARGS;
        request.params = UniValue{UniValue::VARR};
        const size_t space{name.find(' ')};
        request.strMethod = name.substr(0, space);
        if (space != std::string::npos) request.params.push_back(name.substr(space + 1));
        UniValue rows;
        try {
            rows = tableRPC.execute(request);
        } catch (const UniValue& error) {
            // A wallet command an earlier suite registered and has since let go
            // keeps its name here with no handler; the wallet's own list follows.
            const UniValue& code{find_value(error, "code")};
            if (code.isNum() && code.get_int() == RPC_METHOD_NOT_FOUND) continue;
            QFAIL(qPrintable(QString::fromStdString(name + ": " + error.write())));
        } catch (const std::exception& e) {
            QFAIL(qPrintable(QString::fromStdString(name + ": " + e.what())));
        }
        if (!rows.isArray()) QFAIL(qPrintable(QString::fromStdString(name + " has no argument map: " + rows.write())));
        for (const UniValue& row : rows.getValues()) {
            if (!row.isArray() || row.size() < 3 || !row[1].isNum() || !row[2].isStr()) {
                QFAIL(qPrintable(QString::fromStdString(name + " has an argument row of another shape: " + row.write())));
            }
            args.emplace_back(name, size_t(row[1].get_int()), row[2].get_str());
        }
    }
#ifdef ENABLE_WALLET
    for (const CRPCCommand& command : GetWalletRPCCommands()) {
        for (size_t pos = 0; pos < command.argNames.size(); ++pos) {
            for (const std::string& arg : SplitString(command.argNames[pos], '|')) args.emplace_back(command.name, pos, arg);
        }
    }
#endif
    std::set<std::string> checked;
    for (const auto& [name, pos, arg] : args) {
        if (!secret_args.count(arg)) continue;
        std::string line{name};
        for (size_t i = 0; i < pos; ++i) line += " x";
        line += " SECRETPROBE";
        RPCConsole::RPCParseCommandLine(nullptr, result, line, false, &filtered);
        if (filtered.find("SECRETPROBE") != std::string::npos) {
            QFAIL(qPrintable(QString::fromStdString(name + " shows its argument " + arg)));
        }
        checked.insert(name);
    }
    // The walk reaches subcommands, and both node and wallet commands. The
    // protx subcommands that take a key exist only with the wallet built in.
    QVERIFY(checked.count("bls fromsecret") && checked.count("signrawtransactionwithkey"));
#ifdef ENABLE_WALLET
    QVERIFY(checked.count("protx revoke") && checked.count("importblsprivkey") && checked.count("createwallet") && checked.count("upgradetohd"));
#endif

    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest");
    QVERIFY(result == "[]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest ''");
    QVERIFY(result == "[\"\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest \"\"");
    QVERIFY(result == "[\"\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest '' abc");
    QVERIFY(result == "[\"\",\"abc\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest abc '' abc");
    QVERIFY(result == "[\"abc\",\"\",\"abc\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest abc  abc");
    QVERIFY(result == "[\"abc\",\"abc\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest abc\t\tabc");
    QVERIFY(result == "[\"abc\",\"abc\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest(abc )");
    QVERIFY(result == "[\"abc\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest( abc )");
    QVERIFY(result == "[\"abc\"]");
    RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest(   abc   ,   cba )");
    QVERIFY(result == "[\"abc\",\"cba\"]");

    QVERIFY_EXCEPTION_THROWN(RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo() .\n"), std::runtime_error); //invalid syntax
    QVERIFY_EXCEPTION_THROWN(RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo() getblockchaininfo()"), std::runtime_error); //invalid syntax
    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo("); //tolerate non closing brackets if we have no arguments
    RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo()()()"); //tolerate non command brackets
    QVERIFY_EXCEPTION_THROWN(RPCConsole::RPCExecuteCommandLine(m_node, result, "getblockchaininfo(True)"), UniValue); //invalid argument
    QVERIFY_EXCEPTION_THROWN(RPCConsole::RPCExecuteCommandLine(m_node, result, "a(getblockchaininfo(True))"), UniValue); //method not found
    QVERIFY_EXCEPTION_THROWN(RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest abc,,abc"), std::runtime_error); //don't tolerate empty arguments when using ,
    QVERIFY_EXCEPTION_THROWN(RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest(abc,,abc)"), std::runtime_error); //don't tolerate empty arguments when using ,
    QVERIFY_EXCEPTION_THROWN(RPCConsole::RPCExecuteCommandLine(m_node, result, "rpcNestedTest(abc,,)"), std::runtime_error); //don't tolerate empty arguments when using ,
}
