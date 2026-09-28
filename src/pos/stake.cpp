// Copyright (c) 2025 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pos/stake.h>

// Only the coin-selection and split arithmetic below use this; keeping it out
// of the header stops it colliding with the test framework's CENT.
static constexpr CAmount CENT{1000000};

#include <chainparams.h>
#include <consensus/merkle.h>
#include <interfaces/chain.h>
#include <node/miner.h>
#include <pos/minter.h>
#include <pow.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <util/moneystr.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/walletutil.h>

#include <algorithm>
#include <cmath>

extern std::atomic<bool> fStopMinerProc;

void StakeSkipReport::Add(StakeEligibility why, CAmount value)
{
    switch (why) {
    case StakeEligibility::Eligible:   break;
    case StakeEligibility::Immature:   immature   += value; break;
    case StakeEligibility::BLSAddress: bls        += value; break;
    case StakeEligibility::BelowMin:   below_min  += value; break;
    case StakeEligibility::AboveMax:   above_max  += value; break;
    case StakeEligibility::Collateral: collateral += value; break;
    case StakeEligibility::TooYoung:   too_young  += value; break;
    case StakeEligibility::TooOld:     too_old    += value; break;
    } // no default case, so the compiler can warn about missing cases
}

CAmount StakeSkipReport::Total() const
{
    return immature + bls + below_min + above_max + collateral + too_young + too_old;
}

CAmount PermanentlyExcluded(const StakeSkipReport& report)
{
    return report.bls + report.below_min + report.above_max + report.collateral + report.too_old;
}

bool ShouldWarnAboutExcludedValue(const StakeSkipReport& report, CAmount min_stake_value)
{
    // A floor of zero is regtest's "everything stakes", where nothing can be
    // permanently excluded by amount and the question does not arise.
    if (min_stake_value <= 0) return false;
    return PermanentlyExcluded(report) >= min_stake_value;
}

int64_t StakeInputAge(int64_t candidate_time, int64_t coin_block_time, int64_t wallet_time)
{
    return candidate_time - (coin_block_time > 0 ? coin_block_time : wallet_time);
}

int64_t CStakeWallet::CoinBlockTime(const CWalletTx& coin) const
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    if (!wallet) return 0;
    // A CONFLICTED entry reuses these fields for the block of the conflicting
    // transaction, not the one holding this coin, so only CONFIRMED names the
    // block whose time consensus would measure against.
    if (coin.m_confirm.status != CWalletTx::CONFIRMED) return 0;
    if (coin.m_confirm.hashBlock.IsNull()) return 0;

    int64_t block_time = 0;
    if (!wallet->chain().findBlock(coin.m_confirm.hashBlock, interfaces::FoundBlock().time(block_time))) {
        return 0;
    }
    return block_time;
}

std::string DescribePermanentExclusions(const StakeSkipReport& report)
{
    // Same names getstakinginfo already uses, so the log line and the RPC
    // cannot drift into describing the same coins differently.
    const std::pair<const char*, CAmount> reasons[] = {
        {"too_small", report.below_min},
        {"too_large", report.above_max},
        {"collateral_amount", report.collateral},
        {"bls", report.bls},
        {"too_old", report.too_old},
    };

    std::string out;
    for (const auto& [name, amount] : reasons) {
        if (amount <= 0) continue;
        if (!out.empty()) out += ", ";
        out += strprintf("%s: %s", name, FormatMoney(amount));
    }
    return out;
}

StakeEligibility CStakeWallet::ClassifyForStaking(CAmount value, int depth,
                                                  TxoutType type, int64_t inputAge, int nHeight) const
{
    // CheckProofOfStake measures depth as pindexPrev->nHeight - coin.nHeight,
    // which does not count the coin's own block, while GetDepthInMainChain()
    // does: the wallet's number is one larger for the same coin. It also
    // applies the rule to every staking input, not only to generated ones, so
    // restricting it here let the wallet offer coins the kernel would refuse.
    if (depth - 1 < COINBASE_MATURITY + 1) return StakeEligibility::Immature;
    if (type == TxoutType::BLSPUBKEY) return StakeEligibility::BLSAddress;
    if (value < params.stakeValueRange[0]) return StakeEligibility::BelowMin;
    if (value > params.stakeValueRange[1]) return StakeEligibility::AboveMax;
    if (value == params.regularMnCollateral || value == params.evoMnCollateral) {
        return StakeEligibility::Collateral;
    }
    if (inputAge < params.stakeAgeRange[0]) return StakeEligibility::TooYoung;
    // Both halves of this rule now match validation. The v2 age-cap GATE is
    // resolved from the height being mined, as validation resolves it, and the
    // age VALUE is the same quantity CheckProofOfStake computes: callers build
    // it with StakeInputAge, from the candidate block's time and the time of
    // the block holding the coin. It used to be the wallet's own estimate --
    // wall clock minus GetTxTime() -- which disagreed near stakeAgeRange[0] and
    // had the wallet offering coins the kernel would refuse.
    //
    // The kernel remains the authority; the point is that the wallet no longer
    // asks it a different question.
    if (!IsPosKernelV2(params, nHeight) && inputAge > params.stakeAgeRange[1]) {
        return StakeEligibility::TooOld;
    }
    return StakeEligibility::Eligible;
}

