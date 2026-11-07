// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/thread.h>

#include <logging.h>
#include <util/system.h>
#include <util/threadnames.h>

#include <exception>
#include <mutex>
#include <utility>

namespace {
std::mutex g_handler_mutex;
util::ThreadExceptionHandler g_handler;

util::ThreadExceptionHandler GetThreadExceptionHandler()
{
    std::lock_guard<std::mutex> lock(g_handler_mutex);
    return g_handler;
}
} // namespace

void util::SetThreadExceptionHandler(ThreadExceptionHandler handler)
{
    std::lock_guard<std::mutex> lock(g_handler_mutex);
    g_handler = std::move(handler);
}

void util::TraceThread(const char* thread_name, std::function<void()> thread_func)
{
    util::ThreadRename(thread_name);
    try {
        LogPrintf("%s thread start\n", thread_name);
        thread_func();
        LogPrintf("%s thread exit\n", thread_name);
    } catch (...) {
        PrintExceptionContinue(std::current_exception(), thread_name);
        if (const auto handler = GetThreadExceptionHandler(); handler && handler(thread_name, std::current_exception())) {
            LogPrintf("%s thread exit after a handled error\n", thread_name);
            return;
        }
        throw;
    }
}
