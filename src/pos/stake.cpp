// Copyright (c) 2025 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/stake.h>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <node/miner.h>
#include <pos/minter.h>
#include <pow.h>
#include <rpc/util.h>
#include <wallet/scriptpubkeyman.h>

extern std::atomic<bool> fStopMinerProc;

uint64_t CStakeWallet::GetStakeWeight(int64_t nTime, int nHeight) const
{
    // Choose coins to use
    CAmount nBalance = wallet->GetBalance().m_mine_trusted;
    if (nBalance <= wallet->nReserveBalance) {
        return 0;
    }

    CAmount nValueIn = 0;
    std::vector<const CWalletTx*> vwtxPrev;
    std::set<std::pair<const CWalletTx*,unsigned int> > setCoins;

    CAmount nTargetValue = nBalance - wallet->nReserveBalance;
    if (!SelectCoinsForStaking(nTargetValue, nTime, nHeight, setCoins, nValueIn)) {
        return 0;
    }

    if (setCoins.empty()) {
        return 0;
    }

    uint64_t nWeight = 0;
    for(std::pair<const CWalletTx* ,unsigned int> pcoin : setCoins) {
        if (pcoin.first->GetDepthInMainChain() >= COINBASE_MATURITY) {
            nWeight += pcoin.first->tx->vout[pcoin.second].nValue;
        }
    }

    return nWeight;
}

bool CStakeWallet::SelectCoinsForStaking(CAmount nTargetValue, int64_t nTime, int nHeight, std::set<std::pair<const CWalletTx*, unsigned int>>& setCoinsRet, CAmount& nValueRet) const
{
    std::vector<COutput> vCoins;

    {
        LOCK(wallet->cs_wallet);
        wallet->AvailableCoins(vCoins, nullptr, params.stakeValueRange[0], params.stakeValueRange[1]);
    }

    setCoinsRet.clear();
    nValueRet = 0;
    int nRequiredDepth = COINBASE_MATURITY + 1;

    for (const auto& output : vCoins)
    {
        const CWalletTx* pcoin = output.tx;
        int i = output.i;

        // Stop if we've chosen enough inputs
        if (nValueRet >= nTargetValue) {
            break;
        }

        // Determine depth
        int nDepth;
        {
            LOCK(wallet->cs_wallet);
            nDepth = pcoin->GetDepthInMainChain();
        }

        // If coinbase/stake, ensure it has reached maturity
        bool nGenerated = pcoin->IsCoinBase() || pcoin->IsCoinStake();
        if (nGenerated && (nDepth < nRequiredDepth)) {
            continue;
        }

        // Do not include BLS addresses
        std::vector<valtype> vSolutions;
        CScript scriptPubKeyKernel = pcoin->tx->vout[i].scriptPubKey;
        TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);
        if (whichType == TxoutType::BLSPUBKEY) {
            continue;
        }

        // Skip inputs that dont meet age, value requirements or are collaterals
        CAmount inputValue = pcoin->tx->vout[i].nValue;
        int64_t inputAge = GetTime() - pcoin->GetTxTime();
        if (inputValue < params.stakeValueRange[0] || inputValue > params.stakeValueRange[1]) {
            continue;
        }
        if (inputValue == params.regularMnCollateral || inputValue == params.evoMnCollateral) {
            continue;
        }
        if (inputAge < params.stakeAgeRange[0] || inputAge > params.stakeAgeRange[1]) {
            continue;
        }

        std::pair<int64_t, std::pair<const CWalletTx*, unsigned int>> coin = std::make_pair(inputValue, std::make_pair(pcoin, i));
        if (inputValue >= nTargetValue) {
            // If input value is greater or equal to target then simply insert
            //    it into the current subset and exit
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            break;
        } else {
            if (inputValue < nTargetValue + CENT) {
                setCoinsRet.insert(coin.second);
                nValueRet += coin.first;
            }
        }
    }

    return true;
}

