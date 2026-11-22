// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/coinjoinuitests.h>

#include <chainparams.h>
#include <coinjoin/common.h>
#include <consensus/amount.h>
#include <interfaces/node.h>
#include <qt/forms/ui_optionsdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/utilitydialog.h>
#include <util/system.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif

#include <univalue.h>

#include <QApplication>
#include <QDataWidgetMapper>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLocale>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace {
//! An amount of whole DFCN as the English texts write it ("10,000").
QString Written(int amount) { return QLocale(QLocale::English).toString(amount); }

//! An amount as the texts write one to type into an amount field: no
//! separators, no trailing zeros ("10000.1").
QString Typed(CAmount amount)
{
    QString text = QString("%1.%2").arg(amount / COIN).arg(amount % COIN, 8, 10, QLatin1Char('0'));
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
    return text;
}

//! The larger (DFCN-scale) amounts, bits 5-7 of the wire table, as typed.
QStringList LargerAmounts()
{
    QStringList amounts;
    for (size_t bit = 5; bit < CoinJoin::vecDenominationsByBit.size(); ++bit) {
        amounts << Typed(CoinJoin::vecDenominationsByBit[bit]);
    }
    return amounts;
}
} // namespace

void CoinJoinUiTests::initTestCase()
{
    // The options dialog needs the application fonts (savedTargetKept).
    if (!GUIUtil::fontsLoaded()) GUIUtil::loadFonts();
}

void CoinJoinUiTests::targetFieldIsCapped()
{
    // The amounts the wallet makes are the small ones of the chain this one
    // forked from (at most about 10 DFCN), and the largest one has no count
    // limit: a target in the millions, which the field allowed, splits the
    // balance into that many coins of 10 DFCN. The options form caps the field
    // and says why, and that the larger amounts, which the wallet does not
    // make, are mixed apart from the target. The form is read on its own: the
    // dialog raises the cap only for a target saved above it
    // (savedTargetKept), and it cannot be built without the application fonts.
    QDialog dialog;
    Ui::OptionsDialog form;
    form.setupUi(&dialog);
    QSpinBox* target = form.coinJoinAmount;
    QCOMPARE(target->minimum(), 2);
    QCOMPARE(target->maximum(), 10000);
    QCOMPARE(target->value(), 1000);
    // A larger value typed in stops at the cap.
    target->setValue(1000000);
    QCOMPARE(target->value(), 10000);
    const QString tip = target->toolTip();
    QVERIFY2(tip.startsWith(QString("At most %1 DFCN.").arg(Written(target->maximum()))), qPrintable(tip));
    QVERIFY2(tip.contains("the standard amounts are at most about 10 DFCN", Qt::CaseInsensitive), qPrintable(tip));
    // The larger amounts by their values in the wire table: the target does not count them.
    const QStringList larger = LargerAmounts();
    QCOMPARE(larger.size(), 3);
    QVERIFY2(tip.endsWith(QString(" Coins of %1, %2 and %3 DFCN are mixed apart from this target.").arg(larger[0], larger[1], larger[2])), qPrintable(tip));
    // The label says which mixing the target turns off.
    QCOMPARE(form.lblCoinJoinAmountText->toolTip(), QString("This amount acts as a threshold to turn off mixing of the standard amounts once it's reached."));
}

