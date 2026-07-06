#!/usr/bin/env bash
set -euo pipefail

# Install and enable the StudioCast systemd --user service.
#
# This is intended for dev/debug/MVP tester convenience.

usage() {
  cat <<'EOF'
Usage:
  ./scripts/install/user_service.sh [options]

Options:
  --build-dir DIR     Build dir containing built StudioCast binaries.
                     Default: auto-detect first existing of:
                       <repo>/build, <repo>/cmake-build-release, <repo>/cmake-build-debug
  --no-link-bins      Do not create/update ~/.local/bin symlinks
  --link-bins-only    Create/update ~/.local/bin symlinks, but do not install
                     or start the systemd user service
  --dry-run           Print actions without executing
  -y, --yes           Assume yes (skip confirmation prompts)
  -h, --help          Show help

What it does:
  - (default) Symlinks built binaries into ~/.local/bin
  - Copies packaging/systemd/user/studiocastd.service into ~/.config/systemd/user/
  - Runs: systemctl --user daemon-reload && systemctl --user enable --now studiocastd.service

Notes:
  - The unit ExecStart is %h/.local/bin/studiocastd.
  - If you change the daemon arguments, edit ~/.config/systemd/user/studiocastd.service and run:
      systemctl --user daemon-reload && systemctl --user restart studiocastd.service
EOF
}

log() { echo "[install] $*"; }
die() { echo "[install] ERROR: $*" >&2; exit 2; }

DRY_RUN=0
YES=0
BUILD_DIR=""
LINK_BINS=1
LINK_BINS_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      [[ $# -ge 2 ]] || die "--build-dir requires a path"
      BUILD_DIR="$2"
      shift 2
      ;;
    --no-link-bins) LINK_BINS=0; shift ;;
    --link-bins-only) LINK_BINS_ONLY=1; LINK_BINS=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -y|--yes) YES=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown argument: $1 (use --help)" ;;
  esac
done

run() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    printf '+ ' ; printf '%q ' "$@" ; printf '\n'
    return 0
  fi
  "$@"
}

confirm() {
  local prompt="$1"
  if [[ "$YES" -eq 1 ]]; then
    return 0
  fi
  if [[ ! -t 0 ]]; then
    die "Refusing to prompt on a non-interactive stdin. Re-run with --yes."
  fi
  read -r -p "$prompt [y/N] " ans
  case "${ans}" in
    y|Y|yes|YES) return 0 ;;
    *) return 1 ;;
  esac
}

have_cmd() { command -v "$1" >/dev/null 2>&1; }

