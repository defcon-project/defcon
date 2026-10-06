// Copyright (c) 2011-2019 The Bitcoin Core developers
// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef HAVE_CONFIG_H
#include <config/bitcoin-config.h>
#endif

#include <qt/transactiondesc.h>

#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/paymentserver.h>
#include <qt/transactionrecord.h>

#include <consensus/consensus.h>
#include <chainparams.h>
#include <key_io.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <script/script.h>
#include <util/system.h>
#include <validation.h>
#include <wallet/ismine.h>

#include <algorithm>
#include <stdint.h>
#include <string>
#include <set>

#include <QLatin1String>

QString TransactionDesc::FormatTxStatus(const interfaces::WalletTx& wtx, const interfaces::WalletTxStatus& status, bool inMempool, int numBlocks)
{
    if (!status.is_final)
    {
        if (wtx.tx->nLockTime < LOCKTIME_THRESHOLD)
            return tr("Open for %n more block(s)", "", wtx.tx->nLockTime - numBlocks);
        else
            return tr("Open until %1").arg(GUIUtil::dateTimeStr(wtx.tx->nLockTime));
    }
    else
    {
        int nDepth = status.depth_in_main_chain;
        if (nDepth < 0) return tr("conflicted");

        QString strTxStatus;
        bool fChainLocked = status.is_chainlocked;

        if (nDepth == 0) {
            const QString abandoned{status.is_abandoned ? QLatin1String(", ") + tr("abandoned") : QString()};
            strTxStatus = tr("0/unconfirmed, %1").arg((inMempool ? tr("in memory pool") : tr("not in memory pool"))) + abandoned;
        } else if (!fChainLocked && nDepth < 6) {
            strTxStatus = tr("%1/unconfirmed").arg(nDepth);
        } else {
            strTxStatus = tr("%1 confirmations").arg(nDepth);
            if (fChainLocked) {
                strTxStatus += QLatin1String(", ") + tr("locked via ChainLocks");
                return strTxStatus;
            }
        }

        if (status.is_islocked) {
            strTxStatus += QLatin1String(", ") + tr("verified via InstantSend");
        }

        return strTxStatus;
    }
}

