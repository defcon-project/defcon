#!/usr/bin/env python3
# Copyright (c) 2017-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test loadblock option

Test the option to start a node with the option loadblock which loads
a serialized blockchain from a file (usually called bootstrap.dat).
To generate that file this test uses the helper scripts available
in contrib/linearize.
"""

import glob
import os
import shutil
import subprocess
import sys
import tempfile
import urllib

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class LoadblockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.supports_cli = False

    def run_test(self):
        self.nodes[1].setnetworkactive(state=False)
        self.nodes[2].setnetworkactive(state=False)
        self.generate(self.nodes[0], COINBASE_MATURITY // 2, sync_fun=self.no_op)
        # The second half of the chain is in a later month, for the monthly split below.
        self.bump_mocktime(40 * 24 * 60 * 60, update_schedulers=False)
        self.generate(self.nodes[0], COINBASE_MATURITY // 2, sync_fun=self.no_op)
        height = 2 * (COINBASE_MATURITY // 2)  # both halves above

        # Parsing the url of our node to get settings for config file
        data_dir = self.nodes[0].datadir
        node_url = urllib.parse.urlparse(self.nodes[0].url)
        cfg_file = os.path.join(data_dir, "linearize.cfg")
        bootstrap_file = os.path.join(self.options.tmpdir, "bootstrap.dat")
        genesis_block = self.nodes[0].getblockhash(0)
        blocks_dir = os.path.join(data_dir, self.chain, "blocks")
        hash_list = tempfile.NamedTemporaryFile(dir=data_dir,
                                                mode='w',
                                                delete=False,
                                                encoding="utf-8")

        self.log.info("Create linearization config file")
        with open(cfg_file, "a", encoding="utf-8") as cfg:
            cfg.write(f"datadir={data_dir}\n")
            cfg.write(f"rpcuser={node_url.username}\n")
            cfg.write(f"rpcpassword={node_url.password}\n")
            cfg.write(f"port={node_url.port}\n")
            cfg.write(f"host={node_url.hostname}\n")
            cfg.write(f"output_file={bootstrap_file}\n")
            cfg.write(f"max_height={height}\n")
            cfg.write(f"netmagic=fcc1b7dc\n")
            cfg.write(f"input={blocks_dir}\n")
            cfg.write(f"genesis={genesis_block}\n")
            cfg.write(f"hashlist={hash_list.name}\n")

        base_dir = self.config["environment"]["SRCDIR"]
        linearize_dir = os.path.join(base_dir, "contrib", "linearize")

        self.log.info("Run linearization of block hashes")
        linearize_hashes_file = os.path.join(linearize_dir, "linearize-hashes.py")
        subprocess.run([sys.executable, linearize_hashes_file, cfg_file],
                       stdout=hash_list,
                       check=True)

        self.log.info("Run linearization of block data")
        linearize_data_file = os.path.join(linearize_dir, "linearize-data.py")
        subprocess.run([sys.executable, linearize_data_file, cfg_file],
                       check=True)

        self.log.info("Restart second, unsynced node with bootstrap file")
        self.restart_node(1, extra_args=[f"-loadblock={bootstrap_file}"])
        assert_equal(self.nodes[1].getblockcount(), height)  # start_node is blocking on all block files being imported

        assert_equal(self.nodes[1].getblockchaininfo()['blocks'], height)
        assert_equal(self.nodes[0].getbestblockhash(), self.nodes[1].getbestblockhash())

        self.log.info("Refuse a monthly split into one output file, which would keep only the last month")
        split_cfg_file = os.path.join(data_dir, "linearize-split.cfg")
        shutil.copy(cfg_file, split_cfg_file)
        with open(split_cfg_file, "a", encoding="utf-8") as cfg:
            cfg.write("split_timestamp=1\n")
        with open(bootstrap_file, "rb") as f:
            bootstrap_data = f.read()
        refused = subprocess.run([sys.executable, linearize_data_file, split_cfg_file],
                                 capture_output=True,
                                 text=True,
                                 check=False)
        assert_equal(refused.returncode, 1)
        assert "split_timestamp needs an output directory" in refused.stdout
        with open(bootstrap_file, "rb") as f:
            assert_equal(f.read(), bootstrap_data)

        self.log.info("Split the chain into monthly files in a directory")
        monthly_dir = os.path.join(self.options.tmpdir, "monthly")
        os.mkdir(monthly_dir)
        with open(split_cfg_file, "a", encoding="utf-8") as cfg:
            cfg.write(f"output={monthly_dir}\n")
        subprocess.run([sys.executable, linearize_data_file, split_cfg_file],
                       check=True)
        monthly_files = sorted(glob.glob(os.path.join(monthly_dir, "blk*.dat")))
        assert_equal(len(monthly_files), 2)

        self.log.info("Load both months on a third, unsynced node")
        self.restart_node(2, extra_args=[f"-loadblock={file}" for file in monthly_files])
        assert_equal(self.nodes[2].getblockcount(), height)
        assert_equal(self.nodes[0].getbestblockhash(), self.nodes[2].getbestblockhash())


if __name__ == '__main__':
    LoadblockTest().main()
