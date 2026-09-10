// Copyright (c) 2012-2020 The Bitcoin Core developers
// Copyright (c) 2014-2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VERSION_H
#define BITCOIN_VERSION_H

/**
 * network protocol versioning
 */


//! 70242: the release that carries the Q60 switchover. Every v22.1.x binary
//! advertises 70241 (MN_DSL_PROTO_VERSION, 7bf3c5ad19), and the switchover
//! floor below has to tell this binary from those, so the bump is not
//! cosmetic: without it the floor admits the predecessor.
static const int PROTOCOL_VERSION = 70242;

//! initial proto version, to be increased after version/verack negotiation
static const int INIT_PROTO_VERSION = 209;

//! disconnect from peers older than this proto version
static const int MIN_PEER_PROTO_VERSION = 70216;

//! mandatory protocol after the mainnet fork-recovery activation height
static const int FORK_RECOVERY_PROTO_VERSION = 70239;

//! the highest version a binary WITHOUT the Q60 switchover advertises. 70241
//! came in with the DSL masternode-state fields (7bf3c5ad19) and every
//! v22.1.x release since speaks it. The switchover floor must sit strictly
//! above this, or it admits the very predecessor it exists to exclude --
//! which a floor of 70241 did (independent review, 2026-09-10: an
//! unmodified predecessor build stayed connected through the lead and
//! reconnected after it). Never raise this; it is history.
static const int LAST_PRE_SWITCHOVER_PROTO_VERSION = 70241;

//! mandatory protocol from the Q60 formation lead on, on any network that
//! schedules the switchover (nChainLocksV2ActivationHeight minus the lead):
//! the first version whose binaries carry the switchover. The first
//! llmq_defcon commitment is mined inside the lead and forks off every binary
//! that does not know the profile, so a peer below this is on the old chain
//! by construction. Later releases keep it as it is: it names the first
//! version that carries the switchover, not the current one.
static const int Q60_SWITCHOVER_PROTO_VERSION = 70242;
static_assert(Q60_SWITCHOVER_PROTO_VERSION > LAST_PRE_SWITCHOVER_PROTO_VERSION,
              "the Q60 floor must exclude every binary that predates the switchover; at 70241 or below it admits them");
static_assert(Q60_SWITCHOVER_PROTO_VERSION > FORK_RECOVERY_PROTO_VERSION,
              "the Q60 floor succeeds the fork-recovery floor and must sit above it");
static_assert(Q60_SWITCHOVER_PROTO_VERSION <= PROTOCOL_VERSION,
              "a floor above what this binary speaks would disconnect every peer, ourselves included");

//! minimum proto version of masternode to accept in DKGs
static const int MIN_MASTERNODE_PROTO_VERSION = 70235;

//! protocol version is included in MNAUTH starting with this version
static const int MNAUTH_NODE_VER_VERSION = 70218;

//! introduction of QGETDATA/QDATA messages
static const int LLMQ_DATA_MESSAGES_VERSION = 70219;

//! introduction of instant send deterministic lock (ISDLOCK)
static const int ISDLOCK_PROTO_VERSION = 70220;

//! GOVSCRIPT was activated in this version
static const int GOVSCRIPT_PROTO_VERSION = 70221;

//! ADDRV2 was introduced in this version
static const int ADDRV2_PROTO_VERSION = 70223;

//! BLS scheme was introduced in this version
static const int BLS_SCHEME_PROTO_VERSION = 70225;

//! Masternode type was introduced in this version
static const int DMN_TYPE_PROTO_VERSION = 70227;

//! Versioned Simplified Masternode List Entries were introduced in this version
static const int SMNLE_VERSIONED_PROTO_VERSION = 70228;

//! Versioned Simplified Masternode List Entries were introduced in this version
static const int MNLISTDIFF_VERSION_ORDER = 70229;

//! Compute masternode service descriptors were introduced in this version
static const int MN_COMPUTE_PROTO_VERSION = 70240;

//! Service-PoSe (DSL) masternode-state fields were introduced in this version
static const int MN_DSL_PROTO_VERSION = 70241;

//! Masternode type was introduced in this version
static const int MNLISTDIFF_CHAINLOCKS_PROTO_VERSION = 70230;

//! Legacy ISLOCK messages and a corresponding INV were dropped in this version
static const int NO_LEGACY_ISLOCK_PROTO_VERSION = 70231;

//! Inventory type for DSQ messages added
static const int DSQ_INV_VERSION = 70234;

//! Maximum header count for HEADRES2 message was increased from 2000 to 8000 in this version
static const int INCREASE_MAX_HEADERS2_VERSION = 70235;

//! Behavior of QRINFO is changed in this protocol version
static const int EFFICIENT_QRINFO_VERSION = 70236;

//! cycleHash in isdlock message switched to using quorum's base block in this version
static const int ISDLOCK_CYCLEHASH_UPDATE_VERSION = 70237;

// Make sure that none of the values above collide with `ADDRV2_FORMAT`.

#endif // BITCOIN_VERSION_H
