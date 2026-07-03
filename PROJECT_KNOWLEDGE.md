# PROJECT_KNOWLEDGE — ia-basecamp

See SPEC.md for the design. Key facts:

- **Shape:** `ui_qml` module with a C++ backend (radio `ui-qml-backend` / logos-delivery-demo). Pure
  `QtNetwork` HTTP/JSON-RPC client — `dependencies: []`, `nix.runtime: []`. This is what makes it
  cross-platform and dodges delivery#31.
- **Contract:** `src/archive.rep` (QRO). PROP = synced state, SLOT = QML-callable (returns JSON).
- **Read path (P0 spike, SPEC §4.1):** gateway node Cryptarchia HTTP API — `/cryptarchia/info`
  for health/lag, `/cryptarchia/blocks?slot_from&slot_to` → filter `ops[]` for `opcode==17`
  (`ChannelInscribe`) + matching `channel_id`, decode `inscription` bytes as JSON. Finalized only
  (slot ≤ `lib_slot`). No channelId→account mapping exists; the jsonrpsee indexer serves
  zone-state and can't see raw-JSON curated channels. Gated on LEZ#519 (gateway sync/uptime).
- **LGX install gotcha:** Basecamp resolves the backend .so via manifest `main["linux-amd64"]`,
  but the builder's `.#lgx` target ships only `linux-amd64-dev` → view loads, backend never
  spawns, `logos.module("archive")` is null. Use `scripts/install-lgx.sh` (dual-variant merge).
- **P0 codegen names (confirmed):** `ArchiveSimpleSource`, `ArchiveViewPluginBase`,
  `rep_archive_source.h`, `setBackend(this)`; SLOT renamed `choosePreserveMode` (a `set<Prop>`
  SLOT collides with the QRO PROP setter).

## Campaign / preserve (v0.2.x — 2026-06-12)

- **files.xml self-entry:** archive.org's `{id}_files.xml` lists ITSELF with an md5 tagged
  `<summation>` that can never match the served bytes → must be skipped in `parseIaFilesXml`,
  else `verifyIaFile` Mismatches and preserve fails for every real IA item. See
  `docs/skills/ia-files-xml-summation-self-entry.md`. Regression: `tst_ia_files::parse_dropsSummationSelfEntry`.
- **Preserve trust chain:** download each IA file → verify md5 (sha1 fallback) against files.xml →
  store. No checksum published for a file → stored-but-counted-unverified (deliberate). Mismatch →
  discard + fail loudly. The notification/label must not over-claim "checksum-verified" when
  `unverified > 0`.
- **Storage upload timeout:** the node stalls 10-22s under load (doesn't crash). The SDK
  `invokeRemoteMethod` default `Timeout` is 20s; the generated sync `uploadUrl` must override it
  (`Timeout(120000)`) or uploads landing in a stall fail with `storage_upload_failed`. Editing
  generated code — re-apply on regeneration. (basecamp-skills: `storage-uploadurl-ipc-timeout`.)
- **Auto-preserve on toggle:** flipping `auto` ON must enqueue already-known `available`/`error`
  items immediately (in `setAutoPreserve`), not only items discovered after — auto-preserve
  otherwise fires solely from `itemsDiscovered`.
- **Icon:** the module icon (`icons/archive.png`, both core + ui plugin) must be the keeper icon
  (`keeper-ui/keeper.png`); it was byte-identical to `radio_ui/icons/radio.png` through v0.2.0.
- **Deps for a working install:** archive_ui → archive → storage_module. storage_module is NOT
  reliably bundled-and-resolvable for the dep — it must be in the profile's `modules/` (copy it in
  alongside archive when hand-installing without lgpm).
- **Parallel-instance gotcha:** `LOGOS_DATA_DIR` did NOT separate profiles on the current AppImage
  (it used the default profile) — see the suspect `logos-data-dir-multi-instance` recipe. To run
  "current + archive" alongside an existing install, the module had to go in the default profile.

## UI redesign + unpreserve (v0.3.0 — 2026-06-12, #26–#36)

