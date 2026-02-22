#!/usr/bin/env bash
set -euo pipefail

# Compatibility wrapper (use ./scripts/setup.sh going forward).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/setup/ubuntu.sh" "$@"