void CoinJoinUiTests::helpDescribesLargerAmounts()
{
    // The three larger amounts are bits 5-7 of the wire table: the wallet mixes
    // them and counts none of them toward the target, but makes none; the
    // user does. The help names them as they are typed, says which wallets the
    // recipe is for, gives the recipe, and says from which block they are
    // mixed: the height the mixing client reads, or that this version has
    // none yet.
    const QString start = "Mixing these amounts starts at block 123456. From then on, while mixing is switched on,";
    const QString no_start = "The block at which mixing these amounts starts is not set in this version yet. "
                             "From then on, while mixing is switched on,";
    const QString set = HelpMessageDialog::coinJoinHelp(int64_t{123456});
    const QString unset = HelpMessageDialog::coinJoinHelp(std::nullopt);
    QVERIFY2(set.contains(start) && !set.contains("is not set in this version"), qPrintable(set));
    QVERIFY2(unset.contains(no_start) && !unset.contains("starts at block"), qPrintable(unset));
    // Apart from that sentence the two are one text.
    QCOMPARE(QString(set).replace(start, no_start), unset);
    // The dialog shows the text of the chain's own height: none while the
    // activation height is unset, as on every network today, and the height
    // the client mixes from once it is set. The chain's two values are put back.
    auto& consensus = const_cast<Consensus::Params&>(Params().GetConsensus());
    struct PutBack {
        Consensus::Params& params;
        const int height;
        const std::optional<int> mixing_override;
        ~PutBack()
        {
            params.nV231ActivationHeight = height;
            params.nDfcnScaleMixingHeightOverride = mixing_override;
        }
    } put_back{consensus, consensus.nV231ActivationHeight, consensus.nDfcnScaleMixingHeightOverride};
    const auto shown = [] {
        HelpMessageDialog help(nullptr, HelpMessageDialog::pshelp);
        auto* message = help.findChild<QLabel*>("aboutMessage");
        return message ? message->text() : QString();
    };
    consensus.nV231ActivationHeight = std::numeric_limits<int>::max();
    consensus.nDfcnScaleMixingHeightOverride = std::nullopt;
    QCOMPARE(shown(), unset);
    consensus.nV231ActivationHeight = 123456 - CoinJoin::DFCN_SCALE_MIXING_DELAY;
    QCOMPARE(shown(), set);

    // The section stands between the list and the size section.
    const int list_end = set.indexOf("</ul>");
    const int section = set.indexOf("<hr><b>Larger amounts</b>");
    const int size = set.indexOf("<hr><b>How much to mix</b>");
    QVERIFY2(list_end > 0 && section > list_end && size > section, qPrintable(set));
    const QString text = set.mid(section, size - section);
    // Which wallets the recipe is for stands before its steps: a coin paid to
    // an address the wallet does not track as its own would leave its balance.
    const int condition = text.indexOf("Use this recipe only in a wallet whose receiving and change addresses are its own "
                                       "spendable P2PKH addresses, as they are by default");
    const int steps = text.indexOf("<ol");
    QVERIFY2(condition > 0 && steps > condition, qPrintable(text));
    // The amounts by their values in the wire table, none of them one the wallet makes.
    for (size_t bit = 5; bit < CoinJoin::vecDenominationsByBit.size(); ++bit) {
        const CAmount amount = CoinJoin::vecDenominationsByBit[bit];
        QVERIFY(CoinJoin::IsDfcnScaleDenomination(CoinJoin::AmountToDenomination(amount)));
        QVERIFY(!CoinJoin::IsDenominatedAmount(amount));
    }
    const QStringList larger = LargerAmounts();
    QCOMPARE(larger.size(), 3);
    for (const QString& expected : {QString("three more amounts: %1, %2 and %3 DFCN").arg(larger[0], larger[1], larger[2]),
                                    QString("Your wallet mixes coins of these amounts, but it does not make them"),
                                    QString("Create a new receiving address for each coin you want, and one more"),
                                    QString("Pay exactly %1, %2 or %3 DFCN to each coin's address and 0.01 DFCN to the last one")
                                        .arg(larger[0], larger[1], larger[2]),
                                    QString("with \"Subtract fee from amount\" switched off for every recipient"),
                                    QString("a wallet that was given other active descriptors may not recognise the coins it pays to itself"),
                                    QString("mixes every coin of these amounts that it holds on such an address, whatever the target balance is"),
                                    QString("To keep a coin out, lock it before you start mixing: enable the coin control features in Options, "
                                            "then use Inputs… on the Send page"),
                                    QString("The 0.01 DFCN is ordinary money for the mixing collaterals of these amounts"),
                                    QString("A normal payment can spend these coins like any other coin, mixed or not"),
                                    // Locking is for before mixing starts: a session that has sent its
                                    // collateral counts on its coin until its entry is in.
                                    QString("Lock the ones you want to keep before you start mixing, or choose the coins of the payment yourself"),
                                    // While mixing runs, a payment's coins are to be of no CoinJoin amount at
                                    // all: a session of a standard amount counts on its coin in the same way.
                                    QString("While mixing is running, choose coins that are not of a standard or a larger amount, "
                                            "and neither lock nor spend a coin of these amounts that is waiting to be mixed "
                                            "until its session has ended"),
                                    QString("A session needs other wallets that mix the same amount at the same time")}) {
        QVERIFY2(text.contains(expected), qPrintable(expected));
    }
    QVERIFY2(!text.contains("%1"), qPrintable(text));
    // What the list before it says about staking covers these amounts too.
    QVERIFY2(set.left(section).contains("coins of exactly a standard amount, or of one of the larger amounts below, "
                                        "do not earn staking rewards, whether they are mixed or not"),
             qPrintable(set));
}