- **Remove = unpreserve (#35):** ia_item preserve uploads one CID per file; those CIDs were being
  discarded, so unpreserve had nothing to unpin (the old `unmirrorItem` bailed on ia_item rows as
  a "deliberate v1 gap"). Fix: accumulate each upload's CID in `m_iaStoredCids`, persist on the
  item (`Item.storedCids`, in state.json) at completion. `unmirrorItem` now unpins every stored
  CID (`storage.remove` → frees disk), flipping to `available` only after the last file via an
  `m_unpinRemaining[itemId]` counter. The lesson: a scope cut ("doesn't exist yet") was cheap to
  close because we were already computing the data we threw away — check the close cost before
  enshrining a gap.
- **Idempotent re-upload:** keeper-exact naming makes a fresh preserve after abort/pending produce
  the SAME CIDs (storage dedupes), so `m_iaStoredCids.clear()` at the start of every run is safe —
  partial leftovers from an aborted run aren't orphaned.
- **Sync-unpin caveat:** `m_storage->remove()` is a blocking IPC and `unmirrorItem` loops it
  synchronously over every file — fine for the small campaign fixtures, but a large multi-file
  item would block logos_host for the duration. Revisit (async drain) if items get big.
- **Cross-platform release set:** a release ships 4 lgx — core+ui × linux+mac. **Linux core MUST be
  the dual-variant merge** (portable + dev manifests; lgpm needs the dev variant — see
  `scripts/install-lgx.sh` for the merge). **Mac core is a single portable `darwin-arm64`** (no dev
  merge). This module is pure-Qt (no boost/ssl), so its darwin portable variant carries just the
  dylib + png. Build mac artefacts over ssh `mac`: `git reset --hard origin/main` then
  `nix build .#lgx-portable` for core and `./plugins/archive_ui#lgx-portable` for ui; scp back
  renamed `archive-{core,_ui}-darwin-arm64.lgx`. Pipeline it (background ssh) against the local
  linux build — no idle wait.
- **QML dim-while-busy:** the Preserve pill's `opacity` is both breathing (SequentialAnimation
  value source) and dim-when-offline (#36). Express the dim as a declarative binding; the animation
  overrides while running and the binding restores on stop — never imperatively reset opacity in
  `onXxxChanged`. See basecamp-skill `qml-animation-value-source-vs-binding`.

## Design-system adoption (v0.2.0 — 2026-07-04, #40)

`archive_ui` fully adopts `logos-design-system`, benchmarked against the **delivery-demo** reference
module, at all three layers: **palette** (`Theme.palette.*`, zero hex) → **components**
(`Logos.Controls`) → **typography** (`LogosText` + `Theme.typography.*`, publicSans). Net −42 lines.

- **Verifying a QML change:** the build is NOT a gate. Repo-root `nix build .#lgx` builds the C++
  **core**; `plugins/archive_ui#lgx` only **copies** the QML. The real gates are
  `qmllint -I ~/basecamp/refs/logos-design-system/src/qml` + a render
  (`qml -I <ds> harness.qml` on GL). See basecamp-skill `nix-build-doesnt-validate-viewonly-qml`.
- **Settings gear = inline `data:` URI.** `LogosIconButton` needs an icon URL and `Logos.Icons`
  ships no gear; a loose `qml/icons/*.svg` is **dropped by nix-bundle-lgx** (ships only Main.qml) →
  blank cog in the host. The gear SVG is base64-inlined in Main.qml. The standalone render loads
  from source so it hides this — always `tar tzf *.lgx | grep svg`. → `lgx-bundles-only-view-and-metadata-icon`.
- **Neutral primary buttons.** The design system has no accent-filled button, so `Follow`/`Apply`/
  `Remove all` are neutral `LogosButton`s (delivery-demo's "Call" is neutral too). Orange is reserved
  for the **Preserve state pill** (the CTA) and item-state colours.
- **Custom-but-on-token state widget.** The item state pill (Preserve/Preserved/%/Error/Pending)
  stays a custom fixed-footprint `Rectangle` — it morphs across states with semantic colours no
  component expresses — but uses `Theme.spacing.radiusXlarge` so it reads as the LogosButton family.
- **Status → `LogosBadge`**, sized the settings cog to `gwBadge.implicitHeight` so header chips match.
- **Row ✕ = circular chip** mirroring delivery-demo's InfoChip (22×22 `radius:11`,
  `backgroundElevated`+`borderHairline`, glyph centred), `textMuted` → `errorRed` on hover.
- **Header:** title + a descriptive subtitle on its **own line** (inline overflowed 560px and clipped
  the cog).
- **Version tracks the platform:** realigned 0.3.0 → **0.2.0** to match Basecamp v0.2.0 (the .md
  retro/knowledge history keeps its v0.3.0 references — those record past work).