StakeSkipReport CStakeWallet::ExplainExcludedCoins(int64_t nTime, int nHeight) const
{
    return ExplainExcludedCoins(nTime, nHeight, nullptr);
}

StakeWalletInfo CStakeWallet::GetStakingInfo(int64_t nTime, int nHeight) const
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    if (!wallet) return {};

    // Collect the union of the selection and report ranges. In particular,
    // regtest permits zero-value staking inputs, while the report starts at 1.
    // COutput holds raw mapWallet pointers. Keep them alive for both consumers:
    // removeprunedfunds, for example, can erase entries under this same lock.
    LOCK(wallet->cs_wallet);
    std::vector<COutput> coins;
    wallet->AvailableCoins(coins, nullptr, std::min(CAmount{1}, params.stakeValueRange[0]),
                           std::max(MAX_MONEY, params.stakeValueRange[1]));
    StakeWalletInfo info;
    info.spendable_outputs = std::count_if(coins.begin(), coins.end(), [](const COutput& out) {
        const CAmount value = out.tx->tx->vout[out.i].nValue;
        return out.fSpendable && value >= 1 && value <= MAX_MONEY;
    });
    info.weight = GetStakeWeight(nTime, nHeight, &coins);
    info.excluded = ExplainExcludedCoins(nTime, nHeight, &coins);
    return info;
}

StakeSkipReport CStakeWallet::ExplainExcludedCoins(int64_t nTime, int nHeight, const std::vector<COutput>* coins) const
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    StakeSkipReport report;
    if (!wallet) {
        return report;
    }

    // Deliberately unfiltered, unlike the selection loop: a coin kept out by its
    // value has to reach the classifier to be counted, and AvailableCoins would
    // otherwise drop it before anything could name the reason.
    std::vector<COutput> vCoins;
    if (!coins) {
        LOCK(wallet->cs_wallet);
        wallet->AvailableCoins(vCoins);
        coins = &vCoins;
    }

    for (const auto& output : *coins) {
        const CWalletTx* pcoin = output.tx;
        const int i = output.i;
        const CAmount value = pcoin->tx->vout[i].nValue;
        // The shared list can be wider than AvailableCoins' default range.
        if (value < 1 || value > MAX_MONEY) continue;

        int nDepth;
        {
            LOCK(wallet->cs_wallet);
            nDepth = pcoin->GetDepthInMainChain();
        }

        std::vector<valtype> vSolutions;
        const TxoutType type = Solver(pcoin->tx->vout[i].scriptPubKey, vSolutions);
        const int64_t inputAge = StakeInputAge(nTime, CoinBlockTime(*pcoin), pcoin->GetTxTime());

        report.Add(ClassifyForStaking(value, nDepth, type, inputAge, nHeight), value);
    }

    // The loop above cannot see every immature coin any more.
    //
    // Holding back immature coinstake outputs made AvailableCoins drop them
    // before this classifier runs, which is right for spending and wrong here:
    // this function exists to name what the rules removed, and it was reporting
    // 10,000 against an immature balance of 2.68 million on the devnet seed.
    // The value has to come from the balance instead.
    //
    // The two sets do not overlap, and the reason is a one-block gap in the
    // thresholds: AvailableCoins releases a generated coin at depth
    // COINBASE_MATURITY + 1, while ClassifyForStaking still calls it Immature
    // until depth COINBASE_MATURITY + 2. The loop counts exactly that sliver;
    // the balance counts everything below it. stake_immature_accounting_has_no_gap
    // pins the relationship, because adding these two numbers is only correct
    // while it holds.
    {
        LOCK(wallet->cs_wallet);
        report.immature += wallet->GetBalance().m_mine_immature;
    }

    return report;
}

