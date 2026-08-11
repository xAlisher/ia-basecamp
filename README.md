# ia-basecamp — sovereign archive follower & preservation

> This is a personal, experimental hobby project. It is not an official Logos product. Not audited.


**Follow curated, permanent archive channels and help preserve their items to
decentralized Storage — from any machine, no node required.**

<img width="1290" height="1141" alt="image" src="https://github.com/user-attachments/assets/1327de5e-8ff2-48b4-b338-e48c99c265b0" />

The consumer half of the Internet-Archive preservation campaign. Curators inscribe
lightweight entries into permanent channels on the **Logos Execution Zone**;
**anyone** runs this module to follow those channels and preserve their items to
Logos Storage.

- **Permanent channels** — read from the LEZ (on-chain, append-only, verifiable),
  not ephemeral Waku. A "trusted" channel is just one whose id the campaign
  publishes (signer key == channel id — no new trust machinery).
- **Cross-platform** — a `ui_qml` module with a pure-HTTP C++ backend (QtNetwork
  only): no platform-module deps → builds on Linux, macOS/arm, Windows.
- **Preserve = download + verify + store.** Preserving an entry pulls the bytes
  from archive.org, verifies them against IA's own published checksums, and stores
  them in Logos Storage with keeper-exact naming (so independently-preserved copies
  converge on identical dataset CIDs). Optional per-channel **auto-preserve**.

➡ Campaign model & rationale: [docs/campaign-brief.md](docs/campaign-brief.md) ·
Design: [SPEC.md](SPEC.md) · How to test every use case:
[docs/TESTING.md](docs/TESTING.md).

> **Hard dependency:** reads require a synced LEZ gateway. Inscribing fresh
> channels requires the zone-sequencer module with the fresh-channel publish fix
> (zone-sequencer-rs#3).

## Inscription payload (`ia_item`, v1)

```json
{ "v": 1, "type": "ia_item", "id": "<IA identifier>", "name": "<display name>", "size": <bytes> }
```

- One entry = one IA item. `id` is the only load-bearing field; `name`/`size` are
  display/decision fields. Parsing is permissive (unknown fields ignored).
- No CIDs in the inscription — integrity comes from IA's `{id}_files.xml` md5/sha1,
  checked after download. A checksum mismatch discards the file and fails the item
  loudly; a file IA publishes no checksum for is stored-but-counted-unverified.

## Build & install (dev)

```bash
nix build .                    # plugin + QML
./tests/backend/run.sh         # 75 deterministic backend tests (mock gateway/Storage)
./tests/integration/standalone_load_test.sh   # loads in the standalone harness
./scripts/install-lgx.sh       # dual-variant LGX → local Basecamp (then kill+relaunch)
```

The dual-variant merge matters: a dev-only LGX renders the view but never spawns the
backend (see `basecamp-skills/lgx-ui-qml-backend-dual-variant`).

Cross-platform: `flake.nix` exports `lgx-portable` for `x86_64-linux`,
`aarch64-linux`, `aarch64-darwin`; arm64/darwin artifacts build in catalog CI
(pure QtNetwork — no native deps to port).

## Live verification (opt-in, env-gated)

```bash
ARCHIVE_LIVE_NODE=http://<synced-node>:8080 ./build/tst_lez_client live_readKeeperStyleChannel
```

A ready end-to-end test channel (10 cypherpunk/privacy/freedom items, on-chain) and
the full use-case walkthrough are in [docs/TESTING.md](docs/TESTING.md).

## Scope notes

- ia_item **unpreserve** is unsupported in v1 (no per-file unpin bookkeeping); the
  per-item Remove is hidden for cid-less rows. Settings → **Remove all items**
  forgets channels + items (does not unpin from storage).
- Official channels are followed **explicitly**, not auto-seeded as built-in
  defaults (a baked-in default can't carry channels published after release — ia#19).
