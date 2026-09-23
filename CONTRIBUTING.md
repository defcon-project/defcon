# Contributing to DeFCoN Core

DeFCoN Core is open to contributions in the form of code, review, testing and documentation. This
document describes how changes get into the repository.

## Before you start

- **Security issues are never reported or fixed in public.** Follow [`SECURITY.md`](SECURITY.md).
- **Questions and support** belong on the [DeFCoN Discord](https://discord.gg/UpUq7V7Fg7), not in the
  issue tracker.
- For anything larger than a small fix, open an issue first so the approach can be agreed before the
  work is done. Consensus changes in particular need agreement on the design and on how they activate.

## Making a change

1. Fork the repository and branch from the default branch.
2. Keep each pull request to one logical change. Unrelated fixes go in separate pull requests.
3. Add or update tests with the change: unit tests in [`src/test/`](src/test/), functional tests in
   [`test/functional/`](test/functional/). A pull request that changes behaviour without a test that
   covers it will be asked for one.
4. Follow the style of the surrounding code; [`doc/developer-notes.md`](doc/developer-notes.md) has the
   conventions inherited from upstream.

### Commit messages

Start the subject with the area the change touches, followed by a short summary in the imperative:

```
consensus: reject a proof-of-stake block whose time does not advance
qt: keep the collateral address in the masternode tab's essential view
test: cover the coinbase bound at superblock heights
```

Common areas: `consensus`, `chainparams`, `validation`, `net`, `rpc`, `wallet`, `qt`, `llmq`, `dsl`,
`pos`, `build`, `ci`, `doc`, `test`. Use the body to explain *why* the change is needed.

### Consensus changes

A change to what makes a block or transaction valid must be gated on an activation height, tested on
both sides of that height, and must leave every existing block valid. Heights for mainnet and testnet
are set together at release time, never piecemeal.

### Backports from upstream

Changes taken from Dash Core or Bitcoin Core should say so in the pull request, with the upstream pull
request number, and keep the original author credited.

## Review and merging

- Every pull request is built and tested by GitHub Actions; a pull request is merged only with a green
  build.
- At least one maintainer reviews every change. Consensus and security-relevant changes get an
  independent review before merging.
- Address review comments with new commits while the review is ongoing; the history is tidied before
  merge if needed.

## License

By contributing, you agree that your contributions are licensed under the MIT license (see
[`COPYING`](COPYING)), unless a file states otherwise.
