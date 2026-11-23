A node upgraded after the v23.1 height
--------------------------------------

- A node that was still on an earlier version at the v23.1 activation height
  may have gone on from there on blocks that only the earlier rules allow: its
  own, or those of other nodes that had not upgraded. Once upgraded, such a
  node does not start on that chain. It stops with "Blocks at or above the
  v23.1 activation height were accepted by an earlier version and have to be
  validated again", and the log names the block. Restart it once with
  `-reindex` (the wallet offers to rebuild the block database; answer yes): the
  node validates the blocks it has stored again, leaves out those the v23.1
  rules refuse, and gets the rest from the network.
- A node that ran this version when the chain reached the height is not
  affected, nor is one that stopped below the height or was switched off
  across it.
- To tell the two apart, a node marks each block it connects from the height
  on in its block index. A build made from the development branch before this
  change does not set the mark, so a devnet or regtest data directory in which
  such a build went past the height needs one `-reindex` too.
