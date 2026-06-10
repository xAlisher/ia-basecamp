# Async scan ownership: generation guards, not existence checks

**Context:** `lez_client` pages through `/cryptarchia/blocks` with chained async
`QNetworkReply` callbacks while the user can `unfollowChannel`/`followChannel` at any time.

**The bug class (Senty P1 HIGH/MED):** a callback that re-checks
`m_channels.contains(channelId)` looks safe but isn't — if the user unfollows and re-follows
the *same id* while a reply is in flight, the old scan's callback finds the id present again
and mutates the *new* channel's state (stale cursor, leaked collections from a different
startSlot). Existence is not ownership.

**The pattern:**
1. Each `Channel` carries a `generation` stamped from a monotonic counter at follow time.
2. The scan-in-progress registry maps `channelId → generation` (not a bare set).
3. Every async callback's first line:
   `if (m_scanning.value(channelId, -1) != myGeneration) return;`
4. `unfollowChannel` removes the registry entry — in-flight callbacks self-discard;
   re-follow bumps the generation so even a racing registry re-insert can't be confused.

**Related rule (same phase):** never `emit stateChanged` behind an "only on change" guard
when the *initial* value equals the failure value — our `offline` start state swallowed the
first real offline notification. Emit unconditionally on failure paths; PROP sets are
idempotent.

**Regression test:** `tst_lez_client.cpp::refollow_whileScanInFlight`.
