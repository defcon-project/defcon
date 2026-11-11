// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <warnings.h>

#include <sync.h>
#include <util/string.h>
#include <util/system.h>
#include <util/translation.h>
#include <hash.h>

#include <vector>

static Mutex g_warnings_mutex;
static bilingual_str g_misc_warnings GUARDED_BY(g_warnings_mutex);
static bool fLargeWorkInvalidChainFound GUARDED_BY(g_warnings_mutex) = false;
static bool g_deep_fork GUARDED_BY(g_warnings_mutex) = false;

void SetMiscWarning(const bilingual_str& warning)
{
    LOCK(g_warnings_mutex);
    g_misc_warnings = warning;
}

void SetfLargeWorkInvalidChainFound(bool flag)
{
    LOCK(g_warnings_mutex);
    fLargeWorkInvalidChainFound = flag;
}

bool SetDeepForkWarning(bool flag)
{
    LOCK(g_warnings_mutex);
    const bool changed{g_deep_fork != flag};
    g_deep_fork = flag;
    return changed;
}

bilingual_str GetWarnings(bool verbose)
{
    bilingual_str warnings_concise;
    std::vector<bilingual_str> warnings_verbose;

    LOCK(g_warnings_mutex);

    // Pre-release build warning
    if (!CLIENT_VERSION_IS_RELEASE) {
        warnings_concise = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");
        warnings_verbose.emplace_back(warnings_concise);
    }

    // Misc warnings like out of disk space and clock is wrong
    if (!g_misc_warnings.empty()) {
        warnings_concise = g_misc_warnings;
        warnings_verbose.emplace_back(warnings_concise);
    }

    if (g_deep_fork) {
        warnings_concise = _("Warning: Outbound peers offer a chain that branches off this node's chain too far below its tip to follow automatically. If this node was upgraded late or cut off from the network, add a trusted, up-to-date peer with addnode.");
        warnings_verbose.emplace_back(warnings_concise);
    }

    if (fLargeWorkInvalidChainFound) {
        warnings_concise = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
        warnings_verbose.emplace_back(warnings_concise);
    }

    if (verbose) {
        return Join(warnings_verbose, Untranslated("<hr />"));
    }

    return warnings_concise;
}
