#!/usr/bin/env bash
set -euo pipefail

# StudioCast uninstall helper.
#
# Two modes:
#   1) Simple uninstall (default): remove user-installed binaries/service + runtime socket.
#   2) Greedy uninstall (--greedy): additionally remove most local data/config/state and
#      attempt to purge common system dependencies installed by repo helper scripts.

usage() {
  cat <<'EOF'
Usage:
  ./scripts/uninstall.sh [options]

Modes:
  (default)          Simple uninstall: remove user-level artifacts only.
  --greedy           Also remove most local StudioCast data/config/state and attempt to
                     remove/purge common system dependencies (requires sudo).

Options:
  --dry-run          Print actions without executing.
  -y, --yes          Assume yes (skip confirmation prompts).
  -h, --help         Show help.

Notes:
  - This script is intended for dev/debug/testing of install/setup flows.
  - In --greedy mode, package removal may affect other software on your system.
EOF
}

log() { echo "[uninstall] $*"; }
warn() { echo "[uninstall] WARNING: $*" >&2; }
die() { echo "[uninstall] ERROR: $*" >&2; exit 2; }

DRY_RUN=0
YES=0
GREEDY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --greedy) GREEDY=1; shift ;;
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

run_quiet() {
  # Like run(), but suppress stdout/stderr in non-dry-run mode.
  if [[ "$DRY_RUN" -eq 1 ]]; then
    run "$@"
    return 0
  fi
  "$@" >/dev/null 2>&1
}

rm_path() {
  local p="$1"
  [[ -n "$p" ]] || die "Internal error: empty path"
  case "$p" in
    /) die "Refusing to remove '/'" ;;
    .|..) die "Refusing to remove relative path '$p'" ;;
  esac

  if [[ -e "$p" || -L "$p" ]]; then
    run rm -rf -- "$p"
  fi
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

HOME_DIR="${HOME:-}"
[[ -n "$HOME_DIR" ]] || die "HOME is not set"

XDG_DATA_HOME_DIR="${XDG_DATA_HOME:-$HOME_DIR/.local/share}"
XDG_CONFIG_HOME_DIR="${XDG_CONFIG_HOME:-$HOME_DIR/.config}"
XDG_STATE_HOME_DIR="${XDG_STATE_HOME:-$HOME_DIR/.local/state}"

UID_NUM="$(id -u)"
XDG_RUNTIME_DIR_DIR="${XDG_RUNTIME_DIR:-/tmp/studiocast-runtime-${UID_NUM}}"

STUDIOCAST_DATA_DIR="${XDG_DATA_HOME_DIR}/studiocast"
STUDIOCAST_CONFIG_DIR="${XDG_CONFIG_HOME_DIR}/studiocast"
STUDIOCAST_STATE_DIR="${XDG_STATE_HOME_DIR}/studiocast"
STUDIOCAST_RUNTIME_DIR="${XDG_RUNTIME_DIR_DIR}/studiocast"

SYSTEMD_USER_DIR="${XDG_CONFIG_HOME_DIR}/systemd/user"
SYSTEMD_SERVICE_PATH="${SYSTEMD_USER_DIR}/studiocastd.service"

LOCAL_BIN_DIR="${HOME_DIR}/.local/bin"

remove_systemd_user_service() {
  if ! have_cmd systemctl; then
    return 0
  fi

  # Best-effort: service might not be installed.
  run_quiet systemctl --user stop studiocastd.service || true
  run_quiet systemctl --user disable studiocastd.service || true

  if [[ -f "$SYSTEMD_SERVICE_PATH" ]]; then
    log "Removing user systemd unit: $SYSTEMD_SERVICE_PATH"
    rm_path "$SYSTEMD_SERVICE_PATH"
  fi

  run_quiet systemctl --user daemon-reload || true
}

