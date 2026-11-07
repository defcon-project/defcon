// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_THREAD_H
#define BITCOIN_UTIL_THREAD_H

#include <exception>
#include <functional>

namespace util {
/**
 * A wrapper for do-something-once thread functions.
 *
 * An exception leaving thread_func is logged. If a handler set with
 * SetThreadExceptionHandler accepts it (returns true), the thread ends there;
 * otherwise it is rethrown, which ends the process (std::terminate).
 */
void TraceThread(const char* thread_name, std::function<void()> thread_func);

/**
 * The handler TraceThread consults for an exception leaving a thread. It is
 * meant for the kinds of error that call for an orderly shutdown instead of a
 * crash, such as a local database that cannot be read. Set once at startup,
 * before threads are started; an empty handler restores the default (rethrow).
 */
using ThreadExceptionHandler = std::function<bool(const char* thread_name, std::exception_ptr exception)>;
void SetThreadExceptionHandler(ThreadExceptionHandler handler);

} // namespace util

#endif // BITCOIN_UTIL_THREAD_H
