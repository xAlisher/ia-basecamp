# ia-basecamp — sovereign archive follower & preservation

> **One line:** follow permanent, curated archive channels (inscribed on the Logos Execution Zone)
> and help preserve their collections to decentralized Storage — from any machine, no node required.

## 1. Purpose & goals

The Internet-Archive preservation campaign needs a public-facing module that **anyone** can run —
Mac, Windows, Linux, low-resource — to participate in keeping curated collections alive. Authoring
(inscribing) stays a curated activity (Logos/IA team, Linux + zone-sequencer + Keycard). This module
is the **consumer + preserver** half:

- **Follow** curated channels — permanent, append-only, verifiable (LEZ inscriptions, not ephemeral Waku).
- **Browse** every collection a channel has ever curated, with on-chain provenance (`txHash`, curator).
- **Preserve** collections to Storage — either *delegate* to a trusted node (zero-infra) or *contribute*
  your own Storage (real redundancy).

**Non-goals (v1):** user-side inscription/authoring (curators do that), running a chain node, hosting.

## 2. Architecture — why `ui_qml` with a C++ backend

A single **`ui_qml` module with a compiled C++ backend** (the `logos-delivery-demo` / radio
`ui-qml-backend` shape). Reasons:

- **No core-module crash.** A `type: core` module that consumes platform services hits
  [delivery-module#31](https://github.com/logos-co/logos-delivery-module/issues/31) at load. A ui_qml
  backend runs in the ui-host where that works. → **runs on the latest Basecamp build.**
- **Pure HTTP client → no platform deps, no native heavy deps → builds on macOS/arm + Windows + Linux.**
  The backend talks to the LEZ indexer (JSON-RPC over HTTP) and Storage (HTTP) directly with `QtNetwork`.
  `dependencies: []`, `nix.runtime: []`. This is the entire reason the module is cross-platform.

```
 CURATOR (Linux, curated)                        FOLLOWER (anyone, this module)
 ┌──────────────────────┐    inscribe       ┌──────────────────────────────────┐
 │ zone-sequencer +     │ ───────────────►  │   LEZ           Storage           │
 │ Keycard              │   (write/prove)   │   indexer  ◄──  (Codex/IPFS)       │
 └──────────────────────┘                   │   JSON-RPC      HTTP              │
            │                                │      ▲             ▲             │
            ▼ permanent on-chain             │  read│        pin/ │mirror       │
   LEZ (Logos Execution Zone)  ───────────►  │  ┌───┴─────────────┴──────────┐  │
   = the channel (append-only)   indexed     │  │  ia-basecamp C++ backend   │  │
                                             │  │  (QtNetwork HTTP client)   │  │
                                             │  └────────────▲───────────────┘  │
                                             │       QRO .rep │  QML view        │
                                             └────────────────┴─────────────────┘
        TRUSTED GATEWAY (federated 2–4):  hosts the indexer RPC + Storage endpoint
```

## 3. Data model

- **Channel** = an LEZ zone / account. Permanent, append-only. Identified by a `channelId`/account.
- **Collection** = one inscription (`ChannelInscribe` op) in the channel: `{ id, title, cid, sizeBytes,
  items, thumbnail, inscribedAt, txHash, curator }`. The `cid` points at the content on Storage;
  `txHash` is the verifiable provenance (explorer link); **`thumbnail`** is a small cover-image
  reference (CID/URL) the curator inscribes in the manifest — used by the share cards (§12).
- **Mirror state** (local, per collection): `available | mirroring | mirrored | error`.

## 4. Read path (the channel is permanent)

The LEZ **indexer** exposes a JSON-RPC read API (`jsonrpsee`); the **explorer** exposes a public web
API over it (keeper already uses `https://testnet.blockchain.logos.co/web/explorer/api/v1/...`).

| Need | Indexer method |
|------|----------------|
| List a channel's full inscription history | `getTransactionsByAccount(channelAccount, offset, limit)` |
| Channel rolled-up state | `getAccount(accountId)` |
| Verify one inscription | `getTransaction(hash)` → explorer link |
| "new collection added" | `subscribe_to_finalized_blocks` (or poll `getLastFinalizedBlockId`) |
| Finality | read **finalized** data only (PoS reorgs above LIB are normal) |

The backend points at a **trusted gateway** (a hosted indexer). Ship a **federated list of 2–4**
gateways and fail over — keeps the zero-friction default without one machine being the single point.

> **⚠ HARD DEPENDENCY:** the read path only works while the gateway's indexer is **synced**. Tracked
> upstream as [logos-execution-zone#519](https://github.com/logos-blockchain/logos-execution-zone/issues/519)
> (indexer stalled behind tip → recent inscriptions 404). The indexer is also **one channel per
> instance** (`config.channel_id`), so the gateway runs one synced indexer per curated channel. See §10.

### 4.1 P0 spike findings (2026-06-10) — confirmed read path

The spike ([docs/spikes/p0-channel-read.md](docs/spikes/p0-channel-read.md)) resolved §10.3: the
table above describes the LEZ **zone-state** indexer, which borsh-decodes inscriptions as zone
blocks and therefore **cannot serve raw-JSON curated channels** (the kind zone-sequencer curators
inscribe); its transactions also carry no `ChannelInscribe` ops to decode. There is **no
channelId→account_id derivation** — upstream's own consumer reference (the
`l2-sequencer-archival-demo` archiver) reads a channel by filtering `Op::ChannelInscribe`
(opcode 17) on `channel_id` over **finalized L1 blocks**. `lez_client` therefore speaks the
gateway node's Cryptarchia HTTP API, confirmed live against a synced node:

| Need | Call |
|------|------|
| Health / finality / lag | `GET /cryptarchia/info` → `{lib_slot, slot, tip, mode}`; `syncLagBlocks = slot − lib_slot`, unreachable ⇒ `offline` |
| Inscription history | `GET /cryptarchia/blocks?slot_from&slot_to` (paged) → `transactions[].mantle_tx.ops[]`, keep `opcode==17 && channel_id==followed`, slots ≤ `lib_slot` only |
| New collections | poll `info` for `lib_slot` advance; scan the delta |

Channel refs accept `channelId` or `channelId@startSlot` (history bootstrap hint). A gateway
entry is `{nodeUrl, storageUrl}` (+ optional basic-auth). The federated-gateway, finalized-only,
and sync-lag-surfacing requirements are unchanged.

## 5. Preserve path

- **Delegate mode (default, zero-infra):** "Preserve" tells the trusted node to pin the collection's
  CIDs. The user contributes *reach*, not storage. Anyone runs it instantly.
- **Contribute mode (opt-in):** point at a local Storage node and pin/replicate to your own disk —
  real redundancy. For people who want to physically hold a piece of the archive.

Both go through the same Storage HTTP API; `mode` is a setting. Progress streamed via events.

## 6. Interface — `src/archive.rep` (Qt RemoteObjects contract)

`PROP` = state auto-synced to the QML replica (READONLY). `SLOT` = method QML calls (async, returns a
JSON string). QML: `logos.module("archive")`, `logos.watch(backend.slot(args), cb)`.

```
class Archive {
    // status (header pills)
    PROP(QString gatewayState="offline" READONLY)   // offline | ready (indexer reachable + synced)
    PROP(QString storageState="offline" READONLY)   // offline | ready
    PROP(QString preserveMode="delegate" READONLY)  // delegate | local
    PROP(QString lastError="" READONLY)
    PROP(QString channelsJson="[]" READONLY)        // [{channelId,name,curator,collections,lastInscription,synced}]
    PROP(QString collectionsJson="[]" READONLY)     // [{id,title,channelId,cid,sizeBytes,items,inscribedAt,txHash,state}]
    PROP(QString summaryJson="{}" READONLY)         // {following,collections,mirrored,usedBytes}

    // config
    SLOT(QString setGateways(QString jsonList))      // [{indexerUrl,storageUrl}] + failover order
    SLOT(QString setPreserveMode(QString mode))      // "delegate" | "local"
    SLOT(QString getStatus())

    // channels (read LEZ)
    SLOT(QString followChannel(QString channelRef))  // accept channelId OR explorer/zone URL
    SLOT(QString unfollowChannel(QString channelId))
    SLOT(QString refreshChannel(QString channelId))  // re-read inscriptions (finalized only)

    // collections + preserve (Storage)
    SLOT(QString getCollections(QString channelId))  // "" = all followed; rows include `thumbnail`
    SLOT(QString mirrorCollection(QString collectionId))
    SLOT(QString unmirrorCollection(QString collectionId))
    SLOT(QString getMirrorStatus(QString collectionId))

    // share cards (§12 — growth)
    SLOT(QString getShareData(QString scope))        // scope="me" | collectionId → {title,totalGB,count,items:[{name,sizeGB,thumbnail}]}
    SLOT(QString saveShareCard(QString pngBase64, QString name))  // QML grabs the rendered card → backend writes PNG → {ok,path}
    SLOT(QString revealCard(QString path))           // open the folder / share intent
}
```

## 7. UI (`src/qml/Main.qml`)

- **Header pills** (radio's pattern): **Gateway** (offline/ready + *sync lag* — surfaces #519 directly),
  **Storage** (offline/ready), **Mode** (Delegate/Local). Cogwheel → settings (gateways, mode).
- **Channels tab:** follow by id/URL, list followed channels (name · curator · N collections · synced?).
- **Collections tab:** the curated list — each row: title · size · provenance link · **Preserve** button
  + progress bar. Header counter: *"you're preserving N collections · X GB"* — the campaign's hook.
- **Activity log** (keycard ActivityLog pattern): timestamped follows / mirrors / errors / sync warnings.
- **Share** button (on the summary counter + per collection) → renders + exports a card to post (§12).

## 8. Module structure

```
ia-basecamp/                       (repo; module name: "archive")
├── flake.nix            mkLogosQmlModule
├── metadata.json        type:ui_qml, main:archive_plugin, view:qml/Main.qml, deps:[], runtime:[]
├── CMakeLists.txt       logos_module(NAME archive REP_FILE src/archive.rep SOURCES …)
├── icons/archive.png
├── SPEC.md  README.md  PROJECT_KNOWLEDGE.md
└── src/
    ├── archive.rep                  the QRO contract (§6)
    ├── archive_interface.h          Q_DECLARE_INTERFACE
    ├── archive_plugin.{h,cpp}       initLogos → setBackend(this); the HTTP/JSON-RPC clients
    ├── lez_client.{h,cpp}           LEZ indexer JSON-RPC reads (getTransactionsByAccount, …) + decode InscriptionOp
    ├── storage_client.{h,cpp}       Storage pin/replicate (delegate|local)
    └── qml/Main.qml
```

## 9. Dependencies

- **Build/runtime:** `QtNetwork` only (HTTP/JSON-RPC). No platform-module deps, no mediamtx/tor/keycard.
- **External services (configured, not bundled):** an LEZ indexer RPC + a Storage endpoint, behind a
  **trusted gateway** (federated). Defaults ship pointing at the campaign/Logos testnet gateways.

## 10. Hard dependency & risks (track before committing a campaign timeline)

1. **[LEZ#519](https://github.com/logos-blockchain/logos-execution-zone/issues/519) — indexer sync.**
   Reads are only trustworthy while the gateway's indexer is caught up. This is THE gating dependency.
   Tracked for this module in its own issue (see repo). The campaign's gateway operator owns the SLA.
2. **Indexer is one-channel-per-instance** (`config.channel_id`) → the gateway runs one synced indexer
   per curated channel. Fine for a finite curated set; an ops cost, not a code change here.
3. **`channelId → account_id` mapping** for `getTransactionsByAccount` — confirm the query key against
   the zone-sequencer + indexer (Phase 1 spike). Mechanism exists; exact key derivation TBD.
4. **Testnet today** (`testnet.blockchain.logos.co`); read **finalized** data only; point at mainnet later.
5. **Centralization tension:** delegate-mode preservation runs on the gateway, not the user. Federation
   mitigates the read side; the "contribute your own Storage" mode is what gives the decentralization
   claim teeth. Keep both.

## 11. Phases (→ GitHub issues)

- **P0 — Scaffold + spike:** ui_qml-with-backend skeleton builds on the latest Basecamp; confirm
  `channelId → account_id` + the indexer/explorer read against a live channel.
- **P1 — Read channels:** `lez_client` (followChannel/getCollections via `getTransactionsByAccount`,
  decode `ChannelInscribe`), channels/collections PROPs, finality handling.
- **P2 — Preserve:** `storage_client` (delegate + local), mirror progress events, summary counter.
- **P3 — UI:** pills (with sync-lag), settings cogwheel, Channels + Collections tabs, activity log.
- **P4 — Federation + resilience:** gateway list + failover; surface #519 sync-lag to the user.
- **P5 — Package + cross-platform:** LGX for linux-amd64 **and** arm64 + darwin-arm64; catalog entry.
- **P6 — Shareable cards (growth):** the `ShareCard` render-to-image + `getShareData`/`saveShareCard`
  /`revealCard` + the Share button (§12).

## 12. Shareable preservation cards (growth hook)

The campaign spreads when contributors *show off*. Every preserver can export a polished image card —
**"I'm preserving 4.2 GB of the Internet Archive"** with collection names + cover thumbnails, sized for
X/Twitter, Mastodon, etc. Each share is an ad + a recruit. This is the module's organic-growth engine.

**Two card types:**
- **Contribution card** (`scope="me"`): the user's total — *N collections · X GB preserved* — a montage
  of featured collections' thumbnails + campaign branding + a join link/QR.
- **Collection card** (`scope=<collectionId>`): one collection — title, size, cover thumbnail, *"preserved
  & verifiable on-chain"* + the provenance link.

**Rendering (QML-side, zero deps):** a hidden, beautifully-styled `ShareCard` Item driven by
`getShareData(scope)` (names, sizes, `thumbnail` refs, totals). Thumbnails load via the Storage/gateway
(the inscribed `thumbnail` CID) with an IA fallback. The **Share** button → `grabToImage` →
`saveShareCard(pngBase64, name)` (backend writes the PNG to a known folder) → `revealCard(path)` opens
it / fires a share intent. No native image libs — pure QML render-to-image, so it stays cross-platform.

**Design:** 1200×675 (X ratio); dark theme + campaign logo; thumbnail grid (1–6 covers, graceful with
missing thumbnails → tasteful placeholder); a big **"X GB"** number; a short verifiable line ("preserved
on the Logos Execution Zone"). Screenshot-clean, no PII — the user is "anonymous preserver" or an
optional handle.

**Curator requirement:** the inscription manifest should carry a `thumbnail` (small cover-image CID) per
collection — that's what makes the share pop. Absent → a placeholder.

## 13. Testing (every phase ships tests — no issue closes without them)

Per fieldcraft (verify before claiming). Two harnesses, mirroring radio:

- **Backend tests (headless):** the backend is a pure HTTP/JSON-RPC client, so tests run it against a
  **local mock** indexer + Storage (a tiny `QHttpServer`/fixture serving canned JSON-RPC + CIDs). No
  testnet, no network, deterministic. Covers: LEZ decode (`getTransactionsByAccount` → `ChannelInscribe`
  → collection records), finality filtering, follow/refresh state, sync-lag computation, mirror flow
  (delegate vs local), and `getShareData`/`saveShareCard` (PNG actually written).
- **UI integration test:** `nix build .#integration-test` (logos-standalone-app) — QML loads,
  `logos.module("archive")` resolves the replica, pills/tabs/ShareCard instantiate.

**Rule:** each phase issue (P0–P6) ships its tests in the same PR and lists them in its checklist;
"Verified: build Y/N, tests Y/N" goes in the handoff. A mock-fixture lib lands in P1 and grows per phase.
