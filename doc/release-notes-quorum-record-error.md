Block validation
----------------

- When the node's own database is missing the record of a quorum commitment
  mined in the active chain, or holds it in a form that cannot be decoded, the
  node no longer marks the next good block as invalid, which kept it off the
  chain even after the database was rebuilt. It now stops with a message
  asking for a restart with `-reindex`, and the debug log names the quorum
  whose record is missing.
