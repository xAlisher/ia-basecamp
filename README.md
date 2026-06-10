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

> **Hard dependency:** reads require a synced LEZ indexer behind a trusted gateway —
> [logos-execution-zone#519](https://github.com/logos-blockchain/logos-execution-zone/issues/519).