CAmount CStakeWallet::StakeTargetSize(double network_weight, CAmount configured) const
{
    // Twice the floor, so that the pieces of a split -- each at least a target
    // -- and a credit topped up to one can always stake again; half the
    // ceiling, so that a credit just under two targets still fits under it.
    const CAmount floor = std::max<CAmount>(2 * params.stakeValueRange[0], MIN_STAKE_TARGET);
    const CAmount ceiling = std::max<CAmount>(floor, params.stakeValueRange[1] / 2);

    if (configured > 0) {
        return std::clamp(configured, floor, ceiling);
    }

    // A coin that wins cannot stake again until the kernel's depth rule and
    // the minimum age both pass again; whichever is longer is the rest.
    int64_t rest_blocks = COINBASE_MATURITY + 2;
    if (params.posTargetSpacing > 0) {
        rest_blocks = std::max<int64_t>(rest_blocks,
            (params.stakeAgeRange[0] + params.posTargetSpacing - 1) / params.posTargetSpacing);
    }

    // netstakeweight is an estimate from recent difficulty and can be zero on
    // a young chain; anything that is not a positive, finite number keeps the
    // floor rather than inventing a size.
    if (!(network_weight > 0) || !std::isfinite(network_weight)) {
        return floor;
    }
    const double wanted = network_weight * STAKE_REST_BUDGET_BPS / 10000.0 / rest_blocks;
    if (wanted >= static_cast<double>(ceiling)) {
        return ceiling;
    }
    // Whole coins: the value shows in getstakinginfo and in every coinstake,
    // and a size that moved by fractions of a satoshi with each new estimate
    // would read as noise.
    const CAmount whole = (static_cast<CAmount>(wanted) / COIN) * COIN;
    return std::clamp(whole, floor, ceiling);
}

std::vector<CAmount> CStakeWallet::SplitStakeCredit(CAmount nCredit, CAmount target) const
{
    // Splitting must not manufacture an output that can never stake again.
    // Under stakeValueRange[0] a coin is skipped for good, and so is one sitting
    // exactly on a collateral amount or over the ceiling.
    //
    // The rule this replaces halved the credit at 15,000 whenever both halves
    // could stake, which walked every output down to between 10,000 and 20,000
    // and left wallets holding thousands of them. Pieces of about a target
    // keep the count proportional to the balance over the target instead.
    const auto stakeable = [this](CAmount value) {
        return value >= params.stakeValueRange[0] &&
               value <= params.stakeValueRange[1] &&
               value != params.regularMnCollateral &&
               value != params.evoMnCollateral;
    };

    if ((target <= 0 || nCredit < 2 * target) && stakeable(nCredit)) {
        return {nCredit};
    }

    // As many whole targets as the credit holds, capped so that one win cannot
    // write an arbitrarily large coinstake; but never so few that a piece is
    // left above the ceiling.
    // Even an unsplit credit must avoid collateral amounts. A safe split
    // takes precedence over the target in that case.
    int64_t pieces = target > 0 ? std::clamp<int64_t>(nCredit / target, 2, MAX_STAKE_SPLIT_OUTPUTS) : 2;
    if (params.stakeValueRange[1] > 0) {
        pieces = std::max<int64_t>(pieces, (nCredit + params.stakeValueRange[1] - 1) / params.stakeValueRange[1]);
    }

    // Equal pieces, whole cents, the remainder on the first. If a piece lands
    // exactly on a collateral amount, one more piece moves every piece off it.
    for (int64_t n = pieces; n <= pieces + 2; ++n) {
        const CAmount each = (nCredit / n / CENT) * CENT;
        const CAmount first = nCredit - each * (n - 1);
        if (stakeable(each) && stakeable(first)) {
            std::vector<CAmount> outputs(n, each);
            outputs[0] = first;
            return outputs;
        }
    }
    return {}; // No safe layout; never silently return an ineligible output.
}

std::vector<size_t> CStakeWallet::ChooseCombineInputs(CAmount kernel_value, const std::vector<CAmount>& candidates,
                                                      CAmount target, size_t max_extra, CAmount allowance) const
{
    std::vector<size_t> order;
    order.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const CAmount value = candidates[i];
        // A collateral amount is left alone even when it is small enough to
        // qualify: a larger network's target can exceed one, and the output may
        // be a masternode's even when nothing has locked it.
        if (value <= 0 || value >= target) continue;
        if (value == params.regularMnCollateral || value == params.evoMnCollateral) continue;
        order.push_back(i);
    }
    // Smallest first: the count falls by one per input whatever its size, and
    // the smallest cost the least resting value. Stable, so equal amounts keep
    // the caller's order and the choice is deterministic.
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return candidates[a] < candidates[b]; });

    std::vector<size_t> chosen;
    CAmount credit = kernel_value;
    CAmount added = 0;
    for (const size_t i : order) {
        if (chosen.size() >= max_extra) break;
        const CAmount value = candidates[i];
        // Ascending, so once one does not fit none after it will.
        if (added + value > allowance) break;
        // Value under the floor never stakes, so taking it costs no staking
        // time at all. Value that stakes would rest with the kernel, and only
        // up to the target is that worth paying.
        const bool stakes = value >= params.stakeValueRange[0];
        if (stakes && credit + value > target) break;
        chosen.push_back(i);
        credit += value;
        added += value;
    }
    return chosen;
}

