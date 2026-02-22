#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
StudioCast install entrypoint

Usage:
  ./scripts/install.sh <command> [args...]

Commands:
  user-service        Install/enable the systemd --user service (studiocastd)
  open-audio-models   Download/install curated Open Audio model packs

Examples:
  ./scripts/install.sh user-service --build-dir ./cmake-build-debug
  ./scripts/install.sh open-audio-models --list
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmd="${1:-}"
if [[ -z "$cmd" || "$cmd" == "-h" || "$cmd" == "--help" ]]; then
  usage
  exit 0
fi
shift

case "$cmd" in
  user-service)
    exec "${SCRIPT_DIR}/install/user_service.sh" "$@"
    ;;
  open-audio-models)
    exec "${SCRIPT_DIR}/install/open_audio_models.sh" "$@"
    ;;
  *)
    echo "[install] ERROR: Unknown command: $cmd" >&2
    usage
    exit 2
    ;;
esac
