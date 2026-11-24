#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Runs a disposable, paused machine and opens the manager on a private X server.
set -euo pipefail
runtime=${1:-/usr/lib/qemu-lightning}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
"$runtime/bin/qemu-img" create -f qcow2 "$work/empty.qcow2" 16M
"$runtime/bin/qemu-system-x86_64" -device help | grep 'name "lightning-pci"' > "$work/device"
printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"query-status"}' '{"execute":"quit"}' |
    timeout 30 "$runtime/bin/qemu-system-x86_64" -L "$runtime/share/qemu" -machine q35,accel=tcg \
       -m 128 -S -display none -nodefaults -qmp stdio > "$work/qmp"
grep -q '"status": "prelaunch"' "$work/qmp"
for f in "$runtime/bin/"* "$runtime/tools/virgl_render_server" "$runtime/lib/lightning-venus/"*; do
    ldd "$f" > "$work/libs"
    if grep 'not found' "$work/libs"; then exit 1; fi
done
mkdir -m 700 "$work/session"
XDG_RUNTIME_DIR="$work/session" HOME="$work" GTK_USE_PORTAL=0 timeout 45 xvfb-run -a \
    "$runtime/bin/qemu-lightning-manager" --binaries "$runtime" --data-dir "$work/machines" --smoke-screenshot "$work/manager.png"
test -s "$work/manager.png"
echo "Emulator, disk tool, Lightning device registration, runtime libraries and GTK manager passed."