size_t CStakeWallet::CountSpendableOutputs() const
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    if (!wallet) return 0;
    std::vector<COutput> coins;
    LOCK(wallet->cs_wallet);
    wallet->AvailableCoins(coins);
    return std::count_if(coins.begin(), coins.end(), [](const COutput& out) { return out.fSpendable; });
}

uint64_t CStakeWallet::GetStakeWeight(int64_t nTime, int nHeight) const
{
    return GetStakeWeight(nTime, nHeight, nullptr);
}

uint64_t CStakeWallet::GetStakeWeight(int64_t nTime, int nHeight, const std::vector<COutput>* coins) const
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    if (!wallet) return 0;
    // Choose coins to use
    CAmount nBalance = wallet->GetBalance().m_mine_trusted;
    if (nBalance <= wallet->nReserveBalance) {
        return 0;
    }

    CAmount nValueIn = 0;
    std::vector<const CWalletTx*> vwtxPrev;
    std::set<std::pair<const CWalletTx*,unsigned int> > setCoins;

    CAmount nTargetValue = nBalance - wallet->nReserveBalance;
    if (!SelectCoinsForStaking(nTargetValue, nTime, nHeight, setCoins, nValueIn, coins)) {
        return 0;
    }

    if (setCoins.empty()) {
        return 0;
    }

    // Every coin here already passed ClassifyForStaking, which applies the
    // kernel's depth rule. The second, looser test this replaced admitted coins
    // two blocks before the kernel would, so the weight reported to callers
    // disagreed with the loop that had just produced it.
    uint64_t nWeight = 0;
    for(std::pair<const CWalletTx* ,unsigned int> pcoin : setCoins) {
        nWeight += pcoin.first->tx->vout[pcoin.second].nValue;
    }

    return nWeight;
}

bool CStakeWallet::SelectCoinsForStaking(CAmount nTargetValue, int64_t nTime, int nHeight, std::set<std::pair<const CWalletTx*, unsigned int>>& setCoinsRet, CAmount& nValueRet) const
{
    return SelectCoinsForStaking(nTargetValue, nTime, nHeight, setCoinsRet, nValueRet, nullptr);
}

