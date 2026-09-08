# FreeInk SDK patches

Two fixes found while building this firmware. Both are carried on a branch of
the `vendor/freeink-sdk` submodule and are reproduced here as patches, so the
changes are readable without checking out a fork — and so they can be offered
upstream.

| Patch | What it fixes |
|---|---|
| `0001-*` | `Uc8279X4Driver::displayWindow()` was missing entirely, so every windowed refresh request silently repainted the whole panel. |
| `0002-*` | The BLE page-turner fallback ran on ordinary boot-keyboard frames. Its guard accepted any device exposing a Consumer Control page — nearly every keyboard with media keys — so each key *release* surfaced as a phantom keypress. |

To apply them to a clean checkout of the upstream SDK:

```bash
cd vendor/freeink-sdk
git am ../../patches/*.patch
```
