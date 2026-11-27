# DeFCoN build containers

The [GitHub Actions workflow](../../.github/workflows/build.yml) uses the native Linux image for its x86-64 build and test lanes. The full image also includes cross-compilers, Wine and other development tools.

From the repository root:

```sh
docker build -f contrib/containers/ci/Dockerfile.linux64 -t defcon-ci-linux64 contrib/containers/ci
docker build -f contrib/containers/ci/Dockerfile -t defcon-ci-full contrib/containers/ci
```

The [development Compose file](develop/docker-compose.yml) mounts the checkout inside the container at `/src/dash`. Its Dockerfile uses the `edrevo/dockerfile-plus` BuildKit frontend. Its port mappings are inherited from Dash and do not match DeFCoN's mainnet P2P and RPC ports.

```sh
cd contrib/containers/develop
docker compose build
docker compose run --rm container
```

The [Guix container](guix/) is retained for experiments. For node binaries, use the [official DeFCoN releases](https://github.com/defcon-project/defcon/releases).