remove_user_binaries() {
  local -a bins=(
    "studiocast"
    "studiocastd"
    "studiocastctl"
    "studiocast-probe"
    "studiocast-open"
    "studiocast-maxine"
    "studiocast-video"
    "studiocast-audio"
  )

  for b in "${bins[@]}"; do
    local p="${LOCAL_BIN_DIR}/${b}"
    if [[ -e "$p" || -L "$p" ]]; then
      log "Removing binary: $p"
      rm_path "$p"
    fi
  done
}

remove_runtime_artifacts() {
  if [[ -d "$STUDIOCAST_RUNTIME_DIR" ]]; then
    log "Removing runtime dir: $STUDIOCAST_RUNTIME_DIR"
    rm_path "$STUDIOCAST_RUNTIME_DIR"
  fi
}

remove_user_desktop_entry_if_present() {
  local p="${XDG_DATA_HOME_DIR}/applications/studiocast.desktop"
  if [[ -f "$p" ]]; then
    log "Removing desktop entry: $p"
    rm_path "$p"
  fi
}

greedy_remove_user_data() {
  log "Greedy mode: removing StudioCast user data/config/state (XDG)"
  rm_path "$STUDIOCAST_DATA_DIR"
  rm_path "$STUDIOCAST_CONFIG_DIR"
  rm_path "$STUDIOCAST_STATE_DIR"
}

# Directories that can hold the onnxruntime.pc link the setup helper made.
#
# scripts/_lib/onnxruntime.sh links the file into the first directory that
# pkg-config searches, so ask pkg-config for that list. Without pkg-config,
# fall back to the two directories that list almost always starts with.
onnxruntime_pc_link_dirs() {
  local pc_path=""

  if have_cmd pkg-config; then
    pc_path="$(pkg-config --variable pc_path pkg-config 2>/dev/null || true)"
  fi

  if [[ -n "$pc_path" ]]; then
    printf '%s\n' "$pc_path" | tr ':' '\n' | grep -v '^$' || true
    return 0
  fi

  printf '%s\n' /usr/lib64/pkgconfig /usr/lib/pkgconfig
}

# Remove the onnxruntime.pc links that point at our bootstrap file.
#
# Arguments: <bootstrap .pc file> <directory>...
#
# Only a symlink whose target is that file is removed, so a file owned by a
# distribution package is never touched.
remove_onnxruntime_pc_links() {
  local pc_file="$1"
  shift

  local dir pc_link
  for dir in "$@"; do
    pc_link="${dir}/onnxruntime.pc"
    if [[ -L "$pc_link" && "$(readlink -- "$pc_link")" == "$pc_file" ]]; then
      log "Removing: $pc_link"
      run sudo rm -f -- "$pc_link"
    fi
  done
}

greedy_remove_system_onnxruntime_bootstrap() {
  # Installed by scripts/setup.sh --deps (scripts/setup/ubuntu.sh and
  # scripts/setup/fedora.sh). The cuDNN tree and the CUDA ld.so.conf.d file come
  # from the Fedora gpu flavor. The NVIDIA rpms are never touched here.
  local pc_file="/usr/local/lib/pkgconfig/onnxruntime.pc"
  local -a trees=(
    "/opt/studiocast/onnxruntime"
    "/opt/studiocast/cudnn"
  )
  local -a ld_confs=(
    "/etc/ld.so.conf.d/studiocast-onnxruntime.conf"
    "/etc/ld.so.conf.d/studiocast-cudnn.conf"
    "/etc/ld.so.conf.d/studiocast-cuda.conf"
  )

  local tree
  for tree in "${trees[@]}"; do
    if [[ -d "$tree" ]]; then
      log "Removing: $tree"
      run sudo rm -rf -- "$tree"
    fi
  done

  local removed_ld_conf=0
  local ld_conf
  for ld_conf in "${ld_confs[@]}"; do
    if [[ -f "$ld_conf" ]]; then
      log "Removing: $ld_conf"
      run sudo rm -f -- "$ld_conf"
      removed_ld_conf=1
    fi
  done

  if [[ "$removed_ld_conf" -eq 1 ]]; then
    run sudo ldconfig || true
  fi

  local -a pc_dirs=()
  mapfile -t pc_dirs < <(onnxruntime_pc_link_dirs)
  remove_onnxruntime_pc_links "$pc_file" "${pc_dirs[@]}"

  if [[ -f "$pc_file" ]]; then
    log "Removing: $pc_file"
    run sudo rm -f -- "$pc_file"
  fi
}

