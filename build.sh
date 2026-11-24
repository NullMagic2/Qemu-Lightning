#!/usr/bin/env bash
# Module: lightning.build.entry
# Purpose: Start the Rust build tool from any working directory.
# Created: 2026-10-08
# Architecture: Shell only locates the shared bootstrap; Rust owns build policy.
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec "$source_root/tools/lightning/run-tool.sh" build "$@"