namespace {
/**
 * A signing provider for `script` that carries private keys, whatever kind of
 * wallet this is.
 *
 * Staking needs real private keys twice: once for the block signature and once
 * to sign the coinstake inputs. A LegacyScriptPubKeyMan hands back a full
 * keystore, but a descriptor wallet withholds private material unless it is
 * asked for it, and returns nothing at all for a script it does not own.
 */
std::unique_ptr<SigningProvider> GetStakingSigningProvider(const CWallet& wallet, const CScript& script)
{
    for (ScriptPubKeyMan* spk_man : wallet.GetAllScriptPubKeyMans()) {
        if (const auto* desc_man = dynamic_cast<const DescriptorScriptPubKeyMan*>(spk_man)) {
            if (auto provider = desc_man->GetSigningProvider(script, /*include_private=*/true)) {
                return provider;
            }
            continue;
        }
        if (auto provider = spk_man->GetSolvingProvider(script)) {
            return provider;
        }
    }
    return nullptr;
}
} // namespace

bool CStakeWallet::CreateCoinStake(CChainState& chain_state, CBlockIndex* pindexPrev, unsigned int nBits, int64_t nTime, int nBlockHeight, int64_t nFees, CMutableTransaction& txNew, CKey& key)
{
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    CAmount nBalance = wallet->GetAvailableBalance();
    if (nBalance <= wallet->nReserveBalance) {
        return false;
    }

    // Ensure txn is empty
    txNew.vin.clear();
    txNew.vout.clear();

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    // Choose coins to use
    CAmount nValueIn = 0;
    std::vector<const CWalletTx*> vwtxPrev;
    std::set<std::pair<const CWalletTx*, unsigned int>> setCoins;
    if (!SelectCoinsForStaking(nBalance - wallet->nReserveBalance, nTime, nBlockHeight, setCoins, nValueIn)) {
        return false;
    }

    if (setCoins.empty()) {
        return false;
    }

    CAmount nCredit = 0;
    CScript scriptPubKeyKernel;
    std::set<std::pair<const CWalletTx*, unsigned int>>::iterator it = setCoins.begin();

    for (; it != setCoins.end(); ++it)
    {
        auto pcoin = *it;
        if (fStopMinerProc)
            return false;

        int64_t nBlockTime;
        COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);
        if (CheckKernel(chain_state, pindexPrev, nBits, nTime, prevoutStake, &nBlockTime))
        {
            LOCK(wallet->cs_wallet);

            // Found a kernel
            LogPrint(BCLog::POS, "%s: Kernel found.\n", __func__);

            CTxOut kernelOut = pcoin.first->tx->vout[pcoin.second];

            CScript scriptPubKeyOut;
            std::vector<valtype> vSolutions;
            CScript scriptPubKeyKernel = pcoin.first->tx->vout[pcoin.second].scriptPubKey;
            TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);

            LogPrint(BCLog::POS, "%s: parsed kernel type=%s\n", __func__, GetTxnOutputType(whichType));

            const std::unique_ptr<SigningProvider> kernel_provider =
                GetStakingSigningProvider(*wallet, scriptPubKeyKernel);
            if (!kernel_provider) {
                LogPrint(BCLog::POS, "%s: no signing provider for kernel type=%s\n", __func__, GetTxnOutputType(whichType));
                break;
            }

            if (whichType == TxoutType::PUBKEYHASH) {

                uint160 hash160(vSolutions[0]);
                CKeyID pubKeyHash(hash160);
                if (!kernel_provider->GetKey(pubKeyHash, key)) {
                    LogPrint(BCLog::POS, "%s: failed to get key for kernel type=%s\n", __func__, GetTxnOutputType(whichType));
                    break;
                }
                scriptPubKeyOut << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;

            } else if (whichType == TxoutType::PUBKEY) {

                valtype& vchPubKey = vSolutions[0];
                CPubKey pubKey(vchPubKey);
                uint160 hash160(Hash160(vchPubKey));
                CKeyID pubKeyHash(hash160);
                if (!kernel_provider->GetKey(pubKeyHash, key)) {
                    LogPrint(BCLog::POS, "%s: failed to get key for kernel type=%s\n", __func__, GetTxnOutputType(whichType));
                    break;
                }
                if (key.GetPubKey() != pubKey) {
                    LogPrint(BCLog::POS, "%s: invalid key for kernel type=%s\n", __func__, GetTxnOutputType(whichType));
                    break;
                }
                scriptPubKeyOut = scriptPubKeyKernel;

            } else if (whichType == TxoutType::BLSPUBKEY) {

                LogPrint(BCLog::POS, "%s: staking on BLS kernels is forbidden", __func__);
                continue;

            } else {

                LogPrint(BCLog::POS, "%s: no support for kernel type=%s\n", __func__, GetTxnOutputType(whichType));
                continue;
            }

            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->tx->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin.first);
            CTxOut out(0, scriptPubKeyOut);
            txNew.vout.push_back(out);

            LogPrint(BCLog::POS, "%s: Added kernel.\n", __func__);

            setCoins.erase(it);
            break;
        }
    }

    if (nCredit == 0 || nCredit > nBalance - wallet->nReserveBalance) {
        return false;
    }

    // Get block reward
    CAmount nReward = GetProofOfStakeReward();
    if (nReward < 0) {
        return false;
    }

    nCredit += nReward;
    {
        if (nCredit >= wallet->nStakeSplitThreshold) {
            txNew.vout.push_back(CTxOut(0, txNew.vout[1].scriptPubKey));
        }

        // Set output amount
        if (txNew.vout.size() == 3)
        {
            txNew.vout[1].nValue = (nCredit / 2 / CENT) * CENT;
            txNew.vout[2].nValue = nCredit - txNew.vout[1].nValue;
        }
        else
        {
            txNew.vout[1].nValue = nCredit;
        }
    }

    // Sign
    int nIn = 0;
    LOCK(wallet->cs_wallet);
    for (const auto& pcoin : vwtxPrev)
    {
        uint32_t nPrev = txNew.vin[nIn].prevout.n;
        CTxOut prevOut = pcoin->tx->vout[nPrev];
        CAmount amount = prevOut.nValue;
        CScript& scriptPubKeyOut = prevOut.scriptPubKey;

        SignatureData sigdata;
        const std::unique_ptr<SigningProvider> provider = GetStakingSigningProvider(*wallet, scriptPubKeyOut);
        if (!provider) {
            LogPrint(BCLog::POS, "%s: no signing provider for input %d.", __func__, nIn);
            return false;
        }
        if (!ProduceSignature(*provider, MutableTransactionSignatureCreator(&txNew, nIn, amount, SIGHASH_ALL), scriptPubKeyOut, sigdata)) {
            LogPrint(BCLog::POS, "%s: ProduceSignature failed.", __func__);
            return false;
        }

        UpdateInput(txNew.vin[nIn], sigdata);
        nIn++;
    }

    // Limit size
    unsigned int nBytes = ::GetSerializeSize(txNew, PROTOCOL_VERSION);
    if (nBytes >= MaxBlockSize() / 5) {
        LogPrint(BCLog::POS, "%s: Exceeded coinstake size limit.", __func__);
        return false;
    }

    // Successfully generated coinstake
    return true;
}

