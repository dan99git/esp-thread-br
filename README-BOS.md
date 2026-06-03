# Building OS Fork

This is a Building OS fork of `espressif/esp-thread-br`.

- Upstream: https://github.com/espressif/esp-thread-br
- Fork base: upstream `main`, with Building OS integration changes kept in this fork.
- Building OS BR firmware example: `examples/building_os_border_router/`.
- Building OS bare C6 TMFS bench peer: `examples/bos_c6_tmfs_test/`.
- Upstream license: Apache 2.0 (see `LICENSE`). Preserved unmodified.

To see Building OS changes vs the upstream remote:

```
git log upstream/main..main
```

Building OS firmware code lives in this repository, not in the outer Building OS
monorepo. The outer Building OS repository may document the architecture and
host-side APIs, but BR app firmware belongs here.

## Pulling upstream updates

When Espressif updates upstream:

```
git fetch upstream
git checkout main
git merge upstream/main
# resolve conflicts, validate examples/building_os_border_router, push main
```

Do not put new BR firmware implementation in the outer Building OS repository.

## Canonical spec

The Building OS border router architecture is specified in the outer Building OS
repo at `docs/08.8-border-router.md`.
