#!/usr/bin/env bash
set -euo pipefail

# StudioCast setup for Ubuntu 22.04+ (dev + MVP tester convenience).
#
# Robust across Ubuntu versions by:
# - Preferring kernel-provided v4l2loopback if available
# - Falling back to DKMS only if module isn't present
# - Persisting module load + options across reboot via /etc/modules-load.d and /etc/modprobe.d
#
# Also self-heals a common Ubuntu 24.04+ situation:
# - v4l2loopback-dkms is installed/half-installed but kernel already has newer v4l2loopback,
#   causing dpkg to fail on every future apt operation.

usage() {
  cat <<'EOF'
Usage:
  scripts/setup_ubuntu22.sh [options]

Options:
  --deps               Install build/runtime deps via apt.
  --v4l2loopback       Ensure v4l2loopback is available (prefer kernel module; DKMS fallback).
  --load-loopback      Load v4l2loopback now (creates /dev/videoN).
  --persist-loopback   Persist v4l2loopback module + options across reboot.
  --video-nr N         Loopback device number (default: 10).
  --label TEXT         Loopback device label (default: "StudioCast Camera").
  --exclusive-caps 0|1 exclusive_caps module option (default: 1).
  --build              Configure + build (Ninja) into --build-dir.
  --build-dir DIR      Build directory (default: ./build).
  --build-type TYPE    CMake build type (default: Release).
  --maxine             Run Maxine setup helper (see scripts/setup_maxine.sh).
  -y, --yes            Assume yes for apt installs.
  -h, --help           Show help.

Examples:
  ./scripts/setup_ubuntu22.sh --deps --v4l2loopback --load-loopback --persist-loopback
  ./scripts/setup_ubuntu22.sh --build --build-type Release
EOF
}

YES=0
DO_DEPS=0
DO_V4L2=0
DO_LOAD_LOOP=0
DO_PERSIST_LOOP=0
VIDEO_NR=10
LABEL="StudioCast Camera"
EXCLUSIVE_CAPS=1
DO_BUILD=0
BUILD_DIR="./build"
BUILD_TYPE="Release"
DO_MAXINE=0
MAXINE_ARGS=()
PARSE_MAXINE_ARGS=0

log() { echo "[setup] $*"; }

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "[setup] Missing required command: $1"; exit 1; }
}

APT_ARGS=()
APT_GET_ARGS=()

apt_install() {
  local pkgs=("$@")
  sudo apt update
  sudo apt install "${APT_ARGS[@]}" "${pkgs[@]}"
}

have_module() {
  # Does the module exist for this running kernel?
  modinfo v4l2loopback >/dev/null 2>&1
}

ensure_kernel_extras() {
  # On some installs the module may live in linux-modules-extra for the current kernel.
  # Installing it is safe even if already present.
  apt_install "linux-modules-extra-$(uname -r)" || true
}

pkg_status() {
  # Print dpkg status keyword for a package, or empty if unknown.
  # Examples: install ok installed, install ok half-configured, deinstall ok config-files, etc.
  dpkg-query -W -f='${Status}' "$1" 2>/dev/null || true
}

is_pkg_installedish() {
  # True if package is installed OR half-installed/half-configured/unpacked (i.e., can break apt).
  local st
  st="$(pkg_status "$1")"
  [[ "$st" == install\ ok\ installed ]] || \
  [[ "$st" == install\ ok\ unpacked ]] || \
  [[ "$st" == install\ ok\ half-configured ]] || \
  [[ "$st" == install\ ok\ half-installed ]] || \
  [[ "$st" == install\ ok\ triggers-awaited ]] || \
  [[ "$st" == install\ ok\ triggers-pending ]]
}

fix_broken_dkms_if_newer_kernel_module_exists() {
  # If v4l2loopback exists in the kernel AND v4l2loopback-dkms is installed-ish,
  # purge it to avoid dpkg repeatedly failing on future apt installs.
  if have_module; then
    if is_pkg_installedish v4l2loopback-dkms; then
      log "Detected in-kernel v4l2loopback + v4l2loopback-dkms in dpkg state: '$(pkg_status v4l2loopback-dkms)'."
      log "Purging v4l2loopback-dkms (and dkms) to avoid version conflicts..."

      # Use apt-get here for more predictable noninteractive behavior.
      sudo apt-get update
      sudo apt-get remove --purge "${APT_GET_ARGS[@]}" v4l2loopback-dkms dkms || true
      sudo apt-get -f install "${APT_GET_ARGS[@]}" || true
      sudo dpkg --configure -a || true

      # If something still lingers, try one more time.
      if is_pkg_installedish v4l2loopback-dkms; then
        log "v4l2loopback-dkms still present after purge attempt; trying again..."
        sudo apt-get remove --purge "${APT_GET_ARGS[@]}" v4l2loopback-dkms dkms || true
        sudo apt-get -f install "${APT_GET_ARGS[@]}" || true
        sudo dpkg --configure -a || true
      fi

      if is_pkg_installedish v4l2loopback-dkms; then
        echo "[setup] ERROR: Could not remove v4l2loopback-dkms cleanly."
        echo "[setup] Please run manually:"
        echo "  sudo apt remove --purge v4l2loopback-dkms dkms"
        echo "  sudo apt -f install"
        echo "  sudo dpkg --configure -a"
        exit 1
      fi

      log "DKMS conflict cleaned up."
    fi
  fi
}

