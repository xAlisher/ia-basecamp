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
