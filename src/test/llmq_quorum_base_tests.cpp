// Copyright (c) 2026 The DeFCoN developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <deploymentstatus.h>
#include <consensus/validation.h>
#include <evo/specialtx.h>
#include <logging.h>
#include <evo/deterministicmns.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/options.h>
#include <llmq/params.h>
#include <llmq/utils.h>
#include <random.h>
#include <sync.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <vector>

// A quorum hash arrives from the network (a DKG message, a relayed qfcommit, a mined commitment),
// and the block it names is looked up and its parent passed on as `pQuorumBaseBlockIndex->pprev`.
// Genesis has no parent, so that pointer is null. It used to reach IsQuorumTypeEnabled()'s
// gsl::not_null parameter, whose precondition calls std::terminate(): [[noreturn]] noexcept, so no
// exception handler could contain it. A parentless base must be rejected instead.
//
// The first case is the regression test of upstream Dash ca56af88, adapted to the free function
// llmq::IsQuorumTypeEnabled(). Upstream keeps it in evo_utils_tests, which this fork's CI does not
// run yet, so it lives here.

BOOST_AUTO_TEST_SUITE(llmq_quorum_base_tests)

BOOST_FIXTURE_TEST_CASE(genesis_quorum_base_null_pprev_is_safe, RegTestingSetup)
{
    const CBlockIndex* genesis = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip());
    BOOST_REQUIRE(genesis != nullptr);
    BOOST_REQUIRE_EQUAL(genesis->nHeight, 0);
    BOOST_REQUIRE(genesis->pprev == nullptr);

    const auto llmq_type = Params().GetConsensus().llmqTypeChainLocks;

    // The sink itself, as reached via `pQuorumBaseBlockIndex->pprev`. Passed through a variable
    // rather than a literal so this still compiles against the pre-fix gsl::not_null signature,
    // where it terminated at runtime.
    const CBlockIndex* const null_index = genesis->pprev;
    BOOST_CHECK(!llmq::IsQuorumTypeEnabled(llmq_type, null_index));

    // The GetAllQuorumMembers() path that CFinalCommitment::Verify() takes.
    BOOST_CHECK(llmq::utils::GetAllQuorumMembers(llmq_type, *Assert(m_node.dmnman), *Assert(m_node.llmq_ctx)->qsnapman,
                                                 genesis).empty());
}

// The block path: CheckLLMQCommitment() accepts genesis as a quorum base (it is an ancestor of
// every block) and hands a non-null commitment to CFinalCommitment::Verify(), which looked up the
// members through the base block's missing parent. The commitment below is well-formed in every
// field Verify() checks before that lookup, so only the parentless base can reject it.
BOOST_FIXTURE_TEST_CASE(genesis_quorum_base_commitment_is_rejected, RegTestingSetup)
{
    const CBlockIndex* genesis = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip());
    BOOST_REQUIRE(genesis != nullptr);
    BOOST_REQUIRE(genesis->pprev == nullptr);

    const auto llmq_params = *Assert(Params().GetLLMQ(Params().GetConsensus().llmqTypeChainLocks));

    llmq::CFinalCommitment qc(llmq_params, genesis->GetBlockHash());
    qc.nVersion = llmq::CFinalCommitment::GetVersion(llmq::IsQuorumRotationEnabled(llmq_params, genesis),
                                                     DeploymentActiveAfter(genesis, Params().GetConsensus(),
                                                                           Consensus::DEPLOYMENT_V19));
    qc.quorumIndex = 0;
    std::fill(qc.signers.begin(), qc.signers.end(), true);
    std::fill(qc.validMembers.begin(), qc.validMembers.end(), true);
    CBLSSecretKey sk;
    sk.MakeNewKey();
    qc.quorumPublicKey = sk.GetPublicKey();
    qc.quorumVvecHash = GetRandHash();
    qc.membersSig = sk.Sign(GetRandHash(), bls::bls_legacy_scheme.load());
    qc.quorumSig = sk.Sign(GetRandHash(), bls::bls_legacy_scheme.load());
    BOOST_REQUIRE(!qc.IsNull());

    BOOST_CHECK(!qc.Verify(*Assert(m_node.dmnman), *Assert(m_node.llmq_ctx)->qsnapman, genesis, /*checkSigs=*/false));
}

