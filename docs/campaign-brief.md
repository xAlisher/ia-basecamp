# Logos-IA Campaign — Simplification Brief

*2026-06-11 · status: agreed direction, issues to be filed per phase*

## The model

Logos and/or Internet Archive operate **trusted channels**: LEZ channels whose ids are
published by the campaign and pre-listed in this module. Curators inscribe lightweight
entries — **IA identifier, name, size. No CIDs.**

Module owners follow these channels. The module refreshes them automatically. Preserving
an entry means: **download from archive.org, verify against IA's own checksums, store
locally.** Optionally, a per-channel toggle preserves every new entry automatically.
The user is told what happened either way.

This inverts today's flow. Preserve no longer means "re-pin a CID that someone else's
storage hopefully still holds" — IA itself is the content source (it is the durable
party), and the reseed path (IA download + keeper-exact naming) is promoted from
fallback to primary.

## Inscription payload (v2)

```json
{ "v": 1, "type": "ia_item", "id": "<IA identifier>", "name": "<display name>", "size": <bytes> }
```

- **Entry = one IA item.** A curator preserving a whole IA collection inscribes one
  entry per item (curator-side expansion). The module never walks collections.
- `name` and `size` are display/decision fields; `id` is the only load-bearing one.
- Parsing stays permissive (existing convention): unknown fields ignored, missing
  optional fields tolerated.
- Payload discipline: three meaningful fields. Resist additions until a real need.

## Integrity: checksums instead of inscribed CIDs

Dropping CIDs from inscriptions does not drop verification — it relocates it to the
source of truth:

1. For each item, fetch `https://archive.org/download/{id}/{id}_files.xml` — IA's
   authoritative per-file manifest, which carries **md5 and sha1 for every file**.
2. After downloading each file, compute md5/sha1 and compare against the manifest
   entry. Mismatch → the file is discarded, the item is marked `failed`, and the
   failure is surfaced (notification + activity log + skip counter, per the #11 rule:
   nothing silent).
3. Only verified files are uploaded to storage.

Reproducible CIDs survive as an *output*: keeper-exact temp naming
(`keeper-{id}-{file}`) means independently-preserved copies converge on identical
dataset CIDs, so cross-user dedup and on-chain receipts (Beacon) still work — the CID
is derived locally from verified bytes rather than trusted from the channel.

Trust chain, end to end:
**channel id published by campaign → signer is the channel → entry names an IA item →
bytes verified against IA's checksums → CID derived locally.**

## Trusted channels

- Official channel id(s) ship in module defaults, pre-followed on first run,
  auto-preserve **off**.
- A "trusted" channel is nothing more than a channel whose id the campaign publishes —
  signer key == channel id (existing convention). No new trust machinery.

## Automation

- **Auto-refresh**: background timer refreshes each followed channel. Cheap since #10:
  an incremental refresh scans cursor→LIB — typically one page.
- **Auto-preserve**: per-channel boolean. On a new entry: check the size guard
  (configurable cap + available storage space), then run the preserve flow. Guard
  trips are surfaced, not silent. No retry queue — a failed preserve notifies and
  stops; the user can retry manually.

## Notifications (phased honestly)

Basecamp has **no notification system today** (only a designed `LogosBadge`). So:

- **Phase 1 (this module, now)**: desktop notifications via D-Bus
  (`org.freedesktop.Notifications`) from the C++ backend — works on Linux without
  platform changes. Texts:
  - manual mode: *"{name} ({size}) published — preserve?"*
  - auto mode: *"{name} ({size}) preserved ✓"* / *"preserve failed: {reason}"*
  Plus the in-module activity log and an unseen-items count badge.
- **Phase 2 (platform ask, separate logos-app issue)**: clicking a notification opens
  the module on the channel tab. Module activation lives in UIPluginManager; until
  that exists, the notification raises the app at best.

## Rename: collections → items

IA's own vocabulary, and "collections" now actively conflicts with IA collections
(which we deliberately don't model). `getCollections`/`collectionsJson`/UI labels are
module-private API — rename fully in one commit, no compat aliases.

## Test preparation

Channel fixtures are prepared with Beacon's free-text inscribe (beacon-basecamp issue):
paste a JSON `ia_item` entry, inscribe it into a channel you control, follow that
channel here. No curator tooling needed for testing.

## Phases / issue breakdown

1. Rename collections → items (mechanical, lands first).
2. Payload v2 (`ia_item`) + preserve = IA download → checksum verify → store.
3. Official channels pre-listed in defaults.
4. Auto-refresh timer.
5. Auto-preserve toggle + size guard.
6. D-Bus notifications (+ separate logos-app issue for deep-linking).

## Non-goals

- No IA collection expansion in the module (curator-side).
- No retry queues, no scheduling machinery.
- No CIDs in campaign inscriptions (keeper-style `cid_pin` channels keep working —
  payload `type` discriminates).
