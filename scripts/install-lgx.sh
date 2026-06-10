#!/usr/bin/env bash
# Build + install the archive module into the local Basecamp (dev cycle).
#
# Why the dual-variant merge: the builder's `.#lgx` target produces only the
# `linux-amd64-dev` variant, so the installed manifest's `main` map lacks the
# `linux-amd64` key — and Basecamp resolves the backend .so via
# main["linux-amd64"]. Result: the QML view loads but the C++ backend is never
# spawned (logos.module("archive") returns null). `.#lgx-portable` produces the
# `linux-amd64` variant but current lgpm refuses to install an LGX without a
# dev variant. So: build both, merge into one dual LGX, install that.
set -euo pipefail

cd "$(dirname "$0")/.."

LGPM=${LGPM:-$(find /nix/store -maxdepth 3 -name lgpm -path "*logos-package-manager-cli*" 2>/dev/null | head -1)}
[ -n "$LGPM" ] || { echo "lgpm not found in /nix/store"; exit 1; }
MDIR=~/.local/share/Logos/LogosBasecamp/modules
PDIR=~/.local/share/Logos/LogosBasecamp/plugins

echo "Building both LGX variants..."
nix build .#packages.x86_64-linux.lgx          -o result-lgx
nix build .#packages.x86_64-linux.lgx-portable -o result-lgx-portable

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
tar -xzf "$OLDPWD/result-lgx/logos-archive-module.lgx";          mv manifest.json manifest-dev.json
tar -xzf "$OLDPWD/result-lgx-portable/logos-archive-module.lgx"; mv manifest.json manifest-amd64.json

python3 - <<'EOF'
import json
dev = json.load(open('manifest-dev.json'))
amd = json.load(open('manifest-amd64.json'))
m = amd
m['main'].update(dev['main'])
m['hashes']['variants/linux-amd64-dev'] = dev['hashes']['variants/linux-amd64-dev']
json.dump(m, open('manifest.json', 'w'), indent=2)
EOF
rm manifest-dev.json manifest-amd64.json
# bare paths required — './' prefixes make lgpm reject the variant
tar -czf archive-dual.lgx manifest.json variants

rm -rf "$PDIR/archive"   # lgpm skips existing files; stale QML persists otherwise
"$LGPM" --modules-dir "$MDIR" --ui-plugins-dir "$PDIR" --allow-unsigned \
    install --file "$WORK/archive-dual.lgx"
echo "Installed. main keys: $(python3 -c "import json;print(list(json.load(open('$PDIR/archive/manifest.json'))['main']))")"
echo "Kill + relaunch Basecamp to pick it up (see basecamp-skills module-kill-and-relaunch)."
