#!/usr/bin/env bash
# Build + install both archive artifacts into the local Basecamp (dev cycle).
#   archive      — core module (logos_host)      → modules/
#   archive_ui   — view-only QML plugin (no .so) → plugins/
# BOTH need a portable(linux-amd64) + dev(linux-amd64-dev) variant merge: the
# current lgpm REJECTS a package lacking the -dev variant ("no variant matching
# this platform: tried linux-amd64-dev"), while Basecamp resolves linux-amd64 at
# runtime. So build both variants and merge their manifests into one package that
# carries both. (Earlier this script skipped the merge for the UI — lgpm changed
# to require -dev for view-only plugins too. See skill lgx-ui-qml-backend-dual-variant.)
set -euo pipefail

cd "$(dirname "$0")/.."

LGPM=${LGPM:-$(find /nix/store -maxdepth 3 -name lgpm -path "*logos-package-manager-cli*" \
    -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)}
[ -n "$LGPM" ] || { echo "lgpm not found in /nix/store"; exit 1; }
MDIR=~/.local/share/Logos/LogosBasecamp/modules
PDIR=~/.local/share/Logos/LogosBasecamp/plugins

# merge_dual <portable-lgx-dir> <dev-lgx-dir> <out-abs.lgx>
# Combine a linux-amd64 (portable) + linux-amd64-dev (dev) .lgx into one package
# carrying both variants — lgpm needs -dev to install, Basecamp uses linux-amd64.
merge_dual() {
    local portable_dir dev_dir out w
    portable_dir=$(readlink -f "$1"); dev_dir=$(readlink -f "$2"); out="$3"
    w=$(mktemp -d)
    ( cd "$w"
      tar -xzf "$dev_dir"/*.lgx;      mv manifest.json manifest-dev.json
      tar -xzf "$portable_dir"/*.lgx; mv manifest.json manifest-amd64.json
      python3 - <<'PE'
import json
dev = json.load(open('manifest-dev.json'))
amd = json.load(open('manifest-amd64.json'))
m = amd
m['main'].update(dev['main'])
m['hashes']['variants/linux-amd64-dev'] = dev['hashes']['variants/linux-amd64-dev']
json.dump(m, open('manifest.json', 'w'), indent=2)
PE
      rm manifest-dev.json manifest-amd64.json
      tar -czf "$out" manifest.json variants )   # bare paths — lgpm rejects ./
    rm -rf "$w"
}

echo "Building core LGX (portable + dev variants)..."
nix build .#packages.x86_64-linux.lgx-portable -o result-lgx-core
nix build .#packages.x86_64-linux.lgx          -o result-lgx-core-dev
echo "Building UI LGX (portable + dev variants)..."
nix build ./plugins/archive_ui#packages.x86_64-linux.lgx-portable -o result-lgx-ui
nix build ./plugins/archive_ui#packages.x86_64-linux.lgx          -o result-lgx-ui-dev

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
merge_dual result-lgx-core result-lgx-core-dev "$WORK/archive-core-dual.lgx"
merge_dual result-lgx-ui   result-lgx-ui-dev   "$WORK/archive_ui-dual.lgx"

rm -rf "$MDIR/archive"     # lgpm skips existing files; stale state persists otherwise
"$LGPM" --modules-dir "$MDIR" --ui-plugins-dir "$PDIR" --allow-unsigned \
    install --file "$WORK/archive-core-dual.lgx"
# stash convention: repo metadata has deps:[] (no codegen); the INSTALLED manifest
# declares the runtime dependency so Basecamp orders storage_module first
python3 - <<'PE'
import json, os
p = os.path.expanduser("~/.local/share/Logos/LogosBasecamp/modules/archive/manifest.json")
m = json.load(open(p)); m["dependencies"] = ["storage_module"]
json.dump(m, open(p, "w"), indent=2)
PE

rm -rf "$PDIR/archive_ui"
"$LGPM" --modules-dir "$MDIR" --ui-plugins-dir "$PDIR" --allow-unsigned \
    install --file "$WORK/archive_ui-dual.lgx"

echo "core:  $(ls "$MDIR/archive" 2>/dev/null | tr '\n' ' ')"
echo "ui:    $(ls "$PDIR/archive_ui" 2>/dev/null | tr '\n' ' ')"
echo "Kill + relaunch Basecamp to pick them up."
