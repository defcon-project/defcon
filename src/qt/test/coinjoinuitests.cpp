// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <qt/test/coinjoinuitests.h>

#include <interfaces/node.h>
#include <qt/forms/ui_optionsdialog.h>
#include <qt/guiutil.h>
#include <qt/optionsdialog.h>
#include <qt/optionsmodel.h>
#include <qt/utilitydialog.h>
#include <util/system.h>

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
#include <QWidget>

#include <map>
#include <optional>
#include <utility>

namespace {
//! An amount of whole DFCN as the English texts write it ("10,000").
QString Written(int amount) { return QLocale(QLocale::English).toString(amount); }
} // namespace

void CoinJoinUiTests::initTestCase()
{
    // The options dialog needs the application fonts (savedTargetKept).
    if (!GUIUtil::fontsLoaded()) GUIUtil::loadFonts();
}

void CoinJoinUiTests::targetFieldIsCapped()
{
    // The CoinJoin amounts are the small ones of the chain this one forked
    // from (at most about 10 DFCN), and the largest one has no count limit: a
    // target in the millions, which the field allowed, splits the balance into
    // that many coins of 10 DFCN. The options form caps the field and says
    // why. The form is read on its own: the dialog raises the cap only for a
    // target saved above it (savedTargetKept), and it cannot be built without
    // the application fonts.
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
    // The label keeps the tooltip it had.
    QCOMPARE(form.lblCoinJoinAmountText->toolTip(), QString("This amount acts as a threshold to turn off mixing once it's reached."));
}

void CoinJoinUiTests::helpStatesThePracticalSize()
{
    // The help says what the small amounts mean in practice, in a section of
    // its own after the list (the text before it is unchanged, so its
    // translations stay valid), and names the field's cap.
    QDialog dialog;
    Ui::OptionsDialog form;
    form.setupUi(&dialog);
    HelpMessageDialog help(nullptr, HelpMessageDialog::pshelp);
    auto* message = help.findChild<QLabel*>("aboutMessage");
    QVERIFY(message);
    const QString text = message->text();
    const int list_end = text.indexOf("</ul>");
    const int section = text.indexOf("<hr><b>How much to mix</b>");
    QVERIFY2(list_end > 0 && section > list_end, qPrintable(text));
    const QString added = text.mid(section);
    for (const QString& expected : {QString("at most about 10 DFCN, so CoinJoin is meant for smaller sums"),
                                    QString("one payment from mixed coins can be at most about 6,700 DFCN"),
                                    QString("send a larger amount in several payments"),
                                    QString("The target balance in Options goes up to %1 DFCN").arg(Written(form.coinJoinAmount->maximum())),
                                    QString("a thousand mixed coins or more")}) {
        QVERIFY2(added.contains(expected), qPrintable(expected));
    }
    QVERIFY2(!added.contains("%1"), qPrintable(added));
    QVERIFY2(text.endsWith("</p>"), qPrintable(added));
    // The section is below the help's first screen: with DEFCON_QT_SCREENSHOTS
    // set, a picture of the help scrolled to its end, for review.
    const QString output = qEnvironmentVariable("DEFCON_QT_SCREENSHOTS");
    if (!output.isEmpty()) {
        help.show();
        QApplication::processEvents();
        auto* scroll = help.findChild<QScrollArea*>("scrollArea");
        QVERIFY(scroll);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QApplication::processEvents();
        QVERIFY(QDir().mkpath(output));
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
