# How the depends build works

The DeFCoN Core [depends Makefile](Makefile) builds dependency packages for a selected host. The build machine runs the compiler and tools. The host is the platform on which the resulting Core binaries will run.

## Sources and package builds

A [package recipe](packages.md) specifies the source archive, expected checksum, dependencies and build steps. The build fetches and verifies the archive, applies patches and stages the result into a target prefix. Downloaded sources can be reused across builds.

## Cache identity

A package build ID incorporates its recipe, build configuration and dependencies. Changes to a recipe or its inputs invalidate affected package caches. The built package is archived for reuse, and the host prefix supplies a `share/config.site` for the Core build.

Pinned sources and isolated staging reduce differences between builds. Compiler versions and other toolchain inputs also affect the resulting binaries. See [build instructions](README.md) for supported release targets and cache options.
