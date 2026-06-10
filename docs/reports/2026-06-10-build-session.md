# Build session report — 2026-06-10 (P0–P6, autonomous)

All seven phases of ia-basecamp (P0–P6, issues #2–#8) implemented, tested, Senty-reviewed,
and pushed in one session. 45/45 deterministic backend tests; production code proven live
against the real chain (sneg node) and a real Kubo daemon. P3 UI render confirmed on screen.

## The one big design correction (P0 spike)

The SPEC's `getTransactionsByAccount` read path does not exist for curated channels — the
jsonrpsee indexer borsh-decodes inscriptions as zone blocks, so raw-JSON channels are
invisible to it, and there is **no channelId→account_id mapping**. The working path
(validated live; what upstream's `l2-sequencer-archival-demo` archiver does) is the gateway
node's Cryptarchia HTTP API, filtering `ChannelInscribe` (opcode 17) by `channel_id` over
finalized slots. Documented in SPEC §4.1 + `docs/spikes/p0-channel-read.md`. The `.rep`
contract did not change.

## Per phase (each: tests in-change → Senty review → HIGH/MEDIUM fixed → retro)

| Phase | Branch | What shipped | Review highlight |
|-------|--------|--------------|------------------|
| P0 | `p0-confirm-and-spike` | load smoke test, spike doc, dual-LGX install fix | — |
| P1 | `p1-read-channels` | `lez_client`: follow/refresh, slot cursors, finalized-only, health/lag; live test decoded the real `ia:kuMUquaeE6g` inscription | generation guards: in-flight scan vs unfollow/re-follow of the same id |
| P2 | `p2-preserve` | `storage_client`: Kubo pin/unpin, streamed progress, delegate\|local | **live test caught Kubo 403ing Qt's default Mozilla UA** — all mocks were green |
| P3 | `p3-ui` | pills (gateway+lag/storage/mode), settings, 3 tabs, activity log | edge-triggered activity log; `call()` TypeError guard |
| P4 | `p4-federation` | lag-based gateway demotion; scans pinned to one gateway; staleness banner | mid-scan rotation could mix two gateways' finalized views → silent skips; real mid-pagination death test |
| P6 | `p6-share-cards` | share SLOTs + `share_helper` + offscreen 1200×675 ShareCard → PNG → reveal | chain-inscribed thumbnail → `Image.source` SSRF → CID-shape allowlist |
| P5 | `p5-package` | final dual LGX installed; README docs; catalog PR [logos-basecamp-modules#1](https://github.com/xAlisher/logos-basecamp-modules/pull/1) (3-arch workflow) | — |

Branches are stacked and linear; merging `p5-package` → main fast-forwards everything.

## Platform skills extracted (basecamp-skills)

- `lgx-ui-qml-backend-dual-variant` — `.#lgx` dev-only manifest lacks `main["linux-amd64"]`;
  view renders, backend silently never spawns. Fix: `scripts/install-lgx.sh` dual merge.
- `kubo-rpc-qt-user-agent` — Kubo CSRF guard 403s Mozilla-UA requests; set a CLI-class UA.
- Module-local: `docs/skills/async-scan-generation-guard.md`.

## Verification matrix

| Claim | Evidence |
|-------|----------|
| Module builds | `nix build .` clean (every phase) |
| Loads + replica syncs | standalone harness smoke, 4 greps green (every phase) |
| Read path works on the real chain | `live_readKeeperStyleChannel` green vs sneg |
| Preserve works on real Storage | `live_pinUnpinRoundTrip` green vs Kubo 0.33 |
| UI renders with live state | screenshot: `Gateway ready` pill from sneg |
| In-Basecamp backend spawn | **PENDING manual** (dual LGX installed, needs click-through) |
| Share grab→save→reveal | **PENDING manual** (backend half unit-tested) |
| arm64/darwin artifacts | **PENDING catalog CI** post-merge (flake outputs eval) |

## Launch blocker (not code)

Public explorer fully 502 (worse than LEZ#519's stall; datapoint logged on issue #1).
Sneg (Tailscale) is the only open synced gateway — campaign gateway provisioning gates any
workshop/public use.