print_cmd() {
  printf '+ '
  printf '%q ' "$@"
  printf '\n'
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
UNIT_SRC="${REPO_ROOT}/packaging/systemd/user/studiocastd.service"

HOME_DIR="${HOME:-}"
[[ -n "$HOME_DIR" ]] || die "HOME is not set"

default_build_dir() {
  local d
  for d in "${REPO_ROOT}/build" "${REPO_ROOT}/cmake-build-release" "${REPO_ROOT}/cmake-build-debug"; do
    if [[ -d "${d}" ]]; then
      echo "${d}"
      return 0
    fi
  done
  echo "${REPO_ROOT}/build"
}

normalize_build_dir() {
  if [[ -z "${BUILD_DIR}" ]]; then
    BUILD_DIR="$(default_build_dir)"
  fi

  # Expand "~" manually for convenience.
  if [[ "${BUILD_DIR}" == "~"* ]]; then
    BUILD_DIR="${BUILD_DIR/#~/${HOME_DIR}}"
  fi

  if [[ ! -d "${BUILD_DIR}" ]]; then
    die "Build dir not found: ${BUILD_DIR}"
  fi

  # Canonicalize to an absolute path so symlinks in ~/.local/bin are never broken.
  BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
}

XDG_CONFIG_HOME_DIR="${XDG_CONFIG_HOME:-$HOME_DIR/.config}"
SYSTEMD_USER_DIR="${XDG_CONFIG_HOME_DIR}/systemd/user"
APP_CONFIG_DIR="${XDG_CONFIG_HOME_DIR}/studiocast"

LOCAL_BIN_DIR="${HOME_DIR}/.local/bin"
UNIT_DST="${SYSTEMD_USER_DIR}/studiocastd.service"

link_bins() {
  [[ -d "$BUILD_DIR" ]] || die "Build dir not found: $BUILD_DIR"

  if [[ ! -x "${BUILD_DIR}/studiocastd" ]]; then
    die "studiocastd not found in ${BUILD_DIR} (or not executable). Expected: ${BUILD_DIR}/studiocastd. Build it first, or pass --build-dir."
  fi

  run mkdir -p -- "$LOCAL_BIN_DIR"

  local -a bins=(
    "studiocastd"
    "studiocastctl"
    "studiocast-probe"
    "studiocast"
    "studiocast-video"
    "studiocast-audio"
    "studiocast-open"
    "studiocast-maxine"
  )

  for b in "${bins[@]}"; do
    local src="${BUILD_DIR}/${b}"
    local dst="${LOCAL_BIN_DIR}/${b}"
    if [[ -x "$src" ]]; then
      log "Linking: $dst -> $src"
      run ln -sf -- "$src" "$dst"
    fi
  done

  if [[ "$DRY_RUN" -eq 0 ]]; then
    if [[ ! -x "${LOCAL_BIN_DIR}/studiocastd" ]]; then
      die "Failed to install ~/.local/bin/studiocastd (symlink missing or not executable)."
    fi
  fi
}

install_unit() {
  [[ -f "$UNIT_SRC" ]] || die "Unit template not found: $UNIT_SRC"
  run mkdir -p -- "$SYSTEMD_USER_DIR"
  log "Installing systemd user unit: $UNIT_DST"
  run install -m 0644 -- "$UNIT_SRC" "$UNIT_DST"
}

configure_daemon_defaults() {
  local conf="${APP_CONFIG_DIR}/daemon.conf"
  log "Ensuring daemon CPU resize fallback is enabled by default"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    print_cmd mkdir -p -- "$APP_CONFIG_DIR"
    printf '+ ensure-daemon-config %q %q\n' "$conf" "video.scaling.allow_cpu_resize=true"
    return 0
  fi

  mkdir -p -- "$APP_CONFIG_DIR"
  if [[ ! -f "$conf" ]]; then
    {
      printf '# StudioCast daemon (studiocastd) configuration\n'
      printf '# This file is managed by the StudioCast GUI / studiocastctl.\n\n'
      printf 'video.scaling.allow_cpu_resize = true\n'
    } > "$conf"
    return 0
  fi

  local tmp="${conf}.tmp"
  awk '
    BEGIN { done = 0 }
    /^[[:space:]]*video[.]scaling[.]allow_cpu_resize[[:space:]]*=/ {
      print "video.scaling.allow_cpu_resize = true"
      done = 1
      next
    }
    { print }
    END {
      if (!done) {
        print ""
        print "video.scaling.allow_cpu_resize = true"
      }
    }
  ' "$conf" > "$tmp"
  mv -- "$tmp" "$conf"
}

enable_unit() {
  if ! have_cmd systemctl; then
    log "systemctl not found; installed unit but cannot enable/start it automatically."
    return 0
  fi

  log "Enabling and starting studiocastd.service"
  run systemctl --user daemon-reload
  run systemctl --user enable --now studiocastd.service
}

main() {
  normalize_build_dir

  log "Repo root: $REPO_ROOT"
  log "Build dir: $BUILD_DIR"
  log "Dry-run: $([[ "$DRY_RUN" -eq 1 ]] && echo yes || echo no)"
  log "Link-bins-only: $([[ "$LINK_BINS_ONLY" -eq 1 ]] && echo yes || echo no)"

  if [[ "$LINK_BINS" -eq 1 ]]; then
    if ! confirm "Install/refresh ~/.local/bin symlinks from build dir?"; then
      log "Cancelled."
      exit 0
    fi
  fi

  if [[ "$LINK_BINS" -eq 1 ]]; then
    link_bins
  fi

  configure_daemon_defaults

  if [[ "$LINK_BINS_ONLY" -eq 1 ]]; then
    log "Skipping systemd user service install/start (--link-bins-only)."
    log "Done."
    return 0
  fi

  install_unit
  enable_unit

  log "Done."
  log "Status: systemctl --user status studiocastd.service"
  log "Logs:   journalctl --user -u studiocastd.service -f"
}

main
