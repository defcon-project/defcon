## Containers

This directory contains configuration files for containerization utilities.

`ci/Dockerfile.linux64` defines the native Linux build image published to GHCR
by the GitHub Actions workflow. It contains the GCC, Qt host dependencies and
Python tools used by the linux64 build, unit, Qt/leak and functional gates.
It keeps the full image's Python version and mount paths, but omits cross
compilers, Wine, LLVM, Valgrind and the standalone lint toolchains.

`ci/Dockerfile` remains the full environment for cross builds, sanitizers and
lint tools. `develop` extends that full image for local development. The `guix`
directory provides a separate container definition for Guix builds. These are
build environments, not deployment images for a running DeFCoN node.

From the repository root, build either environment explicitly:

```bash
docker build -f contrib/containers/ci/Dockerfile.linux64 -t defcon-ci-linux64 contrib/containers/ci
docker build -f contrib/containers/ci/Dockerfile -t defcon-ci-full contrib/containers/ci
```

The workflow retains its existing GHCR image name and tags. PRs consume the
latest published image; pushes to the release branch or a release tag build
and publish it. When this change first lands, its PR still uses the previous
full image; the merge push publishes and tests the native image. When adding
a different CI target or lint/sanitizer gate, select the full environment or
add its required tools explicitly.

### Usage Guide

We utilise edrevo's [dockerfile-plus](https://github.com/edrevo/dockerfile-plus), a syntax extension that
leverages Docker [BuildKit](https://docs.docker.com/develop/develop-images/build_enhancements/) to reduce
the amount of repetitive code.

As BuildKit is opt-in within many currently supported versions of Docker (as of this writing), you need to
set the following environment variables before continuing. While not needed after the initial `docker-compose build`
(barring updates to the `Dockerfile`), we recommend placing this in your `~/.bash_profile`/`~/.zshrc` or equivalent

```bash
export DOCKER_BUILDKIT=1
export COMPOSE_DOCKER_CLI_BUILD=1
```

After that, it's simply a matter of building and running your own development container. You can use extensions
for your IDE like Visual Studio Code's [Remote Containers](https://code.visualstudio.com/docs/remote/containers)
to run terminal commands from inside the terminal and build DeFCoN Core.

```bash
cd contrib/containers/develop
docker-compose build
docker-compose run container
```