bool CStakeWallet::SelectCoinsForStaking(CAmount nTargetValue, int64_t nTime, int nHeight,
                                      std::set<std::pair<const CWalletTx*, unsigned int>>& setCoinsRet,
                                      CAmount& nValueRet, const std::vector<COutput>* coins) const
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    if (!wallet) return false;
    std::vector<COutput> vCoins;

    if (!coins) {
        LOCK(wallet->cs_wallet);
        wallet->AvailableCoins(vCoins, nullptr, params.stakeValueRange[0], params.stakeValueRange[1]);
        coins = &vCoins;
    }

    setCoinsRet.clear();
    nValueRet = 0;

    for (const auto& output : *coins)
    {
        const CWalletTx* pcoin = output.tx;
        int i = output.i;
        const CAmount inputValue = pcoin->tx->vout[i].nValue;
        // Keep the selection range when consuming the wider shared list.
        if (inputValue < params.stakeValueRange[0] || inputValue > params.stakeValueRange[1]) continue;

        // AvailableCoins lists watch-only outputs as well, marked not spendable,
        // and nothing below ever asked. Such a coin passes every rule that
        // follows -- value, depth, script type, age -- and can be chosen as a
        // kernel this wallet cannot sign: the attempt then fails after the
        // search instead of never starting, and if that kernel is the one that
        // wins, the block is lost.
        if (!output.fSpendable) {
            continue;
        }

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

        std::vector<valtype> vSolutions;
        const TxoutType whichType = Solver(pcoin->tx->vout[i].scriptPubKey, vSolutions);
        // nTime is the block being mined, which is what CheckProofOfStake
        // measures against. It was already being passed in here and ignored,
        // while the age came from the wall clock instead.
        const int64_t inputAge = StakeInputAge(nTime, CoinBlockTime(*pcoin), pcoin->GetTxTime());

        if (ClassifyForStaking(inputValue, nDepth, whichType, inputAge, nHeight)
                != StakeEligibility::Eligible) {
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
 * A signing provider for `script` that can produce private keys, whatever kind
 * of wallet this is. Returns nullptr when no manager owns the script.
 *
 * Staking needs real private keys twice: once for the block header signature
 * and once to sign the coinstake inputs.
 *
 * Note that GetSolvingProvider is *not* usable here for either manager.
 * LegacySigningProvider::GetKey returns false unconditionally, and a descriptor
 * wallet's solving provider is built with include_private = false: both are
 * deliberately key-free, because solving only needs public material. The legacy
 * manager is itself a FillableSigningProvider, so it is used directly; the
 * descriptor manager is asked for a provider that includes private keys.
 *
 * `owned` holds the descriptor manager's provider for as long as the caller
 * needs it. The legacy path returns a pointer to the manager, which the wallet
 * owns and outlives this call.
 */
const SigningProvider* GetStakingSigningProvider(const CWallet& wallet, const CScript& script,
                                                 std::unique_ptr<SigningProvider>& owned)
{
    for (ScriptPubKeyMan* spk_man : wallet.GetAllScriptPubKeyMans()) {
        if (const auto* desc_man = dynamic_cast<const DescriptorScriptPubKeyMan*>(spk_man)) {
            if (auto provider = desc_man->GetSigningProvider(script, /*include_private=*/true)) {
                owned = std::move(provider);
                return owned.get();
            }
            continue;
        }
        if (const auto* legacy = dynamic_cast<const LegacyScriptPubKeyMan*>(spk_man)) {
            return legacy;
        }
    }
    return nullptr;
}

/**
 * Whether `script` pays `pubkey` in one of the two forms a staking key's
 * outputs take: pay-to-pubkey, which every coinstake writes, or the
 * pay-to-pubkey-hash address it was funded at.
 *
 * Combining stays within one key on purpose. Spending outputs of several
 * addresses in one transaction would tie those addresses together on the
 * chain; outputs of one key are already tied by the key itself.
 */
bool PaysToKey(const CScript& script, const CPubKey& pubkey)
{
    std::vector<valtype> solutions;
    switch (Solver(script, solutions)) {
    case TxoutType::PUBKEY:
        return CPubKey(solutions[0]) == pubkey;
    case TxoutType::PUBKEYHASH:
        return CKeyID(uint160(solutions[0])) == pubkey.GetID();
    default:
        return false;
    }
}
} // namespace

/**
 * Teach a descriptor wallet about the pay-to-pubkey form of its own keys.
 *
 * A coinstake must pay vout[1] to a pay-to-pubkey script: CheckBlockSignature
 * recovers the signing pubkey from that output, and only its PUBKEY branch can
 * succeed. A descriptor wallet tracks exactly the scripts its descriptors
 * produce -- pkh(...) -- so it does not recognise that output as its own. It
 * books its own coinstake as an outgoing send, and the staked amount together
 * with the reward leaves its visible balance.
 *
 * Registering the matching pk(...) descriptor closes that gap. No key material
 * is created or changed: this tells the wallet about a second script form for
 * keys it already holds.
 *
 * Only coinstakes from here on become visible. Outputs already mined were never
 * tracked, so recovering those still needs importdescriptors with a rescan.
 */
bool EnsureCoinstakeDescriptors(CWallet& wallet)
{
    // A legacy keystore already matches any script form for a key it holds.
    if (!wallet.IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) return true;

    // A watch-only wallet holds no key that could sign a coinstake, so there is
    // nothing here to register and nothing it could ever stake. Reporting
    // success let the switch go on for a wallet that cannot produce a block,
    // and getstakinginfo then said it was staking.
    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        LogPrint(BCLog::POS, "%s: wallet holds no private keys and cannot stake\n", __func__);
        return false;
    }

    // Locked, and encrypted: DescriptorScriptPubKeyMan::GetKeys() returns an
    // empty map in that state, so every private descriptor below fails to
    // render and the loop finds nothing to mirror. Reporting success then is
    // the worst of both -- staking is enabled with no pk() twin registered,
    // which is precisely the state this function exists to prevent, and
    // unlocking afterwards does not repair it because ToggleWalletStaking is
    // the only caller and runs on the off->on edge alone. Refuse, and say what
    // to do about it.
    if (wallet.IsLocked()) {
        LogPrint(BCLog::POS, "%s: wallet is locked; unlock it before enabling staking\n", __func__);
        return false;
    }

    struct Wanted {
        std::string expression;
        int32_t range_start;
        int32_t range_end;
    };
    std::vector<Wanted> wanted;

    {
        LOCK(wallet.cs_wallet);
        for (ScriptPubKeyMan* spk_man : wallet.GetAllScriptPubKeyMans()) {
            auto* desc_man = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man);
            if (desc_man == nullptr) continue;

            // Past the locked check above, a private descriptor that will not
            // render belongs to keys this wallet does not hold. Such a
            // descriptor cannot stake -- its coins are not spendable here, so
            // selection never offers them -- and skipping it is right. It must
            // not condemn the wallet's own descriptors alongside it.
            std::string desc;
            if (!desc_man->GetDescriptorString(desc, /*priv=*/true)) {
                LogPrint(BCLog::POS, "%s: skipping a descriptor whose keys this wallet does not hold\n", __func__);
                continue;
            }
            // Only pkh needs a counterpart; a pk descriptor is what we add.
            if (desc.rfind("pkh(", 0) != 0) continue;

            const size_t close = desc.rfind(')');
            if (close == std::string::npos || close <= 4) continue;

            // The same indices the source descriptor covers, so every key that
            // can stake has its coinstake form registered too.
            const std::pair<int32_t, int32_t> range = desc_man->GetRange();
            wanted.push_back({"pk(" + desc.substr(4, close - 4) + ")", range.first, range.second});
        }
    }

    bool ok = true;
    bool changed = false;
    for (const Wanted& item : wanted) {
        FlatSigningProvider provider;
        std::string error;
        std::unique_ptr<Descriptor> parsed = Parse(item.expression, provider, error, /*require_checksum=*/false);
        if (!parsed) {
            LogPrint(BCLog::POS, "%s: could not build coinstake descriptor: %s\n", __func__, error);
            ok = false;
            continue;
        }

        const bool ranged = parsed->IsRange();
        WalletDescriptor wdesc(std::move(parsed), /*creation_time=*/0,
                               ranged ? item.range_start : 0,
                               ranged ? item.range_end : 0,
                               /*next_index=*/0);

        LOCK(wallet.cs_wallet);
        if (auto* existing = wallet.GetDescriptorScriptPubKeyMan(wdesc)) {
            // The source pkh() range grows as the wallet hands out addresses,
            // and skipping here froze the pk() twin at whatever range it was
            // first registered with -- keys past that end staked to a script
            // the wallet did not recognise. Extend it alongside the source.
            if (!ranged || existing->GetRange().second >= item.range_end) continue;
            std::string update_error;
            if (!existing->CanUpdateToWalletDescriptor(wdesc, update_error)) {
                LogPrint(BCLog::POS, "%s: could not extend coinstake descriptor: %s\n", __func__, update_error);
                ok = false;
                continue;
            }
        }

        if (wallet.AddWalletDescriptor(wdesc, provider, /*label=*/"", /*internal=*/false) == nullptr) {
            LogPrint(BCLog::POS, "%s: could not register coinstake descriptor\n", __func__);
            ok = false;
            continue;
        }
        changed = true;
    }

    if (changed) {
        wallet.WalletLogPrintf("Registered pay-to-pubkey descriptors so coinstake outputs are recognised\n");
    }
    return ok;
}

