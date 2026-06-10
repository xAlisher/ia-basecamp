# ia-basecamp — sovereign archive follower & preservation

**Follow permanent, curated archive channels and help preserve their collections to decentralized
Storage — from any machine, no node required.**

The consumer half of the Internet-Archive preservation campaign. Curators inscribe collections into
permanent channels on the **Logos Execution Zone** (Linux + zone-sequencer + Keycard); **anyone** runs
this module to follow those channels and pin their collections to Storage.

- **Permanent channels** — read from the LEZ (on-chain, append-only, verifiable), not ephemeral Waku.
- **Cross-platform** — a `ui_qml` module with a pure-HTTP C++ backend: no platform-module deps, no
  [delivery#31](https://github.com/logos-co/logos-delivery-module/issues/31) crash → builds on macOS/arm,
  Windows, Linux.
- **Preserve your way** — *delegate* to a trusted node (zero-infra) or *contribute* your own Storage.

➡ **Design & architecture: [SPEC.md](SPEC.md).** Phases tracked as GitHub issues.

> **Hard dependency:** reads require a synced LEZ gateway —
> [logos-execution-zone#519](https://github.com/logos-blockchain/logos-execution-zone/issues/519).

## Build & install (dev)

```bash
nix build .                    # plugin + replica factory + QML
./tests/backend/run.sh         # 45 deterministic backend tests (mock gateway/Storage)
./tests/integration/standalone_load_test.sh   # loads in the standalone harness
./scripts/install-lgx.sh       # dual-variant LGX → local Basecamp (then kill+relaunch)
```

The dual-variant merge matters: a dev-only LGX renders the view but never spawns the backend
(see `basecamp-skills/lgx-ui-qml-backend-dual-variant`).

Cross-platform: `flake.nix` exports `lgx-portable` for `x86_64-linux`, `aarch64-linux`,
`aarch64-darwin`; arm64/darwin artifacts build in the catalog CI (pure QtNetwork — no native
deps to port).

## Live verification (opt-in env-gated tests)

```bash
ARCHIVE_LIVE_NODE=http://<synced-node>:8080 ./build/tst_lez_client live_readKeeperStyleChannel
ARCHIVE_LIVE_STORAGE=http://127.0.0.1:5001 ARCHIVE_LIVE_CID=Qm… ./build/tst_storage_client live_pinUnpinRoundTrip
```