void CoinJoinUiTests::helpStatesThePracticalSize()
{
    // The help says what the small amounts mean in practice and what a
    // payment from mixed coins needs, in a section of its own at the end, and
    // names the field's cap and the wallet's default fee limit.
    QDialog dialog;
    Ui::OptionsDialog form;
    form.setupUi(&dialog);
    HelpMessageDialog help(nullptr, HelpMessageDialog::pshelp);
    auto* message = help.findChild<QLabel*>("aboutMessage");
    QVERIFY(message);
    const QString text = message->text();
    const int larger = text.indexOf("<hr><b>Larger amounts</b>");
    const int section = text.indexOf("<hr><b>How much to mix</b>");
    QVERIFY2(larger > 0 && section > larger, qPrintable(text));
    const QString added = text.mid(section);
    for (const QString& expected : {QString("at most about 10 DFCN, so on their own they suit smaller sums"),
                                    QString("The target balance in Options goes up to %1 DFCN").arg(Written(form.coinJoinAmount->maximum())),
                                    QString("a thousand mixed coins or more"),
                                    QString("one payment from coins of the standard amounts can be at most about 6,700 DFCN"),
                                    QString("For more, use the larger amounts"),
                                    QString("A payment from your CoinJoin balance has no change"),
                                    QString("what is left over goes to the fee, and the fee can be at most 0.1 DFCN by default"),
                                    // Both ends of what a payment from larger coins can leave over, and the
                                    // setting that would take the difference from the recipient as well.
                                    QString("switch off \"Subtract fee from amount\" for every recipient and send a little less than the coins add up to"),
                                    // The upper end is the wallet's own limit; 0.1 DFCN is only its default.
                                    QString("the difference is the fee, so it has to cover the normal network fee and stay within the wallet's fee limit")}) {
        QVERIFY2(added.contains(expected), qPrintable(expected));
    }
#ifdef ENABLE_WALLET
    // The fee limit the text names is the wallet's default.
    QCOMPARE(Typed(DEFAULT_TRANSACTION_MAXFEE), QString("0.1"));
#endif
    QVERIFY2(!added.contains("%1"), qPrintable(added));
    QVERIFY2(text.endsWith("</p>"), qPrintable(added));
    // Most of the help is below its first screen: with DEFCON_QT_SCREENSHOTS
    // set, a picture of the whole text and one of the help scrolled to its
    // end, for review.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        help.show();
        QApplication::processEvents();
        QVERIFY(QDir().mkpath(output));
        QVERIFY(message->grab().save(QDir(output).filePath("coinjoin-help-text.png")));
        auto* scroll = help.findChild<QScrollArea*>("scrollArea");
        QVERIFY(scroll);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        QVERIFY(help.grab().save(QDir(output).filePath("coinjoin-help-end.png")));
    }
}