bool CStakeWallet::SignBlock(CChainState& chain_state, CBlockTemplate* pblocktemplate, int nHeight, int64_t nSearchTime)
{
    LogPrint(BCLog::POS, "%s, Height %d\n", __func__, nHeight);

    assert(pblocktemplate);
    CBlock* pblock = &pblocktemplate->block;
    assert(pblock);
    if (pblock->vtx.size() < 1) {
        LogPrint(BCLog::POS, "%s: Malformed block.", __func__);
        return false;
    }

    CAmount nFees = -pblocktemplate->vTxFees[0];
    CBlockIndex* pindexPrev = chain_state.m_chainman.ActiveChain().Tip();

    CKey key;
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());
    LogPrint(BCLog::POS, "%s, nBits %d\n", __func__, pblock->nBits);

    CMutableTransaction txCoinStake;
    if (CreateCoinStake(chain_state, pindexPrev, pblock->nBits, nSearchTime, nHeight, nFees, txCoinStake, key)) {

        LogPrint(BCLog::POS, "%s: Kernel found.\n", __func__);

        if (nSearchTime >= pindexPrev->GetPastTimeLimit() + 1) {

            // make sure coinstake would meet timestamp protocol
            //    as it would be the same as the block timestamp
            pblock->nTime = nSearchTime;

            // Insert coinstake as vtx[1]
            pblock->vtx.insert(pblock->vtx.begin() + 1, MakeTransactionRef(txCoinStake));

            bool mutated;
            pblock->hashMerkleRoot = BlockMerkleRoot(*pblock, &mutated);

            uint256 blockhash = pblock->GetHash();
            LogPrint(BCLog::POS, "%s: signing blockhash %s\n", __func__, blockhash.ToString());

            // Append a signature to the block
            return SignBlockWithKey(*pblock, key);
        }
    }

    wallet->nLastCoinStakeSearchTime = nSearchTime;

    return false;
}
