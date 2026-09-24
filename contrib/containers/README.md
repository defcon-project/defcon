## Containers

This directory contains configuration files for containerization utilities.

`ci` defines the build image published to GHCR by the GitHub Actions workflow.
`develop` extends it with tools for a local development environment. The `guix`
directory provides a separate container definition for Guix builds. These are
build environments, not deployment images for a running DeFCoN node.

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
