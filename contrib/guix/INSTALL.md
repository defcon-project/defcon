# Guix installation for build experiments

Guix is optional tooling inherited from Dash Core. To build the DeFCoN release targets, follow the [depends instructions](../../depends/README.md).

For development of the [Guix build scripts](README.md), follow the GNU Guix [binary installation guide](https://guix.gnu.org/manual/en/html_node/Binary-Installation.html) for the build machine. The repository also contains a [Guix container definition](../containers/guix/). The [build manifest](manifest.scm) records the tools and packages used by the port.

For macOS cross-compilation, prepare the SDK described in the [macOS SDK instructions](../macdeploy/README.md#sdk-extraction).
