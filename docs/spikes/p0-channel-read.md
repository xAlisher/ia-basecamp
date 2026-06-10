# P0 spike — channelId → account_id & the live channel read

**Date:** 2026-06-10 · **Issue:** #2 · **Verdict: read path confirmed live; the
`getTransactionsByAccount` route does NOT apply to curated channels.**

## What was confirmed live

Against the synced sneg node (`/cryptarchia/info`: `lib_slot 5,008,404`, `slot 5,008,956`,
mode Online — tip−lib ≈ 550 slots, the normal in-flight window):

```
GET /cryptarchia/blocks?slot_from=4709374&slot_to=4709380
→ block df976320… (slot 4709374) → transactions[].mantle_tx.ops[]
→ op: { "opcode": 17, "payload": { "channel_id": "8edab686…", "inscription": [123,34,…], "parent": "…", "signer": "…" } }
→ bytes(inscription).decode() == {"v":1,"type":"cid_pin","cid":"ia:kuMUquaeE6g","source":"keeper","ts":1780781543}
```

- **`ChannelInscribe` = opcode 17.** Payload fields: `channel_id` (64-hex), `inscription`
  (JSON byte array for curated channels), `parent`, `signer`.
- The same block also carries inscriptions for channel `8101…01` whose payload is **borsh
  binary** (an L2 zone posting its blocks) — both channel species coexist on L1.

## The mapping question — answered

**There is no `channelId → account_id` derivation, because the account-indexed read does not
apply to curated channels:**

1. The LEZ indexer (`jsonrpsee`: `getTransactionsByAccount`…) borsh-deserializes every
   inscription as a zone `Block` (`indexer/core/src/lib.rs::parse_block_owned`) and serves
   **zone-state** (NSSA accounts/txs). A curated channel's raw-JSON inscriptions fail that
   deserialization — such channels are *invisible* to it.
2. The indexer's `Transaction` type (Public/PrivacyPreserving/ProgramDeployment) carries no
   Mantle ops — so "getTransactionsByAccount → decode ChannelInscribe" (SPEC §4) does not
   type-check against the real protocol.
3. Upstream's own consumer reference (`logos-blockchain/testnet/l2-sequencer-archival-demo`
   archiver) reads a channel by **filtering `Op::ChannelInscribe` on `channel_id` from
   finalized L1 blocks** — no account anywhere. Its gateway API is `GET /blocks` +
   `/block_stream` (SSE).
4. Empirically `channel_id == signer` for the keeper-style channel (curator-key-as-channel
   convention) but NOT for the zone channel — identity is a convention, not an invariant.
   If an account-indexed gateway ever fronts a channel, the account id must be **carried in
   the channel descriptor**, not derived.

## Read path for P1 (what the spike validates)

`lez_client` reads the **gateway node's Cryptarchia HTTP API** — the only protocol with a
live, synced deployment today:

| Need | Call |
|------|------|
| Health + finality + lag | `GET /cryptarchia/info` → `{lib_slot, slot, tip, mode}`; `syncLagBlocks = slot − lib_slot` (+ unreachable ⇒ offline) |
| Channel history / refresh | `GET /cryptarchia/blocks?slot_from=…&slot_to=…` (paged), filter `opcode == 17 && channel_id == followed`, **only slots ≤ lib_slot** |
| New collections | poll `info` for `lib_slot` advance, scan the delta |

Channel refs accept `channelId` or `channelId@startSlot` (bootstrap hint — scanning from
slot 0 over a public HTTP API is not viable; curated-channel listings should publish the
inscription start slot).

## Gateway reality, 2026-06-10

`https://testnet.blockchain.logos.co/web/explorer/api/v1/*` → **502 on every endpoint**
(full outage; strictly worse than the #519 stall — that issue's lag table at least returned
200s for old blocks). Logged on issue #1. The public testnet nodes
(`devnet.blockchain.logos.co/node/N/`) require basic-auth credentials — the campaign
gateway operator owns provisioning these. Sneg (Tailscale `100.108.127.3:8080`) is the
only open synced node available to this dev setup; it is the default dev gateway.
