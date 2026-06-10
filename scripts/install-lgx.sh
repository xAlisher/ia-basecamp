#!/usr/bin/env bash
# Build + install both archive artifacts into the local Basecamp (dev cycle).
#   archive      — core module (logos_host)        → modules/  via .#lgx-portable
#   archive_ui   — view-only QML plugin (no .so)   → plugins/  via its .#lgx
# Stash shape: no dual-variant merge needed anywhere (the core LGX ships
# linux-amd64; the UI plugin has no backend binary for Basecamp to resolve).
set -euo pipefail

cd "$(dirname "$0")/.."

LGPM=${LGPM:-$(find /nix/store -maxdepth 3 -name lgpm -path "*logos-package-manager-cli*" \
    -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)}
[ -n "$LGPM" ] || { echo "lgpm not found in /nix/store"; exit 1; }
MDIR=~/.local/share/Logos/LogosBasecamp/modules
PDIR=~/.local/share/Logos/LogosBasecamp/plugins

echo "Building core LGX (both variants — current lgpm requires a dev variant,"
echo "Basecamp resolves main[linux-amd64]; same merge as lgx-ui-qml-backend-dual-variant)..."
nix build .#packages.x86_64-linux.lgx-portable -o result-lgx-core
nix build .#packages.x86_64-linux.lgx          -o result-lgx-core-dev
echo "Building UI LGX..."
nix build ./plugins/archive_ui#packages.x86_64-linux.lgx-portable -o result-lgx-ui

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
( cd "$WORK"
  tar -xzf "$OLDPWD"/result-lgx-core-dev/*.lgx; mv manifest.json manifest-dev.json
  tar -xzf "$OLDPWD"/result-lgx-core/*.lgx;     mv manifest.json manifest-amd64.json
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
  tar -czf archive-core-dual.lgx manifest.json variants )   # bare paths — lgpm rejects ./

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
    install --file result-lgx-ui/*.lgx

echo "core:  $(ls "$MDIR/archive" 2>/dev/null | tr '\n' ' ')"
echo "ui:    $(ls "$PDIR/archive_ui" 2>/dev/null | tr '\n' ' ')"
echo "Kill + relaunch Basecamp to pick them up."
