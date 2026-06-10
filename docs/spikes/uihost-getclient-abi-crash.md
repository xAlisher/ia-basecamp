# ui-host + statically-linked LogosAPI::getClient = bad_alloc (ABI mismatch)

**Date:** 2026-06-10 · **Symptom:** Archive view never opens in Basecamp; ui-host dies.

## The crash

```
ui-host: loaded plugin "archive"
terminate called after throwing an instance of 'std::bad_alloc'
ViewModuleHost: process exited for "archive" with code 6
→ "Timeout waiting for ui-host ready signal" — the user sees nothing.
```

Backtrace (gdb on the AppImage's own ui-host, headless repro):

```
#13 StorageModule::StorageModule(LogosAPI*)        ← our .so (builder-generated SDK)
#12 LogosAPI::getClient(QString, LogosTransportConfig)   ← our .so (STATICALLY linked)
#10 std::basic_string::_M_construct → bad_alloc    ← garbage-size string member read
```

## Root cause

The builder statically links `logos-cpp-sdk` (including `LogosAPI::getClient`) into the
plugin .so. At runtime that code executes against the **LogosAPI object constructed by the
host process** — the AppImage's ui-host, built from a different liblogos revision. Member
offsets differ → a string member reads a garbage length → `bad_alloc` → the host aborts.

- Reproduced on **both** available AppImages (Jun-9 current and 0.1.2): mismatch.
- The nix `logos-standalone-app` ui-host (builder-pinned) works — `getClient` connects,
  `capability_module` grants `requestModule(archive → storage_module)`.
- **logos_host does not have this problem in practice**: stash's plugin makes the same
  statically-linked `getClient` call from logos_host daily and works.
- P0–P6 builds never called `getClient` → never crashed (the breakage appeared exactly
  when P2v2 introduced `new StorageModule(api)` in initLogos).

Headless repro (no clicks needed):

```bash
M=$(mount | grep -i logos | grep fuse | awk '{print $3}' | head -1)
$M/usr/bin/ui-host --name archive --path <plugin.so> --socket repro
```

## Mitigation (shipped)

`initLogos` wraps transport construction in try/catch; on `std::exception` it falls back
to `NullStorageTransport` — the module loads, channels work, the Storage pill reports
offline. Verified on both AppImages: `initLogos done`, remoting up, no crash.

## Consequence for architecture

Typed-SDK consumption from a **ui-host backend is dead on current AppImages**. The working
production pattern is stash's: a **core module** (logos_host) owns the storage talk; the
QML is a view-only plugin using the main-process bridge (`logos.callModule`). This also
affects radio's ui-qml-backend forward path — same landmine. Worth an upstream issue:
either ship `getClient` support guarantees for ui-host LogosAPI objects, or make liblogos
ABI-stable across host types.
