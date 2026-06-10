# Build prompt — ia-basecamp (autonomous, Fable)

> Paste this to start an autonomous implementation session. It is tuned for three targets, in order:
> **(1) reliability · (2) simplicity of architecture & maintenance · (3) autonomous build-test-retro.**
> When a choice trades cleverness for any of these three, take the simpler/safer path.

---

You are implementing this Logos Basecamp module to completion, autonomously, in the
`xAlisher/ia-basecamp` repo. Work phase by phase; verify everything; ask only if genuinely blocked.

## Thesis (what & why)
Anyone — Mac, Windows, Linux, no node, no hardware — can **follow** permanent curated archive
channels and help **preserve** their collections to decentralized Storage. Curators inscribe
collections into permanent channels on the Logos Execution Zone (a separate, curated, Linux-side job).
This module is the **consumer half**: read the permanent channel, pin the collections. No central
index, no chain node, no ephemerality. It's the public on-ramp for an Internet-Archive preservation
campaign; its growth engine is a shareable "I'm preserving X GB" card.

## Read first (authoritative, in order) — do NOT redesign these
1. `SPEC.md` — the complete design. Source of truth.
2. The GitHub issues — phases **P0→P6** (#2–#8) and the **HARD DEPENDENCY #1** (LEZ#519). Do them in order.
3. `src/archive.rep` — the Qt RemoteObjects contract. The interface is fixed.
4. `~/fieldcraft/protocols/` — HOW you work: builder-auditor loop, verify-before-claiming, halt-resume,
   wins-and-fails, skill extraction, retro-after-merge. Follow them.
5. `~/basecamp/basecamp-skills/skills/_index/` — platform knowledge (QML, IPC, the builder, packaging).
   Read the relevant index sheet BEFORE re-deriving platform behavior; open only the recipe you need.
   Don't reinvent what's already a skill.

## Architecture — non-negotiable (this is exactly why it works)
- **ui_qml module with a C++ backend** (`mkLogosQmlModule`), NOT a `type:core` module. A core module
  that consumes services crashes at load (logos-delivery-module#31); a ui_qml backend doesn't. This is
  the whole reason it runs on the **latest** Basecamp build.
- **Pure `QtNetwork` HTTP/JSON-RPC client.** `dependencies:[]`, `nix.runtime:[]`. No platform-module
  deps, no native libs → builds on macOS/arm, Windows, Linux. **Do NOT add deps or `LogosModules`.**
- The `.rep` is the single source of interface truth: `PROP` = synced state, `SLOT` = QML-callable
  returning a JSON string. QML uses `logos.module("archive")` + `logos.watch`.
- Two thin, separate clients: `lez_client` (read the LEZ indexer) and `storage_client` (preserve).

## Reliability rules (from line 1)
- Every SLOT returns `{"ok":true,...}` or `{"ok":false,"error":"<code>"}`. Never throw across the
  boundary; never return a bare value.
- **Graceful degradation, NEVER silent failure.** The gateway/indexer WILL be down or lagging
  (LEZ#519). Detect it, surface it (`gatewayState`, `syncLagBlocks`), and never present stale data as
  live.
- Defensive HTTP: a timeout on every request, bounded retries, **federation failover** (try the next
  gateway). Read **FINALIZED** data only.
- No `not_implemented` remains in a phase you call done.

## Simplicity rules
- Thin client. No frameworks, no clever abstractions, no inheritance towers. Small focused files,
  read top-to-bottom. JSON strings across the boundary; parse in QML. Boring and obvious wins.

## Repo structure (keep it per fieldcraft conventions)
- `SPEC.md` (design), `README.md` (overview), `PROJECT_KNOWLEDGE.md` (accumulated lessons/patterns).
- `docs/plans/<plan>.md` (approach per epic), `docs/retro-log.md` (raw wins/fails captures),
  `docs/skills/` (module-specific recipes; platform-wide ones go to `~/basecamp/basecamp-skills`).
- `halt.md` at root when pausing (gitignored). `.gitignore` already excludes `result*`, `build/`, `*.lgx`.
- Module code: `flake.nix` + `metadata.json` + `CMakeLists.txt` at root; `src/` (`*.rep`, `*_plugin.*`,
  `lez_client.*`, `storage_client.*`, `qml/`). Tests under `tests/`.
- Keep the tree clean: one concern per file, no stray scratch files committed.

## Operating protocol (fieldcraft — non-optional)
- **Branch first**; never commit to main directly. One branch/commit per concern.
- **Builder-auditor:** after each phase, push the branch and run a Senty review (`/codex:review` for
  QML/docs, `/codex:rescue` for C++/HTTP/security); fix every HIGH/MEDIUM before the next phase.
- **Verify before claiming** — the core rule. "Builds" means you ran `nix build`; "tests pass" means
  you ran them.
- If you pause or near a context limit, write `halt.md` (where you stopped, next steps, branch/commit/
  build status) per halt-resume — so the next session resumes cleanly.

## Autonomous build-test-RETRO loop — run it for EVERY issue
1. Read the phase issue + its SPEC section. Create/confirm the branch.
2. Implement the minimum for that phase.
3. **Write its tests in the same change** (SPEC §13): backend tests against a MOCK HTTP indexer/Storage
   (canned JSON, deterministic, no network); UI via `nix build .#integration-test`. Isolate state with
   `XDG_DATA_HOME=$(mktemp -d)`.
4. `nix build .` MUST compile; **run the tests, they MUST pass.** Fix until green.
5. Push + Senty review; fix HIGH/MEDIUM. Commit (conventional message). Tick the issue's checklist +
   its Tests item.
6. **AUTO-RETRO (do this after every issue's self-test, don't wait):** extract any reusable lesson —
   platform-wide → `~/basecamp/basecamp-skills/` (+ update its `_index/`); module-specific →
   `docs/skills/`. Log a win/fail line to `docs/retro-log.md` (moment · wrong action · root cause).
   Fold durable patterns into `PROJECT_KNOWLEDGE.md`. Commit the retro outputs. THEN move to the next
   issue. (Full `/retro` runs once more after the final epic merge.)

## Already done (start from here)
- **P0 skeleton COMPILES** (`archive_plugin.so` + `archive_replica_factory.so`) with the latest builder
  — the shape is proven. Begin by confirming the load on the latest Basecamp + the P0 spike.

## Gotchas already paid for (building radio) — don't rediscover
- **QRO codegen:** a SLOT named `set<Prop>` collides with the auto-generated PROP setter. Name action
  SLOTs distinctly (we renamed `setPreserveMode`→`choosePreserveMode`).
- **`logos_host` swallows child stderr** — persist diagnostics to a file when something fails opaquely.
- **The open spike that gates P1:** confirm the `channelId → account_id` mapping (for
  `getTransactionsByAccount`) against a synced gateway BEFORE building the read.
- ui_qml backends do network/IO fine; you do NOT need (and must not add) `LogosModules`/delivery here.

## Definition of done (per issue and overall)
Builds clean · tests written and passing · Senty HIGH/MEDIUM cleared · the issue checklist + its Tests
item satisfied · **retro done (skill extracted + wins/fails logged)** · committed · degrades gracefully
when the gateway is down or lagging.

**Start now:** read SPEC.md + the issues + fieldcraft, confirm the P0 load and the `channelId→account_id`
spike, then implement P1. Work issue by issue to completion.