QString TransactionDesc::FormatStakeDetails(const interfaces::WalletTx& wtx,
                                            const interfaces::WalletTxStatus& status,
                                            const std::vector<CTxOut>& previous_outputs,
                                            const QStringList& input_labels,
                                            const QStringList& output_labels, int unit)
{
    if (!wtx.tx || !wtx.is_coinstake) return {};
    const CTransaction& tx = *wtx.tx;
    const int inputs = static_cast<int>(tx.vin.size());
    const int outputs = std::count_if(tx.vout.begin(), tx.vout.end(), [](const CTxOut& out) { return out.nValue > 0; });
    const int reduction = inputs - outputs;
    const bool accepted = status.is_in_main_chain && status.depth_in_main_chain > 0;
    QString html = "<hr><h3>" + (inputs > 1 ? tr("Staking with consolidation") : tr("Staking details")) + "</h3>";
    QString summary;
    if (reduction > 0) {
        summary = outputs == 1
            ? tr("%1 existing outputs were used and combined into 1 new output, reducing the UTXO count by %2.").arg(inputs).arg(reduction)
            : tr("%1 existing outputs were used and combined into %2 new outputs, reducing the UTXO count by %3.").arg(inputs).arg(outputs).arg(reduction);
    } else if (inputs == 1 && outputs == 1) {
        summary = tr("1 existing output was used to create 1 new output. The UTXO count is unchanged.");
    } else if (reduction == 0) {
        summary = tr("%1 existing outputs were used to create %2 new outputs. The UTXO count is unchanged.").arg(inputs).arg(outputs);
    } else {
        summary = inputs == 1
            ? tr("1 existing output was split into %1 new outputs, increasing the UTXO count by %2.").arg(outputs).arg(-reduction)
            : tr("%1 existing outputs were used to create %2 new outputs, increasing the UTXO count by %3.").arg(inputs).arg(outputs).arg(-reduction);
    }
    html += "<p>" + summary + "</p>";
    if (!accepted) html += "<p><b>" + tr("This transaction is not currently confirmed in the active chain. The changes below are not a confirmed consolidation.") + "</b></p>";

    CAmount total_in = 0;
    bool complete = previous_outputs.size() == tx.vin.size();
    bool all_own = wtx.txin_is_mine.size() == tx.vin.size() && wtx.txout_is_mine.size() == tx.vout.size();
    std::set<std::string> addresses;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        if (i >= previous_outputs.size() || previous_outputs[i].IsNull() || !MoneyRange(previous_outputs[i].nValue) ||
            total_in > MAX_MONEY - previous_outputs[i].nValue) {
            complete = false;
        } else {
            total_in += previous_outputs[i].nValue;
            CTxDestination dest;
            if (ExtractDestination(previous_outputs[i].scriptPubKey, dest)) addresses.insert(EncodeDestination(dest));
        }
        all_own &= i < wtx.txin_is_mine.size() && (wtx.txin_is_mine[i] & ISMINE_SPENDABLE);
    }
    CAmount total_out = 0;
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        if (!MoneyRange(tx.vout[i].nValue) || total_out > MAX_MONEY - tx.vout[i].nValue) return {};
        total_out += tx.vout[i].nValue;
        if (tx.vout[i].nValue > 0) all_own &= i < wtx.txout_is_mine.size() && (wtx.txout_is_mine[i] & ISMINE_SPENDABLE);
    }
    const auto amount = [unit](CAmount value) { return BitcoinUnits::formatHtmlWithUnit(unit, value); };
    html += "<p><b>" + tr("Existing coins used") + ":</b> " + (complete ? amount(total_in) : tr("Unknown: some previous outputs are unavailable in this wallet")) + "<br>";
    html += "<b>" + tr("Staking reward") + ":</b> " + (complete && total_out >= total_in ? amount(total_out - total_in) : tr("Cannot be calculated from the available input records")) + "<br>";
    html += "<b>" + tr("New output total") + ":</b> " + amount(total_out) + "</p>";
    if (complete && all_own && total_out >= total_in) {
        html += "<p>" + tr("No separate consolidation fee was deducted.") + "</p>";
    }
    if (!all_own) html += "<p>" + tr("The amounts below describe the whole transaction. This wallet may own or watch only part of it.") + "</p>";
    if (addresses.size() > 1) {
        html += "<p><b>" + tr("Multiple source addresses") + ":</b> " + tr("Inputs from %1 addresses were combined, linking them on the blockchain.").arg(addresses.size()) + "</p>";
    }
    html += "<h4>" + tr("Inputs used") + "</h4>";
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        html += "<p><b>" + QString::number(i + 1) + ". " + (i == 0 ? tr("Winning stake input") : tr("Additional combined input")) + "</b><br>";
        if (i < previous_outputs.size() && !previous_outputs[i].IsNull()) {
            const CTxOut& prev = previous_outputs[i];
            html += tr("Amount") + ": " + amount(prev.nValue) + "<br>";
            CTxDestination dest;
            if (ExtractDestination(prev.scriptPubKey, dest)) html += tr("Address") + ": " + GUIUtil::HtmlEscape(EncodeDestination(dest)) + "<br>";
            if (i < static_cast<size_t>(input_labels.size()) && !input_labels[i].isEmpty()) html += tr("Label") + ": " + GUIUtil::HtmlEscape(input_labels[i]) + "<br>";
        } else {
            html += tr("The original amount and address are unavailable in this wallet's history.") + "<br>";
        }
        html += tr("Previous transaction") + ": " + QString::fromStdString(tx.vin[i].prevout.hash.ToString()) + "<br>" +
                tr("Output index") + ": " + QString::number(tx.vin[i].prevout.n) + "</p>";
    }
    html += "<h4>" + (outputs == 1 ? tr("New output") : tr("New outputs")) + "</h4>";
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        const CTxOut& out = tx.vout[i];
        if (out.nValue <= 0) continue;
        html += "<p><b>" + (outputs == 1 ? QString() : tr("Output %1").arg(i) + ": ") + amount(out.nValue) + "</b><br>";
        CTxDestination dest;
        if (ExtractDestination(out.scriptPubKey, dest)) html += tr("Destination address") + ": " + GUIUtil::HtmlEscape(EncodeDestination(dest)) + "<br>";
        if (i < static_cast<size_t>(output_labels.size()) && !output_labels[i].isEmpty()) html += tr("Label") + ": " + GUIUtil::HtmlEscape(output_labels[i]) + "<br>";
        html += "</p>";
    }
    const QString winning_explanation = inputs == 1 ? tr("The first input won the stake.")
        : inputs == 2 ? tr("The first input won the stake. The additional input was combined afterwards and did not affect the chance of winning this block.")
                      : tr("The first input won the stake. The additional inputs were combined afterwards and did not affect the chance of winning this block.");
    html += "<p>" + winning_explanation + "</p>";
    const int64_t min_age = Params().GetConsensus().stakeAgeRange[0];
    const QString age = min_age == 60 ? tr("1 minute")
        : min_age % 60 == 0 ? tr("%1 minutes").arg(min_age / 60)
                            : GUIUtil::formatNiceTimeOffset(min_age);
    html += "<p>" + (outputs == 1
        ? tr("The new output can stake again after at least <b>%1 confirmations</b> and <b>%2</b> from the block timestamp.")
        : tr("The new outputs can stake again after at least <b>%1 confirmations</b> and <b>%2</b> from the block timestamp."))
        .arg(COINBASE_MATURITY + 2).arg(age) + "</p><hr>";
    return html;
}

