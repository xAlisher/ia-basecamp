#!/usr/bin/env bash
# Headless backend tests: build + run against the mock gateway (no network, no display
# needed — QTest core only). Uses the flake devShell for Qt6 + cmake.
set -euo pipefail
cd "$(dirname "$0")"

nix develop ../.. --command bash -c '
    set -euo pipefail
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug > /dev/null
    cmake --build build -j"$(nproc)"
    XDG_DATA_HOME=$(mktemp -d) QT_QPA_PLATFORM=offscreen ./build/tst_lez_client
    XDG_DATA_HOME=$(mktemp -d) QT_QPA_PLATFORM=offscreen ./build/tst_storage_client
'