StakeAttempt CStakeWallet::CreateCoinStake(CChainState& chain_state, CBlockIndex* pindexPrev, unsigned int nBits, int64_t nTime, int nBlockHeight, int64_t nFees, CMutableTransaction& txNew, CKey& key, size_t max_coinstake_bytes)
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    if (!wallet) return StakeAttempt::Error;
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    CAmount nBalance = wallet->GetAvailableBalance();
    if (nBalance <= wallet->nReserveBalance) {
        return StakeAttempt::NoEligibleCoins;
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
        return StakeAttempt::NoEligibleCoins;
    }

    if (setCoins.empty()) {
        return StakeAttempt::NoEligibleCoins;
    }

    CAmount nCredit = 0;
    std::set<std::pair<const CWalletTx*, unsigned int>>::iterator it = setCoins.begin();

    for (; it != setCoins.end(); ++it)
    {
        auto pcoin = *it;
        if (fStopMinerProc)
            return StakeAttempt::Stopped;

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

            std::unique_ptr<SigningProvider> kernel_provider_owned;
            const SigningProvider* kernel_provider =
                GetStakingSigningProvider(*wallet, scriptPubKeyKernel, kernel_provider_owned);
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
        // Coins were eligible and the loop above found no winning kernel at this
        // timestamp. That is the ordinary outcome of an attempt, not a fault, and
        // the caller must not treat it as one.
        return StakeAttempt::NoKernelFound;
    }

    // The size this win lays its credit out in: -staketarget if set, otherwise
    // derived from the network's weight. Read with no wallet lock held;
    // GetPoSKernelPS takes cs_main, which the tree only ever takes under
    // cs_wallet, never around it.
    const CAmount target = StakeTargetSize(GetPoSKernelPS(pindexPrev, Params().GetConsensus()), wallet->m_stake_target);

    // Combine: spend some of the kernel key's small outputs in the same
    // coinstake. A coinstake pays no fee and consensus judges only its first
    // input as the kernel (CheckProofOfStake reads vin[0]); every further input
    // is an ordinary spend, checked for maturity and signature like any other,
    // and its value counts in stakeValueIn, so the reward ceiling is unchanged.
    size_t combined = 0;
    size_t combine_candidates = 0;
    const char* combine_reason = wallet->m_stake_combine ? "no_block_space" : "disabled";
    CAmount combined_value = 0;
    if (wallet->m_stake_combine) {
        // The kernel's own input and the largest split this can write are paid
        // for first; each combined input comes out of what is left.
        const size_t fixed = COINSTAKE_FIXED_BYTES + COINSTAKE_INPUT_BYTES +
                             (MAX_STAKE_SPLIT_OUTPUTS + 3) * COINSTAKE_OUTPUT_BYTES;
        const size_t max_extra = max_coinstake_bytes > fixed
            ? std::min<size_t>(MAX_STAKE_COMBINE_INPUTS - 1, (max_coinstake_bytes - fixed) / COINSTAKE_INPUT_BYTES)
            : 0;
        if (max_extra > 0) {
            const CPubKey kernel_pubkey = key.GetPubKey();
            const COutPoint kernel_outpoint = txNew.vin[0].prevout;
            std::vector<COutput> coins;
            std::vector<CAmount> values;
            std::vector<std::pair<const CWalletTx*, unsigned int>> refs;
            LOCK(wallet->cs_wallet);
            // AvailableCoins already leaves out what is spent, what is locked --
            // lockunspent, and the masternode collaterals the wallet locks at
            // startup -- and generated outputs that are not yet mature.
            wallet->AvailableCoins(coins);
            for (const COutput& out : coins) {
                if (!out.fSpendable) continue;
                if (COutPoint(out.tx->GetHash(), out.i) == kernel_outpoint) continue;
                // As deep as a kernel has to be: past consensus maturity for a
                // generated output, and clear of a shallow reorg for any.
                if (out.tx->GetDepthInMainChain() - 1 < COINBASE_MATURITY + 1) continue;
                const CTxOut& txout = out.tx->tx->vout[out.i];
                if (!PaysToKey(txout.scriptPubKey, kernel_pubkey)) continue;
                values.push_back(txout.nValue);
                refs.emplace_back(out.tx, out.i);
            }
            combine_candidates = values.size();
            combine_reason = values.empty() ? "no_mature_same_key_outputs" : "no_inputs_fit_limits";
            const CAmount allowance = nBalance - wallet->nReserveBalance - nCredit;
            for (const size_t i : ChooseCombineInputs(nCredit, values, target, max_extra, allowance)) {
                txNew.vin.push_back(CTxIn(refs[i].first->GetHash(), refs[i].second));
                vwtxPrev.push_back(refs[i].first);
                nCredit += values[i];
                combined_value += values[i];
                ++combined;
            }
            if (combined > 0) combine_reason = "inputs_selected";
        }
    }

    // Get block reward
    // The subsidy, and deliberately not the fees of the block being built.
    //
    // A coinstake may mint at most what IsBlockValueValid allows it, and past
    // nPosFeeBurnActivationHeight that is the subsidy alone: the fees of the
    // transactions in the block are destroyed. Adding them here would build a
    // block the network refuses. Before that height the ceiling was subsidy
    // plus fees and this line still minted the subsidy, which is why every
    // block on the chain burns its fees -- the rule now says so rather than
    // relying on this line to keep saying it.
    CAmount nReward = GetProofOfStakeReward();
    if (nReward < 0) {
        return StakeAttempt::Error;
    }

    nCredit += nReward;
    {
        // Every piece goes to the kernel's pay-to-pubkey script, which
        // CheckBlockSignature reads from vout[1].
        const std::vector<CAmount> outputs = SplitStakeCredit(nCredit, target);
        if (outputs.empty()) return StakeAttempt::Error;
        const CScript script_out = txNew.vout[1].scriptPubKey;
        txNew.vout.resize(1);
        for (const CAmount value : outputs) {
            txNew.vout.emplace_back(value, script_out);
        }
        LogPrint(BCLog::POS, "%s: wallet '%s': building coinstake: target %s, combined %u outputs (%s), %u pieces, %u mature same-key candidates, reason=%s (not yet accepted)\n", __func__,
                 wallet->GetName(), FormatMoney(target), combined, FormatMoney(combined_value), outputs.size(), combine_candidates, combine_reason);
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
        std::unique_ptr<SigningProvider> provider_owned;
        const SigningProvider* provider = GetStakingSigningProvider(*wallet, scriptPubKeyOut, provider_owned);
        if (!provider) {
            LogPrint(BCLog::POS, "%s: no signing provider for input %d.", __func__, nIn);
            return StakeAttempt::Error;
        }
        if (!ProduceSignature(*provider, MutableTransactionSignatureCreator(&txNew, nIn, amount, SIGHASH_ALL), scriptPubKeyOut, sigdata)) {
            LogPrint(BCLog::POS, "%s: ProduceSignature failed.", __func__);
            return StakeAttempt::Error;
        }

        UpdateInput(txNew.vin[nIn], sigdata);
        nIn++;
    }

    // Limit size
    unsigned int nBytes = ::GetSerializeSize(txNew, PROTOCOL_VERSION);
    if (nBytes >= MaxBlockSize() / 5 || nBytes > max_coinstake_bytes) {
        LogPrint(BCLog::POS, "%s: Exceeded coinstake size limit.", __func__);
        return StakeAttempt::Error;
    }

    // Successfully generated coinstake
    return StakeAttempt::BlockFound;
}

