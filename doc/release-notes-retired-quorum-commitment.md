Consensus, dormant until the v23.1 height: no commitment of a quorum type that is not enabled
----------------------------------------------------------------------------------------------

From the v23.1 height `llmq_400_60` and `llmq_400_85` form no more quorums. A
block could still carry an empty (null) commitment of such a type: the block
checks counted the commitments of the enabled types only, and an empty
commitment was taken on its shape. No quorum came of it; it only took room in the
block. From the v23.1 height a block that carries a commitment of a type that is
not enabled for it is refused (`bad-qc-not-allowed`), empty or not.

The honest block producer never adds such a commitment, so nothing changes for
stakers or masternodes. Nothing changes before the height either: blocks below
it are checked as before, and it stays unset on mainnet and testnet until the
release.