void CoinJoinUiTests::savedTargetKept()
{
#ifndef ENABLE_WALLET
    QSKIP("The CoinJoin options are a wallet build's");
#else
    if (!GUIUtil::fontsLoaded()) QSKIP("The options dialog needs the application fonts; run minimal:enable_fonts");
    // What the model and the dialogs write is put back: the settings, the
    // arguments the model sets from them, the running target.
    struct PutBack {
        interfaces::Node& node;
        const int amount;
        util::Settings args;
        std::map<QString, QVariant> preferences;
        explicit PutBack(interfaces::Node& n) : node(n), amount(n.coinJoinOptions().getAmount())
        {
            gArgs.LockSettings([&](util::Settings& s) { args = s; });
            QSettings settings;
            settings.setFallbacksEnabled(false);
            for (const QString& key : settings.allKeys()) preferences.emplace(key, settings.value(key));
        }
        ~PutBack()
        {
            node.coinJoinOptions().setAmount(amount);
            gArgs.LockSettings([&](util::Settings& s) { s = std::move(args); });
            QSettings settings;
            settings.setFallbacksEnabled(false);
            settings.clear();
            for (const auto& entry : preferences) settings.setValue(entry.first, entry.second);
        }
    } put_back(m_node);
    // A target saved when the field went higher, and a running one apart from
    // it, as -coinjoinamount and setcoinjoinamount set it.
    QSettings().setValue("nCoinJoinAmount", 50000);
    // "Start at login" is read from the system and not from the settings (on
    // Windows the read makes the startup folder where it is missing): the
    // dialog gets a fixed answer for it, and nothing can be written through it.
    struct OptionsButStartup : OptionsModel {
        QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override
        {
            if (index.row() == StartAtStartup) return false;
            return OptionsModel::data(index, role);
        }
        bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override
        {
            if (index.row() == StartAtStartup) return false;
            return OptionsModel::setData(index, value, role);
        }
    } options;
    options.setNode(m_node);
    m_node.coinJoinOptions().setAmount(500);
    struct Shown {
        int value;
        int maximum;
        int edited;
        QString problem;
    };
    // One dialog, as the wallet opens a new one each time: the field as it is
    // shown, the user's value if any, then OK's submit() on the dialog's own
    // mapper with the target field the one widget left mapped. The other rows
    // are not this case's, and some reach past the settings ("start at login",
    // the port mapping that OK starts); the dialog is built without the
    // wallet's part of OK (the wallets' cached blocks).
    const auto submit = [&](std::optional<int> set) {
        // The dialog gives its two proxy validators to its parent to keep: they go with this one.
        QWidget owner;
        OptionsDialog dialog(&owner, /*enableWallet=*/false);
        dialog.setModel(&options);
        auto* field = dialog.findChild<QSpinBox*>("coinJoinAmount");
        const QList<QDataWidgetMapper*> mappers = dialog.findChildren<QDataWidgetMapper*>(QString(), Qt::FindDirectChildrenOnly);
        if (!field || mappers.size() != 1) return Shown{0, 0, 0, "the dialog's target field or its mapper was not found"};
        QDataWidgetMapper* mapper = mappers.first();
        if (mapper->mappedSection(field) != OptionsModel::CoinJoinAmount) return Shown{0, 0, 0, "the target field is not mapped to its row"};
        Shown shown{field->value(), field->maximum(), 0, QString()};
        if (set) field->setValue(*set);
        shown.edited = field->value();
        for (int other = 0; other < options.rowCount(); ++other) {
            if (other == OptionsModel::CoinJoinAmount) continue;
            while (QWidget* widget = mapper->mappedWidgetAt(other)) mapper->removeMapping(widget);
        }
        if (!mapper->submit()) shown.problem = "the mapper did not submit";
        return shown;
    };
    // Left alone, OK changes neither the running target nor the saved one,
    // and the field showed the saved target.
    const Shown first = submit(std::nullopt);
    QVERIFY2(first.problem.isEmpty(), qPrintable(first.problem));
    QCOMPARE(m_node.coinJoinOptions().getAmount(), 500);
    QCOMPARE(QSettings().value("nCoinJoinAmount").toInt(), 50000);
    QCOMPARE(first.value, 50000);
    QCOMPARE(first.maximum, 50000);
    // Any change brings the cap back: a new target above it stops there, and
    // that is the running and the saved one.
    const Shown second = submit(20000);
    QVERIFY2(second.problem.isEmpty(), qPrintable(second.problem));
    QCOMPARE(m_node.coinJoinOptions().getAmount(), 10000);
    QCOMPARE(QSettings().value("nCoinJoinAmount").toInt(), 10000);
    QCOMPARE(second.edited, 10000);
    // Under the cap, the next dialog has the cap from the start.
    const Shown third = submit(8000);
    QVERIFY2(third.problem.isEmpty(), qPrintable(third.problem));
    QCOMPARE(third.value, 10000);
    QCOMPARE(third.maximum, 10000);
    QCOMPARE(m_node.coinJoinOptions().getAmount(), 8000);
    QCOMPARE(QSettings().value("nCoinJoinAmount").toInt(), 8000);
#endif
}
