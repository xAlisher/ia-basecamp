---
id: ia-files-xml-summation-self-entry
title: Skip the <summation> self-entry in archive.org files.xml — its md5 can never match
phase: integration
type: pitfall
severity: high
severity_reason: Preserve fails on EVERY real IA item; the campaign's core promise is broken against archive.org.
module: ia-basecamp
source: extracted-local
last_used: "2026-06-12"
created: "2026-06-12"
status: active
---

## Problem

archive.org's `{id}_files.xml` (the per-file md5/sha1 manifest) lists **itself** as a
`<file>` entry, with an md5 tagged `<summation>md5</summation>`:

```xml
<file name="photo-metro-august-1991_files.xml" source="original">
  <format>Metadata</format>
  <md5>dbfa71cf81df146233fc9e6e8194490f</md5>
  <summation>md5</summation>
</file>
```

That md5 is a summation OVER the manifest — it can never equal the md5 of the served
bytes (the `<md5>` line is part of those bytes). If the parser feeds this entry to the
verify step, `verifyIaFile` returns `Mismatch` → the whole preserve fails. Every real IA
item has this self-entry, so preserve breaks for all of them. Deterministic tests missed
it because synthetic manifests used the no-checksum→Unverified path, not the real
self-md5 + `<summation>` shape.

## Recipe

In `parseIaFilesXml`, drop any `<file>` carrying a `<summation>` child — it's the
manifest's own checksum, not content to verify/store:

```cpp
} else if (inFile && r.name() == QLatin1String("summation")) {
    isSummation = true;   // skip on EndElement: if (!cur.name.isEmpty() && !isSummation)
}
```

`<summation>` is IA's explicit marker and appears on exactly the self-entry. files.xml is
already fetched as the manifest, so there's nothing to store/verify for it anyway.

## Why

Verified live: recorded `dbfa71cf…` vs actual served md5 `ad94ae77…` — structurally
guaranteed mismatch. Regression test: a `<summation>`-tagged entry must not reach the
parsed file list (`tst_ia_files::parse_dropsSummationSelfEntry`).
