#!/usr/bin/env bash
# P0 integration smoke: the module loads in the logos-standalone-app harness.
# Verifies: plugin .so loads in ui-host, initLogos runs, QRO remoting is enabled.
# Needs a display (Wayland/X11) — the harness opens a window. Run from the repo root.
set -euo pipefail

TIMEOUT=${TIMEOUT:-90}   # seconds to wait AFTER the harness starts (build time excluded)
LOG=$(mktemp /tmp/archive-load-test.XXXX.log)

NEEDLES=(
    'loaded plugin \"archive\"'   # ui-host re-quotes its child output
    'ArchivePlugin: initLogos done'
    'remoting enabled on'
    'process ready for "archive"'
)

echo "Building harness (untimed)..."
nix build --no-link .

echo "Running standalone harness (log: $LOG)..."
# Without these, qDebug output (ViewModuleHost/ui-host lines) never reaches the log.
export QT_ASSUME_STDERR_HAS_CONSOLE=1 QT_FORCE_STDERR_LOGGING=1
# Own process group so cleanup kills the harness + its ui-host children, nothing else.
setsid nix run . > "$LOG" 2>&1 &
APP_PID=$!
cleanup() {
    kill -9 -- "-$APP_PID" 2>/dev/null || true
}
trap cleanup EXIT

all_found() {
    for needle in "${NEEDLES[@]}"; do
        grep -qF "$needle" "$LOG" || return 1
    done
}

waited=0
until all_found; do
    if [ "$waited" -ge "$TIMEOUT" ]; then break; fi
    sleep 5; waited=$((waited + 5))
done

fail=0
for needle in "${NEEDLES[@]}"; do
    if grep -qF "$needle" "$LOG"; then
        echo "PASS: $needle"
    else
        echo "FAIL: missing '$needle'"
        fail=1
    fi
done

if grep -iE 'qml.*error|module.*not.*found' "$LOG"; then
    echo "FAIL: QML/load errors present"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "P0 load smoke: ALL GREEN"
    rm -f "$LOG"
else
    cp "$LOG" /tmp/archive-load-test-failed.log
    echo "P0 load smoke: FAILED — see /tmp/archive-load-test-failed.log"
    exit 1
fi