QString TransactionDesc::toHTML(interfaces::Node& node, interfaces::Wallet& wallet, TransactionRecord *rec, int unit)
{
    int numBlocks;
    interfaces::WalletTxStatus status;
    interfaces::WalletOrderForm orderForm;
    bool inMempool;
    interfaces::WalletTx wtx = wallet.getWalletTxDetails(rec->hash, status, orderForm, inMempool, numBlocks);

    QString strHTML;

    strHTML.reserve(4000);
    strHTML += "<html>";

    int64_t nTime = wtx.time;
    CAmount nCredit = wtx.credit;
    CAmount nDebit = wtx.debit;
    CAmount nNet = nCredit - nDebit;

    strHTML += "<b>" + tr("Status") + ":</b> " + FormatTxStatus(wtx, status, inMempool, numBlocks);
    strHTML += "<br>";

    strHTML += "<b>" + tr("Date") + ":</b> " + (nTime ? GUIUtil::dateTimeStr(nTime) : "") + "<br>";

    //
    // From
    //
    if (wtx.is_masternode_reward)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + tr("Masternode Reward") + "<br>";
    }
    else if (wtx.is_coinbase)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + tr("Generated") + "<br>";
    }
    else if (wtx.is_coinstake)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + (wtx.tx->vin.size() > 1 ? tr("Staked (combined)") : tr("Staked")) + "<br>";
    }
    else if (wtx.is_platform_transfer)
    {
        strHTML += "<b>" + tr("Source") + ":</b> " + tr("Platform Transfer") + "<br>";
    }
    else if (wtx.value_map.count("from") && !wtx.value_map["from"].empty())
    {
        // Online transaction
        strHTML += "<b>" + tr("From") + ":</b> " + GUIUtil::HtmlEscape(wtx.value_map["from"]) + "<br>";
    }
    else
    {
        // Offline transaction
        if (nNet > 0)
        {
            // Credit
            CTxDestination address = DecodeDestination(rec->strAddress);
            if (IsValidDestination(address)) {
                std::string name;
                isminetype ismine;
                if (wallet.getAddress(address, &name, &ismine, /* purpose= */ nullptr))
                {
                    strHTML += "<b>" + tr("From") + ":</b> " + tr("unknown") + "<br>";
                    strHTML += "<b>" + tr("To") + ":</b> ";
                    strHTML += GUIUtil::HtmlEscape(rec->strAddress);
                    QString addressOwned = ismine == ISMINE_SPENDABLE ? tr("own address") : tr("watch-only");
                    if (!name.empty())
                        strHTML += " (" + addressOwned + ", " + tr("label") + ": " + GUIUtil::HtmlEscape(name) + ")";
                    else
                        strHTML += " (" + addressOwned + ")";
                    strHTML += "<br>";
                }
            }
        }
    }

    //
    // To
    //
    if (wtx.value_map.count("to") && !wtx.value_map["to"].empty())
    {
        // Online transaction
        std::string strAddress = wtx.value_map["to"];
        strHTML += "<b>" + tr("To") + ":</b> ";
        CTxDestination dest = DecodeDestination(strAddress);
        std::string name;
        if (wallet.getAddress(
                dest, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
            strHTML += GUIUtil::HtmlEscape(name) + " ";
        strHTML += GUIUtil::HtmlEscape(strAddress) + "<br>";
    }

    //
    // Amount
    //
    if (wtx.is_coinstake) {
        std::vector<CTxOut> previous;
        QStringList input_labels;
        QStringList output_labels;
        const auto labelFor = [&wallet](const CTxOut& out) {
            CTxDestination dest;
            std::string label;
            if (!out.IsNull() && ExtractDestination(out.scriptPubKey, dest)) wallet.getAddress(dest, &label, nullptr, nullptr);
            return QString::fromStdString(label);
        };
        // Read the wallet history, not the UTXO set: confirmed inputs have
        // already been spent, and remain explainable after restart/reindex.
        for (const CTxIn& input : wtx.tx->vin) {
            const interfaces::WalletTx parent = wallet.getWalletTx(input.prevout.hash);
            CTxOut prev;
            if (parent.tx && input.prevout.n < parent.tx->vout.size()) prev = parent.tx->vout[input.prevout.n];
            previous.push_back(prev);
            input_labels.push_back(labelFor(prev));
        }
        for (const CTxOut& out : wtx.tx->vout) output_labels.push_back(labelFor(out));
        strHTML += FormatStakeDetails(wtx, status, previous, input_labels, output_labels, unit);
    }
    else if (wtx.is_coinbase && nCredit == 0)
    {
        //
        // Coinbase
        //
        CAmount nUnmatured = 0;
        for (const CTxOut& txout : wtx.tx->vout)
            nUnmatured += wallet.getCredit(txout, ISMINE_ALL);
        strHTML += "<b>" + tr("Credit") + ":</b> ";
        if (status.is_in_main_chain)
            strHTML += BitcoinUnits::formatHtmlWithUnit(unit, nUnmatured)+ " (" + tr("matures in %n more block(s)", "", status.blocks_to_maturity) + ")";
        else
            strHTML += "(" + tr("not accepted") + ")";
        strHTML += "<br>";
    }
    else if (nNet > 0)
    {
        //
        // Credit
        //
        strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, nNet) + "<br>";
    }
    else
    {
        isminetype fAllFromMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txin_is_mine)
        {
            if(fAllFromMe > mine) fAllFromMe = mine;
        }

        isminetype fAllToMe = ISMINE_SPENDABLE;
        for (const isminetype mine : wtx.txout_is_mine)
        {
            if(fAllToMe > mine) fAllToMe = mine;
        }

        if (fAllFromMe)
        {
            if(fAllFromMe & ISMINE_WATCH_ONLY)
                strHTML += "<b>" + tr("From") + ":</b> " + tr("watch-only") + "<br>";

            //
            // Debit
            //
            auto mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout)
            {
                // Ignore change
                isminetype toSelf = *(mine++);
                if ((toSelf == ISMINE_SPENDABLE) && (fAllFromMe == ISMINE_SPENDABLE))
                    continue;

                if (!wtx.value_map.count("to") || wtx.value_map["to"].empty())
                {
                    // Offline transaction
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address))
                    {
                        strHTML += "<b>" + tr("To") + ":</b> ";
                        std::string name;
                        if (wallet.getAddress(
                                address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += GUIUtil::HtmlEscape(EncodeDestination(address));
                        if(toSelf == ISMINE_SPENDABLE)
                            strHTML += " (own address)";
                        else if(toSelf & ISMINE_WATCH_ONLY)
                            strHTML += " (watch-only)";
                        strHTML += "<br>";
                    }
                }

                strHTML += "<b>" + tr("Debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, -txout.nValue) + "<br>";
                if(toSelf)
                    strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, txout.nValue) + "<br>";
            }

            if (fAllToMe)
            {
                // Payment to self
                CAmount nChange = wtx.change;
                CAmount nValue = nCredit - nChange;
                strHTML += "<b>" + tr("Total debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, -nValue) + "<br>";
                strHTML += "<b>" + tr("Total credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, nValue) + "<br>";
            }

            CAmount nTxFee = nDebit - wtx.tx->GetValueOut();
            if (nTxFee > 0)
                strHTML += "<b>" + tr("Transaction fee") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, -nTxFee) + "<br>";
        }
        else
        {
            //
            // Mixed debit transaction
            //
            auto mine = wtx.txin_is_mine.begin();
            for (const CTxIn& txin : wtx.tx->vin) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, -wallet.getDebit(txin, ISMINE_ALL)) + "<br>";
                }
            }
            mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout) {
                if (*(mine++)) {
                    strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, wallet.getCredit(txout, ISMINE_ALL)) + "<br>";
                }
            }
        }
    }

    if (!wtx.is_coinstake) strHTML += "<b>" + tr("Net amount") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, nNet, true) + "<br>";

    //
    // Message
    //
    if (wtx.value_map.count("message") && !wtx.value_map["message"].empty())
        strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["message"], true) + "<br>";
    if (wtx.value_map.count("comment") && !wtx.value_map["comment"].empty())
        strHTML += "<br><b>" + tr("Comment") + ":</b><br>" + GUIUtil::HtmlEscape(wtx.value_map["comment"], true) + "<br>";

    strHTML += "<b>" + tr("Transaction ID") + ":</b> " + rec->getTxHash() + "<br>";
    strHTML += "<b>" + tr("Output index") + ":</b> " + QString::number(rec->getOutputIndex()) + "<br>";
    strHTML += "<b>" + (wtx.is_coinstake ? tr("Transaction size") : tr("Transaction total size")) + ":</b> " + QString::number(wtx.tx->GetTotalSize()) + " bytes<br>";

    // Message from normal dash:URI (dash:XyZ...?message=example)
    for (const std::pair<std::string, std::string>& r : orderForm) {
        if (r.first == "Message")
            strHTML += "<br><b>" + tr("Message") + ":</b><br>" + GUIUtil::HtmlEscape(r.second, true) + "<br>";
    }

    if (wtx.is_coinstake) {
        strHTML += "<br>" + tr("Staked coins mature after %1 blocks before they can be spent.").arg(COINBASE_MATURITY + 1) + "<br>";
    } else if (wtx.is_coinbase) {
        const quint32 numBlocksToMaturity = COINBASE_MATURITY + 1;
        strHTML += "<br>" + tr("Generated");
        strHTML += tr(" coins must mature %1 blocks before they can be spent. When you created this block, it was broadcast to the network to be added to the block chain. If it fails to get into the chain, its state will change to \"not accepted\" and it won't be spendable. This may occasionally happen if another node creates a block within a few seconds of yours.").arg(QString::number(numBlocksToMaturity)) + "<br>";
    }

    //
    // Debug view
    //
    if (node.getLogCategories() != BCLog::NONE)
    {
        strHTML += "<hr><br>" + tr("Debug information") + "<br><br>";
        for (const CTxIn& txin : wtx.tx->vin)
            if(wallet.txinIsMine(txin))
                strHTML += "<b>" + tr("Debit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, -wallet.getDebit(txin, ISMINE_ALL)) + "<br>";
        for (const CTxOut& txout : wtx.tx->vout)
            if(wallet.txoutIsMine(txout))
                strHTML += "<b>" + tr("Credit") + ":</b> " + BitcoinUnits::formatHtmlWithUnit(unit, wallet.getCredit(txout, ISMINE_ALL)) + "<br>";

        strHTML += "<br><b>" + tr("Transaction") + ":</b><br>";
        strHTML += GUIUtil::HtmlEscape(wtx.tx->ToString(), true);

        strHTML += "<br><b>" + tr("Inputs") + ":</b>";
        strHTML += "<ul>";

        for (const CTxIn& txin : wtx.tx->vin)
        {
            COutPoint prevout = txin.prevout;

            Coin prev;
            if(node.getUnspentOutput(prevout, prev))
            {
                {
                    strHTML += "<li>";
                    const CTxOut& txout = prev.out;
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address))
                    {
                        std::string name;
                        if (wallet.getAddress(address, &name, /* is_mine= */ nullptr, /* purpose= */ nullptr) && !name.empty())
                            strHTML += GUIUtil::HtmlEscape(name) + " ";
                        strHTML += QString::fromStdString(EncodeDestination(address));
                    }
                    strHTML = strHTML + " " + tr("Amount") + "=" + BitcoinUnits::formatHtmlWithUnit(unit, txout.nValue);
                    strHTML = strHTML + " IsMine=" + (wallet.txoutIsMine(txout) & ISMINE_SPENDABLE ? tr("true") : tr("false"));
                    strHTML = strHTML + " IsWatchOnly=" + (wallet.txoutIsMine(txout) & ISMINE_WATCH_ONLY ? tr("true") : tr("false")) + "</li>";
                }
            }
        }

        strHTML += "</ul>";
    }

    strHTML += "</font></html>";
    return strHTML;
}
