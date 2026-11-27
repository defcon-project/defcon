# Manage a DeFCoN Core wallet

## Create and select a wallet

Choose **Create Wallet** in the desktop wallet, or use the `createwallet` RPC. To create a descriptor wallet and load it on future startups:

```sh
defcon-cli -named createwallet wallet_name=wallet-01 descriptors=true load_on_startup=true
```

Descriptor wallets require SQLite support and an explicit `load_on_startup` choice in this release. The RPC defaults to a legacy wallet when `descriptors` is omitted. To select a loaded wallet for subsequent commands, pass `-rpcwallet=wallet-01`.

By default the node uses the `wallets` subdirectory of the network data directory if it exists, otherwise the network data directory itself. Mainnet defaults are shown below. A configured `-datadir` or `-walletdir` can change these locations.

| Operating system | Mainnet data directory |
| --- | --- |
| Linux | `~/.defcon/` |
| Windows | `%APPDATA%\Defcon\` |
| macOS | `~/Library/Application Support/Defcon/` |

## Encrypt and unlock

A wallet is not encrypted unless you set a passphrase when you create it or encrypt it later. Anyone who can read an unencrypted wallet file or backup can spend the coins controlled by the private keys in it. Choose **Encrypt Wallet** to protect the wallet’s private keys with a passphrase. Wallet history and addresses remain readable. Encryption does not protect against malware, such as a keylogger, on the same machine.

There is no way to recover a lost passphrase: coins whose keys exist only in the encrypted wallet are lost with it. Keep a safe copy of the passphrase and make a fresh wallet backup right after encryption. An older unencrypted backup still gives access to the private keys it contains; for a legacy HD wallet, that includes every key derived from its HD seed. A descriptor wallet that is not blank gets new descriptors from a new seed when it is first encrypted, so only a backup made after encryption covers its new addresses. Replace or securely destroy older unencrypted backups.

Use **Unlock Wallet** in the desktop wallet to unlock an encrypted wallet. With the CLI, the following command prompts for the passphrase and unlocks the selected wallet for 120 seconds:

```sh
defcon-cli -rpcwallet=wallet-01 -stdinwalletpassphrase walletpassphrase 120
```

To unlock for staking and CoinJoin only, append `true` after the timeout. To lock immediately, run `defcon-cli -rpcwallet=wallet-01 walletlock`.

## Back up

Make backups with **Backup Wallet** or the `backupwallet` RPC, which copy the wallet in a consistent state. Do not copy the wallet file of a running node by hand. Select the wallet and choose **Backup Wallet**, or give the `backupwallet` RPC a new filename in an existing directory. The destination is on the machine running the node and must be writable by its service account.

```sh
defcon-cli -rpcwallet=wallet-01 backupwallet /path/to/backups/wallet-01.dat
```

Keep backups on protected offline storage, and do not restore them on a machine you suspect of malware. If the wallet and every backup are lost, its coins cannot be recovered. Refresh backups after encrypting the wallet, changing its passphrase, upgrading it to HD (`upgradetohd`) or setting a new HD seed (`sethdseed`), importing keys or descriptors, or changing multisig scripts. Imported keys, additional descriptors and address-book labels can postdate a backup. HD wallets, inherited from upstream Dash Core 0.12.2, derive their addresses from the seed, but a deterministic seed alone does not capture all wallet contents. A non-HD wallet also needs regular backups before its pool of previously backed-up keys is exhausted.

## Restore and verify

Give `restorewallet` the backup’s path on the node and a new, unused wallet name. Check the loaded wallet, allow its scan to finish, and verify known addresses and history.

```sh
defcon-cli restorewallet restored-wallet /path/to/backups/wallet-01.dat
defcon-cli -rpcwallet=restored-wallet getwalletinfo
```

Use a separate, trusted installation to test a backup without disturbing the working wallet.
