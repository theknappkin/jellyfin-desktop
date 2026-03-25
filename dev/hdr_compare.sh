#!/bin/bash
# Compare Wayland color management protocol messages between
# standalone mpv (already captured in mpv.log) and our hdr-test.
# No arguments needed — URL is hardcoded in hdr-test.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

# Build
cmake --build build --target hdr-test 2>&1 | tail -5

# Run our test harness with WAYLAND_DEBUG
echo "Running hdr-test for 5 seconds..."
WAYLAND_DEBUG=1 ./build/hdr-test 2>app_wl.log || true

# Filter color management protocol messages
grep -E 'wp_(color_management|image_description)' mpv.log 2>/dev/null | \
    sed 's/^\[[0-9.]*\] //' > mpv_cm.log || true
grep -E 'wp_(color_management|image_description)' app_wl.log 2>/dev/null | \
    sed 's/^\[[0-9.]*\] //' > app_cm.log || true

echo ""
echo "=== MPV color management messages ($(wc -l < mpv_cm.log) lines) ==="
head -50 mpv_cm.log
echo ""
echo "=== APP color management messages ($(wc -l < app_cm.log) lines) ==="
head -50 app_cm.log
echo ""
echo "=== DIFF ==="
diff -u mpv_cm.log app_cm.log || true
