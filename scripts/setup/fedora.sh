#!/usr/bin/env bash
set -euo pipefail

# StudioCast Fedora setup helper.
#
# This script is invoked via ./scripts/setup.sh on Fedora 44.
# It installs build/runtime prerequisites and configures v4l2loopback.
#
# Differences from the Ubuntu helper:
# - ONNX Runtime comes from the distro package onnxruntime-devel. There is no
#   tarball download, so the --onnxruntime-* flags are accepted and ignored.
# - v4l2loopback is not in Fedora. It is in RPM Fusion Free, which this script
#   never enables for you.

usage() {
  cat <<'EOF'
Usage:
  ./scripts/setup.sh [options]          # on Fedora this runs scripts/setup/fedora.sh
  ./scripts/setup/fedora.sh [options]

Options:
  --deps                  Install build/runtime deps with dnf (Qt6/CMake/Ninja/etc + Pulse utils).
  --v4l2loopback          Ensure the v4l2loopback module is available (needs RPM Fusion Free).
  --load-loopback         Load v4l2loopback now (creates /dev/videoN).
  --persist-loopback      Persist module load/options across reboot.

  --video-nr N            v4l2loopback device number (default: 10).
  --label TEXT            v4l2loopback card label (default: "StudioCast Camera").
  --exclusive-caps 0|1    v4l2loopback exclusive_caps (default: 1).

  --onnxruntime-version V Accepted and ignored on Fedora (see notes below).
  --onnxruntime-flavor F  Accepted and ignored on Fedora (see notes below).
  --onnxruntime-arch A    Accepted and ignored on Fedora (see notes below).

  --build                 Configure + build StudioCast (dev convenience).
  --build-dir DIR         Build directory (default: ./cmake-build-debug).
  --build-type TYPE       CMake build type (default: Debug).

  --rpm                   Run packaging/rpm/build_rpm.sh. Arguments after -- go to it.
  --maxine                Run scripts/setup/maxine.sh. Arguments after -- go to it.
  -y, --yes               Assume yes for dnf installs.
  -h, --help              Show help.

Fedora notes:
  - ONNX Runtime comes from the Fedora package onnxruntime-devel. CMake finds it
    through its CMake config file, so no tarball and no pkg-config shim is
    needed and the --onnxruntime-* flags do nothing here. That package has the
    CPU execution provider only. For Open CUDA GPU inference, install an
    upstream ONNX Runtime GPU build by hand.
  - Fedora has no dlib package. CMake reports this and disables the Open Video
    Eye Contact effect.
  - v4l2loopback is not in Fedora. It is in RPM Fusion Free as
    akmod-v4l2loopback. Enable that repository first:
      sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
  - The GUI installer wizard stays Ubuntu-only. Use this script on Fedora.

Examples:
  ./scripts/setup.sh --deps -y
  ./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
  ./scripts/setup.sh --build --build-type Release
  ./scripts/setup.sh --rpm -- --clean
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
BUILD_DIR="./cmake-build-debug"
BUILD_TYPE="Debug"
DO_MAXINE=0
DO_RPM=0
PASSTHRU_ARGS=()
PARSE_PASSTHRU_ARGS=0
ORT_FLAGS_SEEN=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Tests can point this at a fake os-release file. Everything else uses the real one.
OS_RELEASE_FILE="${STUDIOCAST_OS_RELEASE:-/etc/os-release}"

log() { echo "[setup] $*"; }

if [[ "${STUDIOCAST_GUI_SUDO_STDIN:-0}" == "1" ]]; then
  sudo() {
    command sudo -S -p "${STUDIOCAST_GUI_SUDO_PROMPT:-[sudo] password for %u: }" "$@"
  }
fi

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "[setup] Missing required command: $1"; exit 1; }
}

# Run a command as root. Root shells (containers, rescue systems) have no sudo.
run_priv() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

DNF_ARGS=()

