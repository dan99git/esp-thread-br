# Building OS Fork

This is a Building OS fork of `espressif/esp-thread-br`.

- Upstream: https://github.com/espressif/esp-thread-br
- Fork base: tag `v1.3` (commit `446b44d`, January 2025, built against ESP-IDF v5.5.2).
- Fork branch: `bos/v1.3`. All Building OS modifications are commits on this branch on top of the upstream `v1.3` tag.
- Upstream license: Apache 2.0 (see `LICENSE`). Preserved unmodified.

To see Building OS changes vs the upstream `v1.3` baseline:

```
git log v1.3..bos/v1.3
```

Building OS components that wrap this fork live in the outer Building OS repository at `firmware/esp-thread-br-bos/components/`. The fork itself contains only the upstream code plus any patches needed to integrate with the Building OS firmware.

## Pulling upstream updates

When Espressif releases a new tag (for example `v1.4`):

```
git fetch upstream
git checkout -b bos/v1.4 v1.4
git cherry-pick v1.3..bos/v1.3        # replay Building OS patches on the new base
# resolve any conflicts, push the new branch
```

The outer Building OS repository's submodule pointer is then updated to track `bos/v1.4`.

## Canonical spec

The Building OS border router architecture is specified in the outer repo at `docs/08.8-border-router.md`.