// Debug output must not assume an untrusted member bitset has the quorum size.
// Use an invalid payload version so the expected rejection is independent of
// quorum lookup and signatures, and check it with logging both off and on.
BOOST_FIXTURE_TEST_CASE(short_commitment_debug_logging_is_safe, RegTestingSetup)
{
    std::mutex log_mutex;
    std::vector<std::string> log_lines;
    struct RestoreLLMQLogging {
        bool enabled{LogInstance().WillLogCategory(BCLog::LLMQ)};
        std::list<std::function<void(const std::string&)>>::iterator callback;
        explicit RestoreLLMQLogging(std::mutex& mutex, std::vector<std::string>& lines)
        {
            callback = LogInstance().PushBackCallback([&mutex, &lines](const std::string& message) {
                if (message.find("CFinalCommitment -- CheckLLMQCommitment") == std::string::npos) return;
                std::lock_guard<std::mutex> lock(mutex);
                lines.push_back(message);
            });
        }
        ~RestoreLLMQLogging()
        {
            LogInstance().DeleteCallback(callback);
            if (enabled) LogInstance().EnableCategory(BCLog::LLMQ);
            else LogInstance().DisableCategory(BCLog::LLMQ);
        }
    } restore_logging{log_mutex, log_lines};

    const CBlockIndex* tip = WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip());
    BOOST_REQUIRE(tip != nullptr);
    const auto params = *Assert(Params().GetLLMQ(Params().GetConsensus().llmqTypeChainLocks));
    for (bool debug : {false, true}) {
        if (debug) LogInstance().EnableCategory(BCLog::LLMQ);
        else LogInstance().DisableCategory(BCLog::LLMQ);
        BOOST_REQUIRE_EQUAL(LogAcceptDebug(BCLog::LLMQ), debug);
        for (size_t size : {size_t{0}, size_t{1}, size_t(params.size), size_t(params.size + 1)}) {
            llmq::CFinalCommitmentTxPayload payload;
            payload.nVersion = 0;
            payload.nHeight = tip->nHeight + 1;
            payload.commitment = llmq::CFinalCommitment(params, tip->GetBlockHash());
            payload.commitment.validMembers.resize(size);
            CMutableTransaction tx;
            tx.nVersion = 3;
            tx.nType = TRANSACTION_QUORUM_COMMITMENT;
            SetTxPayload(tx, payload);
            const auto decoded = GetTxPayload<llmq::CFinalCommitmentTxPayload>(tx);
            BOOST_REQUIRE(decoded.has_value());
            BOOST_REQUIRE_EQUAL(decoded->commitment.validMembers.size(), size);
            {
                std::lock_guard<std::mutex> lock(log_mutex);
                log_lines.clear();
            }
            TxValidationState state;
            BOOST_CHECK(!llmq::CheckLLMQCommitment(*Assert(m_node.dmnman),
                *Assert(m_node.llmq_ctx)->qsnapman, *m_node.chainman, CTransaction(tx), tip, state));
            BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-qc-version");
            std::lock_guard<std::mutex> lock(log_mutex);
            if (debug) {
                BOOST_REQUIRE_EQUAL(log_lines.size(), 1U);
                std::string expected = "validMembers[";
                for (size_t i = 0; i < std::min(size, size_t(params.size)); ++i) {
                    expected += "v[" + std::to_string(i) + "]=0";
                }
                expected += "] signers[]";
                BOOST_CHECK(log_lines.front().find(expected) != std::string::npos);
            } else {
                BOOST_CHECK(log_lines.empty());
            }
        }
    }
}
BOOST_AUTO_TEST_SUITE_END()
