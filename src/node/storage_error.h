// Copyright (c) 2026 The Defcon Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_STORAGE_ERROR_H
#define BITCOIN_NODE_STORAGE_ERROR_H

#include <exception>
#include <functional>
#include <string>

/**
 * A local storage error is a database or block file this node cannot read or
 * that holds something it cannot decode: not something a peer sent, and not a
 * consensus failure. Where one surfaces off the validation path -- on a worker
 * thread, in a scheduled callback, in a retry loop -- the node must neither
 * crash (std::terminate at the thread boundary) nor carry on as if the data
 * were fine. It stops in order and names the way out, as the block import and
 * the message handler already do.
 *
 * If `exception` is such an error (dbwrapper_error), this logs it with `where`,
 * calls AbortNode with the repair hint and returns true. Any other exception
 * returns false and is left to the caller.
 */
bool HandleStorageError(std::exception_ptr exception, const std::string& where);

/** The util::TraceThread handler: HandleStorageError for the thread's error. */
bool HandleStorageErrorOnThread(const char* thread_name, std::exception_ptr exception);

/**
 * Run `work` to completion. A storage error that stops it is handled
 * (HandleStorageError: the node is asked to stop) and `work` is called again,
 * so that a queue `work` serves or drains keeps going until it is done. For
 * the scheduler threads, whose queues validation waits on
 * (SyncWithValidationInterfaceQueue) and the shutdown drains, and for the
 * shutdown's own draining of the validation callbacks, which runs outside any
 * thread boundary. Any other exception propagates.
 */
void RunHandlingStorageErrors(const std::function<void()>& work, const std::string& where);

#endif // BITCOIN_NODE_STORAGE_ERROR_H
