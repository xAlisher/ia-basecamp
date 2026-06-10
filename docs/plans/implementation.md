# Implementation plan — ia-basecamp

Design: [SPEC.md](../../SPEC.md). Phases tracked as GitHub issues (P0–P6) + the hard dependency (#1, LEZ#519).

| Phase | Issue | Gate |
|-------|-------|------|
| P0 scaffold + spike | #2 | channelId→account_id mapping confirmed |
| P1 read channels | #3 | #1 (synced indexer) |
| P2 preserve | #4 | — |
| P3 UI | #5 | — |
| P4 federation | #6 | — |
| P5 package + cross-platform | #7 | — |
| P6 shareable cards | #8 | thumbnail in manifest |

Every issue: build + tests green → Senty review → commit → **auto-retro** (skill + wins/fails). See `prompt.md`.
