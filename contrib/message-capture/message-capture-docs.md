# Capture peer messages

Start `defcond` with `-capturemessages` to record the messages your node exchanges with each peer. Under `message_capture/` in the network data directory, each peer endpoint has its own folder. Incoming and outgoing records are stored in `msgs_recv.dat` and `msgs_sent.dat`.

For a Linux mainnet node using the default data directory, run the [parser](message-capture-parser.py) from the repository root:

```sh
python3 contrib/message-capture/message-capture-parser.py \
    -o out.json ~/.defcon/message_capture/*/*.dat
```

The parser merges the selected records by timestamp and writes JSON. Omit `-o out.json` to write to standard output. It uses the [functional test framework](../../test/functional/README.md) to decode protocol messages.
