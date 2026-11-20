#!/usr/bin/env python3
# Copyright (c) 2019-2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import random
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.messages import (
    COIN,
    CTransaction,
    CTxOut,
    uint256_to_string,
)
from test_framework.script import (
    CScript,
    OP_CHECKSIG,
)
from test_framework.util import (
    assert_equal,
    assert_is_hex_string,
    assert_raises_rpc_error,
    force_finish_mnsync,
)

# See coinjoin/options.h
COINJOIN_ROUNDS_DEFAULT = 4
COINJOIN_ROUNDS_MAX = 16
COINJOIN_ROUNDS_MIN = 2
COINJOIN_TARGET_MAX = 500_000_000  # src/coinjoin/options.h: MAX_MONEY / COIN
COINJOIN_TARGET_MIN = 2

class CoinJoinTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        # Keep automatic backups from stopping mixing before the keypool test.
        self.extra_args = [['-keypool=200', '-createwalletbackups=10']]
        if not self.options.descriptors:
            self.extra_args[0].append('-usehd=0')

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_nodes(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_nodes()

    def run_test(self):
        node = self.nodes[0]

        node.createwallet(wallet_name='w1', blank=True, disable_private_keys=False)
        w1 = node.get_wallet_rpc('w1')
        self.test_salt_presence(w1)
        self.test_coinjoin_start_stop(w1)
        self.test_setcoinjoinamount(w1)
        self.test_setcoinjoinrounds(w1)
        self.test_coinjoinsalt(w1)
        w1.unloadwallet()

        if not self.options.descriptors:
            node.createwallet(wallet_name='w_keypool', blank=False, disable_private_keys=False)
            w_keypool = node.get_wallet_rpc('w_keypool')
            self.generate(node, 2)
            force_finish_mnsync(node)
            self.test_newkeypool_stops_mixing(w_keypool)
            w_keypool.unloadwallet()
        else:
            self.log.info('newkeypool is incompatible with descriptor wallets')

        node.createwallet(wallet_name='w2', blank=True, disable_private_keys=True)
        w2 = node.get_wallet_rpc('w2')
        self.test_coinjoinsalt_disabled(w2)
        w2.unloadwallet()

        self.test_dfcn_scale_coin(node)

    def test_salt_presence(self, node):
        self.log.info('Salt should be automatically generated in new wallet')
        # Will raise exception if no salt generated
        assert_is_hex_string(node.coinjoinsalt('get'))

    def test_coinjoin_start_stop(self, node):
        self.log.info('"coinjoin" subcommands should update mixing status')
        # Start mix session and ensure it's reported
        node.coinjoin('start')
        cj_info = node.getcoinjoininfo()
        assert_equal(cj_info['enabled'], True)
        assert_equal(cj_info['running'], True)
        # Repeated start should yield error
        assert_raises_rpc_error(-32603, 'Mixing has been started already.', node.coinjoin, 'start')

        # Stop mix session and ensure it's reported
        node.coinjoin('stop')
        cj_info = node.getcoinjoininfo()
        assert_equal(cj_info['enabled'], True)
        assert_equal(cj_info['running'], False)
        # Repeated stop should yield error
        assert_raises_rpc_error(-32603, 'No mix session to stop', node.coinjoin, 'stop')

        # Reset mix session
        assert_equal(node.coinjoin('reset'), "Mixing was reset")

    def test_newkeypool_stops_mixing(self, node):
        self.log.info('sethdseed keypool refresh should stop mixing without reversing the wallet/map lock order')
        # The real mixing path holds the map and then cs_wallet. NewKeyPool
        # calls its stop callback with cs_wallet held (Dash #7259).
        with self.nodes[0].wait_for_debug_log([b'Not enough funds to mix.']):
            node.coinjoin('start')
            assert_equal(node.getcoinjoininfo()['running'], True)
        # This fork has no standalone newkeypool RPC. Its existing sethdseed
        # path calls NewKeyPool under cs_wallet and cs_KeyStore for a non-HD wallet.
        node.sethdseed(True)
        assert_equal(node.getcoinjoininfo()['running'], False)

    def test_setcoinjoinamount(self, node):
        self.log.info('"setcoinjoinamount" should update mixing target')
        # Test normal and large values
        for value in [COINJOIN_TARGET_MIN, 50, 1200000, COINJOIN_TARGET_MAX]:
            node.setcoinjoinamount(value)
            assert_equal(node.getcoinjoininfo()['max_amount'], value)
        # Test values below minimum and above maximum
        for value in [COINJOIN_TARGET_MIN - 1, COINJOIN_TARGET_MAX + 1]:
            assert_raises_rpc_error(-8, "Invalid amount of DFCN as mixing goal amount", node.setcoinjoinamount, value)

    def test_setcoinjoinrounds(self, node):
        self.log.info('"setcoinjoinrounds" should update mixing rounds')
        # Test acceptable values
        for value in [COINJOIN_ROUNDS_MIN, COINJOIN_ROUNDS_DEFAULT, COINJOIN_ROUNDS_MAX]:
            node.setcoinjoinrounds(value)
            assert_equal(node.getcoinjoininfo()['max_rounds'], value)
        # Test values below minimum and above maximum
        for value in [COINJOIN_ROUNDS_MIN - 1, COINJOIN_ROUNDS_MAX + 1]:
            assert_raises_rpc_error(-8, "Invalid number of rounds", node.setcoinjoinrounds, value)

    def test_coinjoinsalt(self, node):
        self.log.info('"coinjoinsalt generate" should fail if salt already present')
        assert_raises_rpc_error(-32600, 'Wallet "w1" already has set CoinJoin salt!', node.coinjoinsalt, 'generate')

        self.log.info('"coinjoinsalt" subcommands should succeed if no balance and not mixing')
        # 'coinjoinsalt generate' should return a new salt if overwrite enabled
        s1 = node.coinjoinsalt('get')
        assert_equal(node.coinjoinsalt('generate', True), True)
        s2 = node.coinjoinsalt('get')
        assert s1 != s2

        # 'coinjoinsalt get' should fetch newly generated value (i.e. new salt should persist)
        node.unloadwallet('w1')
        node.loadwallet('w1')
        node = self.nodes[0].get_wallet_rpc('w1')
        assert_equal(s2, node.coinjoinsalt('get'))

        # 'coinjoinsalt set' should work with random hashes
        s1 = uint256_to_string(random.getrandbits(256))
        node.coinjoinsalt('set', s1)
        assert_equal(s1, node.coinjoinsalt('get'))
        assert s1 != s2

        # 'coinjoinsalt set' shouldn't work with nonsense values
        s2 = format(0, '064x')
        assert_raises_rpc_error(-8, "Invalid CoinJoin salt value", node.coinjoinsalt, 'set', s2, True)
        s2 = s2[0:63] + 'h'
        assert_raises_rpc_error(-8, "salt must be hexadecimal string (not '%s')" % s2, node.coinjoinsalt, 'set', s2, True)

        self.log.info('"coinjoinsalt generate" and "coinjoinsalt set" should fail if mixing')
        # Start mix session
        node.coinjoin('start')
        assert_equal(node.getcoinjoininfo()['running'], True)

        # 'coinjoinsalt generate' and 'coinjoinsalt set' should fail when mixing
        assert_raises_rpc_error(-4, 'Wallet "w1" is currently mixing, cannot change salt!', node.coinjoinsalt, 'generate', True)
        assert_raises_rpc_error(-4, 'Wallet "w1" is currently mixing, cannot change salt!', node.coinjoinsalt, 'set', s1, True)

        # 'coinjoinsalt get' should still work
        assert_equal(node.coinjoinsalt('get'), s1)

        # Stop mix session
        node.coinjoin('stop')
        assert_equal(node.getcoinjoininfo()['running'], False)

        # 'coinjoinsalt generate' and 'coinjoinsalt set' should start working again
        assert_equal(node.coinjoinsalt('generate', True), True)
        assert_equal(node.coinjoinsalt('set', s1, True), True)

    def test_coinjoinsalt_disabled(self, node):
        self.log.info('"coinjoinsalt" subcommands should fail if private keys disabled')
        for subcommand in ['generate', 'get']:
            assert_raises_rpc_error(-32600, 'Wallet "w2" has private keys disabled, cannot perform CoinJoin!', node.coinjoinsalt, subcommand)
        s1 = uint256_to_string(random.getrandbits(256))
        assert_raises_rpc_error(-32600, 'Wallet "w2" has private keys disabled, cannot perform CoinJoin!', node.coinjoinsalt, 'set', s1)

    def test_dfcn_scale_coin(self, node):
        self.log.info('A DFCN-scale coin (CJ4) is listed by its rounds')
        amount = Decimal('100.001')
        miner = node.get_deterministic_priv_key()
        node.createwallet(wallet_name='w3', blank=False, disable_private_keys=False)
        w3 = node.get_wallet_rpc('w3')
        w3.importprivkey(privkey=miner.key, label='coinbase', rescan=True)
        node.createwallet(wallet_name='w4', blank=False, disable_private_keys=False)  # the other party of the mixes
        w4 = node.get_wallet_rpc('w4')
        w3.sendtoaddress(w4.getnewaddress(), 2000)
        self.generate(node, 1, sync_fun=self.no_op)
        w3.setcoinjoinrounds(COINJOIN_ROUNDS_MIN)  # fully mixed from 2 + 3 random rounds on

        def types_of(txid, vout):
            return [coin_type for coin_type in range(4)
                    if any(u['txid'] == txid and u['vout'] == vout for u in w3.listunspent(1, 9999999, [], True, {'coinType': coin_type}))]

        def entry(txid, vout):
            return next(u for u in w3.listunspent(1, 9999999, [], True) if u['txid'] == txid and u['vout'] == vout)

        # An exact-amount P2PK output to an own key: listed as ready to mix (the help says it is no mixing candidate)
        pubkey = w3.getaddressinfo(miner.address)['pubkey']
        p2pk = CTransaction()
        p2pk.vout.append(CTxOut(int(amount * COIN), CScript([bytes.fromhex(pubkey), OP_CHECKSIG])))
        signed_p2pk = w3.signrawtransactionwithwallet(w3.fundrawtransaction(p2pk.serialize().hex())['hex'])['hex']
        p2pk_tx = w3.decoderawtransaction(signed_p2pk)
        w3.sendrawtransaction(signed_p2pk)
        self.generate(node, 1, sync_fun=self.no_op)
        p2pk_vout = next(o['n'] for o in p2pk_tx['vout'] if o['scriptPubKey']['hex'] == p2pk.vout[0].scriptPubKey.hex())
        assert_equal(entry(p2pk_tx['txid'], p2pk_vout)['amount'], amount)
        assert_equal(types_of(p2pk_tx['txid'], p2pk_vout), [0, 2])
        # Spent on to the other wallet, so that later the fully mixed coin is w3's only DFCN-scale coin
        to_w4 = w3.createrawtransaction([{'txid': p2pk_tx['txid'], 'vout': p2pk_vout}], [{w4.getnewaddress(): amount - Decimal('0.001')}])
        w3.sendrawtransaction(w3.signrawtransactionwithwallet(to_w4)['hex'])
        self.generate(node, 1, sync_fun=self.no_op)

        address = w3.getnewaddress()
        txid = w4.sendtoaddress(address, amount)  # paid by another wallet: no own input
        self.generate(node, 1, sync_fun=self.no_op)
        vout = next(u['vout'] for u in w3.listunspent(1, 9999999, [address]) if u['txid'] == txid)
        assert_equal(entry(txid, vout)['coinjoin_rounds'], 0)  # an ordinary incoming DFCN-scale coin; v23.0.0 read -2
        assert_equal(types_of(txid, vout), [0, 2])  # ready to mix, not fully mixed, not an ordinary coin
        assert_equal(w3.getbalances()['mine']['coinjoin'], 0)
        assert_equal(w3.getwalletinfo()['coinjoin_balance'], 0)
        # With CoinJoin enabled the salt guard counts an unmixed DFCN-scale coin (here the only one, of zero rounds)
        salt = uint256_to_string(random.getrandbits(256))
        assert_raises_rpc_error(-4, 'Wallet "w3" has CoinJoin balance, cannot continue!', w3.coinjoinsalt, 'set', salt)

        self.log.info('Five real mixes, zero-fee and mined directly: a fully mixed DFCN-scale coin')

        def script_of(addr):
            return w3.getaddressinfo(addr)['scriptPubKey']

        def mix(own_txid, own_vout):
            foreign = w4.getnewaddress()
            foreign_txid = w4.sendtoaddress(foreign, amount)
            self.generate(node, 1, sync_fun=self.no_op)
            foreign_vout = next(u['vout'] for u in w4.listunspent(1, 9999999, [foreign]) if u['txid'] == foreign_txid)
            own_to, foreign_to = w3.getnewaddress(), w4.getnewaddress()
            ins = sorted([{'txid': own_txid, 'vout': own_vout}, {'txid': foreign_txid, 'vout': foreign_vout}],
                         key=lambda i: (i['txid'], i['vout']))  # BIP69: the txid as shown, then the index
            outs = sorted([own_to, foreign_to], key=script_of)  # BIP69: equal amounts, then the script
            raw = w3.createrawtransaction(ins, [{a: amount} for a in outs])
            raw = w3.signrawtransactionwithwallet(raw, [], 'ALL|ANYONECANPAY')['hex']
            signed = w4.signrawtransactionwithwallet(raw, [], 'ALL|ANYONECANPAY')
            assert signed['complete']
            self.generateblock(node, miner.address, [signed['hex']], sync_fun=self.no_op)
            tx = w3.decoderawtransaction(signed['hex'])
            return tx['txid'], next(o['n'] for o in tx['vout'] if o['scriptPubKey']['hex'] == script_of(own_to))

        for r in range(1, 6):
            txid, vout = mix(txid, vout)
            assert_equal(entry(txid, vout)['coinjoin_rounds'], r)
        assert_equal(types_of(txid, vout), [0, 1])  # fully mixed: neither ready to mix nor an ordinary coin
        assert_equal(w3.getbalances()['mine']['coinjoin'], amount)
        assert_equal(w3.getwalletinfo()['coinjoin_balance'], amount)

        self.log.info('The salt guard with the fully mixed coin, w3\'s only DFCN-scale coin; generate\'s own guard comes first')
        assert_raises_rpc_error(-4, 'Wallet "w3" has CoinJoin balance, cannot continue!', w3.coinjoinsalt, 'set', salt)
        assert_raises_rpc_error(-32600, 'Wallet "w3" already has set CoinJoin salt!', w3.coinjoinsalt, 'generate')
        assert_equal(w3.coinjoinsalt('set', salt, True), True)

        self.log.info('With CoinJoin disabled the salt guard leaves the DFCN-scale coin out, as it leaves the old amounts')
        self.restart_node(0, extra_args=self.extra_args[0] + ['-enablecoinjoin=0'])
        node.loadwallet('w3')
        w3 = node.get_wallet_rpc('w3')
        assert_equal(w3.coinjoinsalt('set', uint256_to_string(random.getrandbits(256))), True)
        w3.unloadwallet()

if __name__ == '__main__':
    CoinJoinTest().main()
