#!/usr/bin/env bash
# Module: lightning.clean.entry
# Purpose: Remove owned build intermediates while retaining published binaries.
# Created: 2026-10-08
# Architecture: The shared Rust tool validates ownership before cleanup.
set -euo pipefail
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec "$source_root/tools/lightning/run-tool.sh" clean "$@"