dnf_install() {
  require_cmd dnf
  run_priv dnf install "${DNF_ARGS[@]}" "$@"
}

have_module() {
  # Does the module exist for this running kernel?
  modinfo v4l2loopback >/dev/null 2>&1
}

rpmfusion_free_enabled() {
  command -v dnf >/dev/null 2>&1 || return 1
  dnf repolist --enabled 2>/dev/null | grep -qi 'rpmfusion-free'
}

print_rpmfusion_hint() {
  cat >&2 <<'EOF'
[setup] ERROR: v4l2loopback is not available on this system.
[setup] Fedora does not ship v4l2loopback. RPM Fusion Free ships it as
[setup] akmod-v4l2loopback. This script does not add third-party repositories
[setup] for you. Enable RPM Fusion Free, then run the setup again:
[setup]
[setup]   sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
[setup]   ./scripts/setup.sh --v4l2loopback --load-loopback --persist-loopback
[setup]
EOF
}

ensure_v4l2loopback_available() {
  log "Ensuring v4l2loopback availability..."

  if have_module; then
    log "v4l2loopback module is available for this kernel."
    dnf_install v4l-utils || true
    return 0
  fi

  if ! rpmfusion_free_enabled; then
    print_rpmfusion_hint
    exit 2
  fi

  log "RPM Fusion Free is enabled; installing akmod-v4l2loopback..."
  # akmods needs the headers of the running kernel. Older kernels can be gone
  # from the repositories, so fall back to the newest kernel-devel package.
  if ! dnf_install akmod-v4l2loopback "kernel-devel-$(uname -r)" v4l-utils; then
    log "kernel-devel-$(uname -r) is not available; using the newest kernel-devel."
    dnf_install akmod-v4l2loopback kernel-devel v4l-utils
  fi

  # akmods builds the kernel module for the running kernel. The package trigger
  # normally does this, but it is skipped in some images, so run it directly.
  if command -v akmods >/dev/null 2>&1; then
    log "Building the module with akmods (this can take a few minutes)..."
    run_priv akmods --kernels "$(uname -r)" || true
    run_priv depmod -a || true
  fi

  if have_module; then
    log "v4l2loopback is now available (akmod)."
    return 0
  fi

  echo "[setup] ERROR: v4l2loopback still not available after the akmod install." >&2
  echo "[setup] Check the build log under /var/cache/akmods/v4l2loopback/ and that" >&2
  echo "[setup] kernel-devel matches the running kernel: $(uname -r)" >&2
  exit 1
}

load_v4l2loopback_now() {
  require_cmd modprobe
  log "Loading v4l2loopback now (video_nr=${VIDEO_NR}, label=${LABEL}, exclusive_caps=${EXCLUSIVE_CAPS})..."
  run_priv modprobe -r v4l2loopback 2>/dev/null || true
  run_priv modprobe v4l2loopback "video_nr=${VIDEO_NR}" "card_label=${LABEL}" "exclusive_caps=${EXCLUSIVE_CAPS}"
  log "Loaded. Devices:"
  if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl --list-devices || true
  fi
  ls -l "/dev/video${VIDEO_NR}" 2>/dev/null || true
}