greedy_remove_v4l2loopback_persistence() {
  # Persisted by scripts/setup.sh / scripts/setup/ubuntu.sh (compat wrapper existed as setup_ubuntu22.sh).
  local modprobe_conf="/etc/modprobe.d/studiocast-v4l2loopback.conf"
  local modules_load_conf="/etc/modules-load.d/v4l2loopback.conf"

  log "Greedy mode: removing v4l2loopback persistence (if created for StudioCast)"

  if have_cmd modprobe; then
    run_quiet sudo modprobe -r v4l2loopback || true
  fi

  if [[ -f "$modprobe_conf" ]]; then
    log "Removing: $modprobe_conf"
    run sudo rm -f -- "$modprobe_conf"
  fi

  if [[ -f "$modules_load_conf" ]]; then
    # Be conservative: only remove if it's a single-line file containing exactly 'v4l2loopback'.
    if [[ "$(wc -l < "$modules_load_conf" | tr -d ' ')" == "1" ]] && grep -qx 'v4l2loopback' "$modules_load_conf"; then
      log "Removing: $modules_load_conf"
      run sudo rm -f -- "$modules_load_conf"
    else
      warn "Not removing $modules_load_conf (contents don't match expected single-line 'v4l2loopback')."
    fi
  fi
}

greedy_purge_apt_dependencies() {
  if ! have_cmd apt-get; then
    warn "apt-get not found; skipping dependency purge"
    return 0
  fi

  # A best-effort list of packages installed by repo helper scripts.
  # This may remove packages used by other software.
  local -a pkgs=(
    build-essential cmake ninja-build pkg-config
    git curl ca-certificates tar
    qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools
    qtbase5-dev
    libxkbcommon-dev
    libpulse-dev libpulse0 pulseaudio-utils
    clang clang-format clang-tidy
    v4l-utils v4l2loopback-utils
    libblas-dev liblapack-dev
    libdlib-dev libsqlite3-dev
    libjpeg-turbo8 libjpeg-turbo8-dev
    libpng-dev
    dkms "linux-headers-$(uname -r)" v4l2loopback-dkms
  )

  log "Greedy mode: attempting to purge apt dependencies (best-effort)"
  run sudo apt-get update
  # Intentionally allow failures (packages may not be installed).
  run sudo apt-get remove --purge -y "${pkgs[@]}" || true
  run sudo apt-get autoremove --purge -y || true
  run sudo apt-get -f install -y || true
}

main() {
  log "Mode: $([[ "$GREEDY" -eq 1 ]] && echo greedy || echo simple)"
  log "Dry-run: $([[ "$DRY_RUN" -eq 1 ]] && echo yes || echo no)"

  if [[ "$GREEDY" -eq 1 ]]; then
    log "Greedy mode will remove local data and attempt to purge system packages."
    if ! confirm "Proceed with greedy uninstall?"; then
      log "Cancelled."
      exit 0
    fi
  fi

  remove_systemd_user_service
  remove_user_binaries
  remove_user_desktop_entry_if_present
  remove_runtime_artifacts

  if [[ "$GREEDY" -eq 1 ]]; then
    greedy_remove_user_data

    if ! have_cmd sudo; then
      warn "sudo not found; skipping system-level cleanup (onnxruntime/v4l2loopback/apt purge)"
    else
      greedy_remove_system_onnxruntime_bootstrap
      greedy_remove_v4l2loopback_persistence
      greedy_purge_apt_dependencies
    fi
  fi

  log "Done."
}

# Run only when this file is the program. tests/uninstall_pkgconfig_tests.sh
# sources it to call single functions.
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  main
fi