ensure_v4l2loopback_available() {
  log "Ensuring v4l2loopback availability..."

  # If kernel has it, don't touch DKMS.
  if have_module; then
    log "v4l2loopback module is available for this kernel (no DKMS needed)."
    # Ensure runtime tools are present; modules-extra is harmless and may be required on minimal installs.
    apt_install "linux-modules-extra-$(uname -r)" v4l2loopback-utils v4l-utils || true
    return 0
  fi

  # Try installing modules-extra for this kernel, then re-check.
  ensure_kernel_extras
  if have_module; then
    log "v4l2loopback became available after installing linux-modules-extra."
    apt_install v4l2loopback-utils v4l-utils || true
    return 0
  fi

  # DKMS fallback only if not present.
  log "v4l2loopback not found in kernel modules; installing DKMS fallback."
  apt_install dkms "linux-headers-$(uname -r)" v4l2loopback-dkms v4l2loopback-utils v4l-utils

  if have_module; then
    log "v4l2loopback is now available (DKMS)."
  else
    echo "[setup] ERROR: v4l2loopback still not available after DKMS install."
    echo "[setup] Check dkms status: dkms status"
    exit 1
  fi
}

load_v4l2loopback_now() {
  require_cmd modprobe
  log "Loading v4l2loopback now (video_nr=${VIDEO_NR}, label=${LABEL}, exclusive_caps=${EXCLUSIVE_CAPS})..."
  sudo modprobe -r v4l2loopback 2>/dev/null || true
  sudo modprobe v4l2loopback "video_nr=${VIDEO_NR}" "card_label=${LABEL}" "exclusive_caps=${EXCLUSIVE_CAPS}"
  log "Loaded. Devices:"
  if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl --list-devices || true
  fi
  ls -l "/dev/video${VIDEO_NR}" 2>/dev/null || true
}

persist_v4l2loopback() {
  log "Persisting v4l2loopback across reboot..."
  echo "v4l2loopback" | sudo tee /etc/modules-load.d/v4l2loopback.conf >/dev/null

  cat <<EOF | sudo tee /etc/modprobe.d/studiocast-v4l2loopback.conf >/dev/null
# StudioCast v4l2loopback options
options v4l2loopback video_nr=${VIDEO_NR} card_label="${LABEL}" exclusive_caps=${EXCLUSIVE_CAPS}
EOF

  log "Wrote:"
  log "  /etc/modules-load.d/v4l2loopback.conf"
  log "  /etc/modprobe.d/studiocast-v4l2loopback.conf"
  log "You can verify after reboot with:"
  log "  modinfo v4l2loopback | head"
  log "  ls -l /dev/video${VIDEO_NR}"
}

while [[ $# -gt 0 ]]; do
  if [[ "$PARSE_MAXINE_ARGS" -eq 1 ]]; then
    MAXINE_ARGS+=("$1"); shift; continue
  fi

  case "$1" in
    --deps) DO_DEPS=1; shift ;;
    --v4l2loopback) DO_V4L2=1; DO_DEPS=1; shift ;;
    --load-loopback) DO_LOAD_LOOP=1; shift ;;
    --persist-loopback) DO_PERSIST_LOOP=1; shift ;;
    --video-nr) VIDEO_NR="${2:-}"; shift 2 ;;
    --label) LABEL="${2:-}"; shift 2 ;;
    --exclusive-caps) EXCLUSIVE_CAPS="${2:-}"; shift 2 ;;
    --build) DO_BUILD=1; shift ;;
    --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
    --build-type) BUILD_TYPE="${2:-}"; shift 2 ;;
    --maxine) DO_MAXINE=1; shift ;;
    -y|--yes) YES=1; shift ;;
    --) PARSE_MAXINE_ARGS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1"; usage; exit 2 ;;
  esac
done

if [[ -f /etc/os-release ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release
  if [[ "${ID:-}" != "ubuntu" ]]; then
    log "Warning: tuned for Ubuntu. Detected ID=${ID:-unknown}."
  else
    log "Detected Ubuntu ${VERSION_ID:-unknown} (${VERSION_CODENAME:-unknown})."
  fi
fi

if [[ "$YES" -eq 1 ]]; then
  APT_ARGS+=("-y")
  # apt-get flags for noninteractive scripting
  APT_GET_ARGS+=("-y")
fi

# IMPORTANT: heal dpkg state before any apt installs.
fix_broken_dkms_if_newer_kernel_module_exists

if [[ "$DO_DEPS" -eq 1 ]]; then
  log "Installing build/runtime dependencies..."
  apt_install \
    build-essential cmake ninja-build pkg-config \
    git curl ca-certificates \
    qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools \
    qtbase5-dev \
    libxkbcommon-dev \
    libpulse-dev libpulse0 pulseaudio-utils \
    clang clang-format clang-tidy \
    v4l-utils \
    libsqlite3-dev \
    libjpeg-turbo8 libjpeg-turbo8-dev
fi

if [[ "$DO_V4L2" -eq 1 ]]; then
  ensure_v4l2loopback_available
fi

if [[ "$DO_LOAD_LOOP" -eq 1 ]]; then
  if ! have_module; then
    ensure_v4l2loopback_available
  fi
  load_v4l2loopback_now
fi

if [[ "$DO_PERSIST_LOOP" -eq 1 ]]; then
  if ! have_module; then
    ensure_v4l2loopback_available
  fi
  persist_v4l2loopback
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  log "Configuring + building into: $BUILD_DIR (type: $BUILD_TYPE)"
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$BUILD_DIR"
  log "Built. Useful commands:"
  echo "  $BUILD_DIR/studiocast --version"
  echo "  $BUILD_DIR/studiocastd"
  echo "  $BUILD_DIR/studiocastctl status"
  echo "  $BUILD_DIR/studiocast-maxine install-hints"
fi

if [[ "$DO_MAXINE" -eq 1 ]]; then
  log "Running Maxine setup helper..."
  ./scripts/setup_maxine.sh --build-dir "$BUILD_DIR" "${MAXINE_ARGS[@]}"
fi

log "Done."