StakeAttempt CStakeWallet::SignBlock(CChainState& chain_state, CBlockTemplate* pblocktemplate, int nHeight, int64_t nSearchTime)
{
    const std::shared_ptr<CWallet> wallet = m_wallet.lock();
    if (!wallet) return StakeAttempt::Error;
    LogPrint(BCLog::POS, "%s, Height %d\n", __func__, nHeight);

    assert(pblocktemplate);
    CBlock* pblock = &pblocktemplate->block;
    assert(pblock);
    if (pblock->vtx.size() < 1) {
        LogPrint(BCLog::POS, "%s: Malformed block.", __func__);
        return StakeAttempt::Error;
    }

    CAmount nFees = -pblocktemplate->vTxFees[0];
    // Read the tip under the lock that guards it. The pointer stays valid afterwards
    // (block index entries are never freed) and CheckStake re-checks staleness, so the
    // lock is not held across coinstake creation, which takes wallet locks.
    CBlockIndex* pindexPrev = WITH_LOCK(::cs_main, return chain_state.m_chainman.ActiveChain().Tip());

    CKey key;
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, Params().GetConsensus());
    LogPrint(BCLog::POS, "%s, nBits %d\n", __func__, pblock->nBits);

    // The room the coinstake has. The template reserves only about a thousand
    // bytes beside the coinbase, which a single-input coinstake fits and a
    // combining one might not; a block over the limit is invalid, so the
    // coinstake is fitted to what is left rather than the other way round.
    // The margin covers the block signature and the longer transaction count.
    constexpr size_t BLOCK_SIGNATURE_MARGIN{200};
    const size_t block_bytes = ::GetSerializeSize(*pblock, PROTOCOL_VERSION) + BLOCK_SIGNATURE_MARGIN;
    const size_t max_coinstake_bytes = block_bytes < MaxBlockSize() ? MaxBlockSize() - block_bytes : 0;

    CMutableTransaction txCoinStake;
    const StakeAttempt made = CreateCoinStake(chain_state, pindexPrev, pblock->nBits, nSearchTime, nHeight, nFees, txCoinStake, key, max_coinstake_bytes);
    if (made != StakeAttempt::BlockFound) {
        LOCK(wallet->cs_wallet);
        wallet->nLastCoinStakeSearchTime = nSearchTime;
        return made;
    }
    {
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
            return SignBlockWithKey(*pblock, key) ? StakeAttempt::BlockFound : StakeAttempt::Error;
        }
    }

    {
        LOCK(wallet->cs_wallet);
        wallet->nLastCoinStakeSearchTime = nSearchTime;
    }

    // A kernel was found but this search time is not yet past the tip's median.
    // Transient, and the next timestamp may serve, so it must not be reported as
    // an eligibility problem.
    return StakeAttempt::NoKernelFound;
}
