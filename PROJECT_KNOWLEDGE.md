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