persist_v4l2loopback() {
  log "Persisting v4l2loopback across reboot..."
  echo "v4l2loopback" | run_priv tee /etc/modules-load.d/v4l2loopback.conf >/dev/null

  cat <<EOF | run_priv tee /etc/modprobe.d/studiocast-v4l2loopback.conf >/dev/null
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
  if [[ "$PARSE_PASSTHRU_ARGS" -eq 1 ]]; then
    PASSTHRU_ARGS+=("$1"); shift; continue
  fi

  case "$1" in
    --deps) DO_DEPS=1; shift ;;
    --v4l2loopback) DO_V4L2=1; shift ;;
    --load-loopback) DO_LOAD_LOOP=1; shift ;;
    --persist-loopback) DO_PERSIST_LOOP=1; shift ;;
    --video-nr) VIDEO_NR="${2:-}"; shift 2 ;;
    --label) LABEL="${2:-}"; shift 2 ;;
    --exclusive-caps) EXCLUSIVE_CAPS="${2:-}"; shift 2 ;;
    --onnxruntime-version|--onnxruntime-flavor|--onnxruntime-arch)
      ORT_FLAGS_SEEN=1; shift 2 ;;
    --build) DO_BUILD=1; shift ;;
    --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
    --build-type) BUILD_TYPE="${2:-}"; shift 2 ;;
    --rpm) DO_RPM=1; shift ;;
    --maxine) DO_MAXINE=1; shift ;;
    -y|--yes) YES=1; shift ;;
    --) PARSE_PASSTHRU_ARGS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1"; usage; exit 2 ;;
  esac
done

if [[ "$DO_RPM" -eq 1 && "$DO_MAXINE" -eq 1 ]]; then
  echo "[setup] ERROR: --rpm and --maxine both take the arguments after --." >&2
  echo "[setup] Run them one at a time." >&2
  exit 2
fi

if [[ -f "$OS_RELEASE_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$OS_RELEASE_FILE"
  if [[ "${ID:-}" == "fedora" ]]; then
    log "Detected Fedora ${VERSION_ID:-unknown}."
  elif [[ "${ID_LIKE:-}" == *fedora* ]]; then
    log "Warning: tuned for Fedora 44. Detected ID=${ID:-unknown} (ID_LIKE=${ID_LIKE})."
  else
    echo "[setup] ERROR: This helper is for Fedora. Detected ID=${ID:-unknown}." >&2
    echo "[setup] Run ./scripts/setup.sh to pick the right helper for this distro." >&2
    exit 2
  fi
fi

if [[ "$YES" -eq 1 ]]; then
  DNF_ARGS+=("-y")
fi

if [[ "$ORT_FLAGS_SEEN" -eq 1 ]]; then
  log "Note: the --onnxruntime-* flags are ignored on Fedora."
  log "      ONNX Runtime comes from the distro package onnxruntime-devel."
fi

if [[ "$DO_DEPS" -eq 1 ]]; then
  log "Installing build/runtime dependencies..."
  dnf_install \
    cmake ninja-build gcc-c++ pkgconf-pkg-config \
    git curl \
    qt6-qtbase-devel \
    pulseaudio-libs-devel pulseaudio-utils \
    libjpeg-turbo-devel libpng-devel \
    sqlite-devel \
    onnxruntime-devel \
    libyuv-devel \
    clang clang-tools-extra \
    v4l-utils

  log "ONNX Runtime comes from onnxruntime-devel; CMake finds it through its CMake config."
  log "That package has the CPU execution provider only. Open CUDA GPU inference"
  log "needs an upstream ONNX Runtime GPU build installed by hand."
  log "Fedora has no dlib package, so CMake disables the Open Video Eye Contact effect."
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
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON \
    -DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON
  cmake --build "$BUILD_DIR"
  log "Built. Useful commands:"
  echo "  $BUILD_DIR/studiocast --version"
  echo "  $BUILD_DIR/studiocastd"
  echo "  $BUILD_DIR/studiocastctl status"
  echo "  $BUILD_DIR/studiocast-maxine install-hints"
fi

if [[ "$DO_MAXINE" -eq 1 ]]; then
  log "Running Maxine setup helper..."
  "${REPO_ROOT}/scripts/setup/maxine.sh" --build-dir "$BUILD_DIR" "${PASSTHRU_ARGS[@]}"
fi

if [[ "$DO_RPM" -eq 1 ]]; then
  log "Running the RPM build helper..."
  exec "${REPO_ROOT}/packaging/rpm/build_rpm.sh" "${PASSTHRU_ARGS[@]}"
fi

log "Done."
