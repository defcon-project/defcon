# Partially signed transactions in DeFCoN Core

DeFCoN Core wallets can prepare, sign and exchange DFCN transactions using the Partially Signed Bitcoin Transaction (PSBT) format defined in [BIP 174](https://github.com/bitcoin/bips/blob/master/bip-0174.mediawiki). Bitcoin Core introduced its PSBT RPC interface in version 0.17. A PSBT carries the unsigned transaction and signing metadata. Give signers the PSBT and public wallet information. Each signer keeps their private keys.

## Prepare a transaction

Use `createpsbt` when you already know the inputs and outputs. Use `walletcreatefundedpsbt` to select inputs, add change and attach data known to the selected wallet. For wallet RPCs choose the intended wallet with `defcon-cli -rpcwallet=<name>`. The [descriptor guide](descriptors.md#basic-multisig-example) includes a multisignature wallet example.

For an existing unsigned raw transaction, `converttopsbt` converts it to PSBT format. It rejects existing signature data by default. Use `decodepsbt` to inspect inputs, recipients, DFCN amounts and, where the input values are known, fees.

## Collect signatures

1. Each participant checks the proposed transaction and runs `walletprocesspsbt` with their signing wallet. Each signer can pass their updated PSBT to the next signer.
2. If participants signed separate copies of the same underlying transaction, combine them with `combinepsbt`.
3. Use `analyzepsbt` to see whether more metadata or signatures are needed. `finalizepsbt` returns `complete` and, when complete and extraction is enabled, the serialized transaction in `hex`.
4. After verifying the final transaction, use `sendrawtransaction` to broadcast it on the intended DeFCoN network.

For the accepted parameters of a command, run `defcon-cli help` followed by its name.
