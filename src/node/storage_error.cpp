// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/storage_error.h>

#include <dbwrapper.h>
#include <shutdown.h>
#include <tinyformat.h>
#include <util/translation.h>

#include <utility>

bool HandleStorageError(std::exception_ptr exception, const std::string& where)
{
    if (!exception) return false;
    try {
        std::rethrow_exception(exception);
    } catch (const dbwrapper_error& e) {
        AbortNode(strprintf("Database error in %s: %s. Restart with -reindex to rebuild the databases; damaged "
                            "masternode list diffs may also be recalculated with -evodbrepair (add "
                            "-forceevodbrepair if they were repaired before).",
                            where, e.what()),
                  _("Error reading from database, shutting down."));
        return true;
    } catch (...) {
        return false;
    }
}

bool HandleStorageErrorOnThread(const char* thread_name, std::exception_ptr exception)
{
    return HandleStorageError(std::move(exception), strprintf("the %s thread", thread_name));
}

void RunHandlingStorageErrors(const std::function<void()>& work, const std::string& where)
{
    for (;;) {
        try {
            work();
            return;
        } catch (const dbwrapper_error&) {
            // Each round of `work` that stops here has consumed the task it failed on (a scheduler pops a
            // task before running it, a drain pops a callback), so calling it again makes progress.
            if (!HandleStorageError(std::current_exception(), where)) throw;
        }
    }
}
