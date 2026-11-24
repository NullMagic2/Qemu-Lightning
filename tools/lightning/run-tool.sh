#!/usr/bin/env bash
# Module: lightning.tools.bootstrap
# Purpose: Compile and run dependency-free Rust build/cleanup automation.
# Created: 2026-10-08
# Architecture: Holds a shared build/cleanup lock; no graphics logic lives here.
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
binary_root="$(dirname -- "$source_root")/Binaries"

if [[ -L "$binary_root" || -L "$binary_root/.lightning.lock" ]]; then
    echo 'Refusing symlink binary directory or lock.' >&2
    exit 1
fi
mkdir -p -- "$binary_root"
exec 9>"$binary_root/.lightning.lock"
flock -n 9 || { echo 'Another build or cleanup is running.' >&2; exit 1; }
bootstrap=$(mktemp -d "$binary_root/.bootstrap.XXXXXXXX")
trap 'rm -rf -- "$bootstrap"' EXIT
"${RUSTC:-rustc}" --edition=2021 "$source_root/tools/lightning/main.rs" -o "$bootstrap/lightning-build"
"$bootstrap/lightning-build" "$source_root" "$@"
