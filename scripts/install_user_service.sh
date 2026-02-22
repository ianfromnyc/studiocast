#!/usr/bin/env bash
set -euo pipefail

# Compatibility wrapper.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${SCRIPT_DIR}/install/user_service.sh" "$@"
