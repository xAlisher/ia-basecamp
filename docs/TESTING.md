# Logos-IA (Archive module) — Testing Guide

How to exercise every use case end-to-end. The campaign model: follow a **trusted
channel** whose curators inscribe lightweight `ia_item` entries (IA identifier +
name + size, no CIDs); **preserving** an entry means download from archive.org →
verify against IA's own checksums → store in Logos Storage. A per-channel **auto**
toggle preserves new (and existing) entries automatically.

## Prerequisites

- A built Basecamp AppImage with the **fixed zone-sequencer module** (the
  fresh-channel publish fix, zone-sequencer-rs#3) and the **archive** module
  installed.
- A reachable Logos node / gateway (default dev node in `lez_client.cpp`).
- Keycard only needed if you want to *inscribe* new entries via Beacon; **following
  and preserving need no Keycard** (read-only channel reads).

## Install (dev)

```bash
./scripts/install-lgx.sh          # builds + installs archive (core) + archive_ui
# kill + relaunch Basecamp to pick them up:
pkill -9 -f '[m]ount_logos'; pkill -9 -f '[l]ogos_host_qt'
~/logos-basecamp-current.AppImage > /tmp/basecamp.log 2>&1 &
```

Logs: `/tmp/basecamp.log` (stdout) and `~/.local/share/Logos/LogosBasecamp/logs/`.
ia state: `~/.local/share/ia-archive/state.json`; local storage repo:
`~/.local/share/ia-archive/storage` (delete to wipe preserved data — Basecamp down).

## Test channel (ready, on-chain)

A clean channel pre-loaded with **10 cypherpunk / privacy / freedom `ia_item`s**
(6.8 KB → 6.4 MB; all verified-resolvable real archive.org items):

```
73313f1470bc9ea16590fd9f63ae7ef7c239d23598ffb642b3dfbc40e8fb79a6@5126000
```

The `@5126000` is the scan-start hint (bounds the scan to ~3k slots). Raw payloads
are in `docs/test-fixtures-cypherpunk.jsonl` (one `ia_item` JSON per line) if you
want to inscribe your own channel via Beacon free-text.

## Use cases

### 1. Module loads
Open the **Archive** module. UI renders with **Channels** and **Items** tabs, a
storage pill, and a settings cogwheel. Log shows `Module loaded: storage_module`
then `Module loaded: archive` (storage must load first — it's a dependency).

### 2. Follow a channel + scan
Paste the test channel string into the follow field. Within seconds:
`Channel 73313f14… fully synced — scanned ~3k slots: N matched`. Items appear in
the Items tab.

### 3. ia_item row label is honest
An un-preserved ia_item row's grey sub-label reads **"from archive.org"** — *not*
"checksum-verified" (verification is an outcome of preserving, surfaced afterward).

### 4. Manual preserve (download → verify → store)
Click **Preserve** on `HackersManifesto` (smallest, ~instant). Watch the log:
`keeper-HackersManifesto-*` files `Stored data` in storage_module. The row badge
goes **preserving → preserved**; a desktop notification fires
`HackersManifesto preserved ✓ — N files, checksum-verified against the Internet
Archive` (or `… N files — M stored without an IA-published checksum` if any file
lacked a checksum). **No `_files.xml` is stored** (the manifest self-entry is
skipped — its checksum can never match its own bytes).

### 5. Auto-preserve — existing items (toggle after sync)
With the channel already synced, flip the channel's **auto** toggle ON. Every
un-preserved item (and any previously-failed ones) should begin preserving
immediately — not just future arrivals. Expect all 10 to preserve in sequence.

### 6. Auto-preserve — new items (≤60s auto-refresh)
With auto ON, inscribe a new `ia_item` into the channel via Beacon (needs Keycard).
Within ≤60s the auto-refresh discovers it and auto-preserves it; a `preserved ✓`
notification fires with no manual click.

### 7. Fresh channel publish (zone-seq#3 regression)
Create a brand-new channel via Beacon (Keycard) and inscribe an `ia_item`. The
first publish must land in seconds (node `GET /channel/{id}` flips 404 → 200) — it
must **not** hang. (Pre-fix it hung indefinitely on the genesis backfill.)

### 8. Failure is loud, no silent retry
If a preserve fails (e.g. transient storage blip, or a checksum mismatch), the item
goes to a **failed** badge, a `Preserve failed: {id}` notification fires, and it
**stops** — no retry queue. Re-running is manual: click Preserve again, or toggle
auto (which re-queues failed items).

### 9. Size guard
An item over the 1 GiB auto-preserve cap is skipped by auto (logged + notified
"over the cap — preserve manually") but can still be preserved manually.

### 10. Settings — Remove all items
Settings (cogwheel) → Danger zone → **Remove all items** (two-click confirm).
Forgets every followed channel and its items (a clean slate). Does **not** unpin
from storage (ia_item unpin is unsupported in v1).

### 11. Activity log + unseen badge
The activity log records follow/publish/preserve/fail events; an unseen-items count
badge updates as new items arrive.

## What's NOT in scope (won't see / deliberately omitted)
- **Share** button (hidden — share cards out of scope).
- **ia_item unpreserve** (per-file unpin bookkeeping doesn't exist in v1; the
  per-item Remove is hidden for cid-less ia rows).
- Official channels are **not** auto-seeded as built-in defaults (see ia#19) — you
  follow channels explicitly.

## Known issues / follow-ups
- ia#19 remove hardcoded official-channel default · ia#20 newest-on-top ordering ·
  ia#23 storage flaps offline (fixed — skip health probe during upload) ·
  beacon#28 inscribe request not reaching the zone-seq module · zone-seq#3 (fixed).
