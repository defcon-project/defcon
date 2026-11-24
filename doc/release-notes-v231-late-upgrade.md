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
- A node that was still on an earlier version when the network's block at
  the height reached it has refused that block, and stored the refusal.
  Once upgraded it does not start on that block index either: it stops
  with "Blocks refused at or above the v23.1 activation height have to be
  validated again". Restart it once with `-reindex`, as above;
  `-reindex-chainstate` is refused the same way. (A pruned node downloads
  the chain again for this. If the node had also gone on from a block of
  the earlier rules, a plain start shows the message of the first point
  instead; the remedy is the same.)
- A node of this version asks for the same `-reindex` if it is restarted
  after it has refused a block at or above the height and before it has
  connected its first block there, a short interval at the activation.
  After a power loss or a crash this can also happen somewhat later, until
  the node has next written its block index (about hourly, and at every
  regular stop).
- A node that has connected a block at or above the height with this
  version is not affected, nor is one that holds no block from the height
  on yet (stopped before the network reached it, or switched off across
  it).
- To tell the two apart, a node marks each block it connects from the height
  on in its block index. A build made from the development branch before the
  release does not set this mark, so start a devnet or regtest data
  directory in which such a build went past the height once with
  `-reindex`.
