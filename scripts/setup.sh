#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
StudioCast setup entrypoint

Usage:
  ./scripts/setup.sh [args...]

This script dispatches to the appropriate distro-family helper.

Currently supported:
  - Ubuntu-family (Ubuntu, Linux Mint, Pop!_OS, etc.)

Examples:
  ./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
  ./scripts/setup.sh --build
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

distro_family=""
if [[ -f /etc/os-release ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release

  id="${ID:-}"
  like="${ID_LIKE:-}"
  if [[ "$id" == "ubuntu" || "$id" == "linuxmint" || "$like" == *ubuntu* ]]; then
    distro_family="ubuntu"
  fi
fi

if [[ -z "$distro_family" ]]; then
  echo "[setup] ERROR: Unsupported distro for helper script." >&2
  echo "[setup] This repo helper currently supports Ubuntu-family distributions only." >&2
  echo "[setup] You can still run distro-specific scripts directly under: scripts/setup/" >&2
  exit 2
fi

exec "${SCRIPT_DIR}/setup/ubuntu.sh" "$@"
