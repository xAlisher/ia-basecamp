# Retro Log — ia-basecamp

Raw wins/fails captured after each issue (auto-retro), reshuffled into PROJECT_KNOWLEDGE.md / skills at `/retro`.
Format per fieldcraft wins-and-fails: each fail names the **moment**, the **wrong action**, the **root cause**.

## 2026-06-10 — P0 (issue #2)

**Fail** · verifying the Basecamp load · drove GUI clicks via ydotool to open the module view ·
clicks landed in an unrelated window (Proton Mail) because GNOME pointer acceleration distorts
relative moves; ~20 min lost and real misclick risk. Root cause: reached for GUI automation
before asking "what's the headless equivalent?" — the standalone harness + log grep already
proved every load stage. Headless-first is now the standing rule for verification.

**Fail** · first runs of the load smoke test were red on a working module · the script grepped
for qDebug lines that never reach a redirected log without `QT_FORCE_STDERR_LOGGING=1` /
`QT_ASSUME_STDERR_HAS_CONSOLE=1`. Root cause: assumed qDebug output behaves like stdout; a
prior manual run had only "worked" because those env vars happened to be set in that shell.

**Fail** · Basecamp rendered the view but `logos.module("archive")` was null with zero errors ·
installed the `.#lgx` (dev-only) package; Basecamp resolves the backend via
`main["linux-amd64"]`, which that manifest lacks. Root cause: the silent-failure pair
(view loads / backend skipped) is invisible in every log; only a manifest diff against radio's
known-good install exposed it. → skill `lgx-ui-qml-backend-dual-variant`, fix
`scripts/install-lgx.sh`.

**Win** · spike answered without a live gateway (explorer fully 502): combined the local LEZ
repo clone (indexer borsh internals), a sparse clone of upstream's `l2-sequencer-archival-demo`
(the reference consumer), and a live read against the synced sneg node — decoded a real keeper
inscription end-to-end (opcode 17). The "indexer JSON-RPC" assumption in SPEC §4 was disproven
at the type level before any client code was written.

**Win** · the standalone `nix run .` harness is a complete P0 verifier for ui_qml-with-backend
modules: plugin load, initLogos, QRO remoting, QML replica sync — all greppable. Now codified
as `tests/integration/standalone_load_test.sh`.

## 2026-06-10 — P1 (issue #3)

**Fail** · first test run of the failure-path tests · guarded `healthChanged` emission behind
"only when state changes" while the initial state was already `offline` — the first real
failure never notified anyone. Root cause: change-guards assume the initial value is a
non-signal value; on failure paths emit unconditionally (PROP sets are idempotent).
→ docs/skills/async-scan-generation-guard.md (related rule).

**Fail** · `setGateways` auto-polled health as a UX nicety · made every test's request flow
nondeterministic (two in-flight polls double-rotated failover). Root cause: side-effectful
config setters; the poll belongs to the caller (the plugin SLOT polls explicitly).

**Win** · Senty round 1 on real C++ caught a genuine async-ownership bug (in-flight scan
adopting a re-followed channel of the same id) that all 19 green tests missed — adversarial
review pays exactly where async lifetimes meet user-driven state mutation. Pattern + regression
test extracted → docs/skills/async-scan-generation-guard.md.

**Win** · test pyramid for an HTTP backend module: 19 deterministic QTest cases against a
60-line QTcpServer mock (33ms, no display, no network) + one env-gated live test
(`ARCHIVE_LIVE_NODE=…`) that ran the production client against the real chain and decoded the
spike's exact on-chain artifact. Deterministic for CI, live for runtime proof — both green.

## 2026-06-10 — P2 (issue #4)

**Fail** · live pin test failed instantly while all 10 mock tests were green · Kubo's CSRF
guard 403s Qt's default `Mozilla/5.0` User-Agent; the mock encoded my assumptions, not the
server's policy. Root cause: external-API behavior verified only against a self-written mock.
Rule: fidelity-check the mock against the real service once, and keep one env-gated live test
per external API. → basecamp-skill `kubo-rpc-qt-user-agent`.

**Fail** · first pin-stream parser ate the success line · the readyRead drain consumed
`{"Pins":...}` looking only for Progress, so `finished` never saw it — mock test caught it.
Root cause: two handlers splitting one stream without shared outcome state; fixed with a
single consumeLine closure shared by both.

**Win** · verified the real Kubo response shapes with a 5-minute throwaway daemon
(`ipfs init --profile test` + fixed API port + curl) before trusting the mock — that session
also produced the CID used by the live test.

**Win** · Senty caught the cid→collection single-slot ownership races (two collections sharing
a cid; unmirror racing a mirror) — the same async-ownership class as P1's finding. The
storage_busy one-op-per-cid guard is simpler than any bookkeeping that tries to allow the race.
