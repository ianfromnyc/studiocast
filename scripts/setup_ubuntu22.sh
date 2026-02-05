#!/usr/bin/env bash
set -euo pipefail

# StudioCast setup for Ubuntu 22.04+ (dev + MVP tester convenience).
#
# This installs:
# - build dependencies (CMake/Ninja/Qt6/etc)
# - runtime dependencies (v4l2loopback DKMS, v4l-utils)
#
# It intentionally does NOT download NVIDIA Maxine SDK core tarballs (you must obtain them from NVIDIA).
# It can, however, help you extract SDK tarballs and install Maxine "features" if you provide an NGC API key.

usage() {
  cat <<'EOF'
Usage:
  scripts/setup_ubuntu22.sh [options]

Options:
  --deps               Install build/runtime deps via apt.
  --v4l2loopback       Install v4l2loopback DKMS (implies --deps runtime portion).
  --load-loopback      Load v4l2loopback now (creates /dev/videoN).
  --persist-loopback   Persist v4l2loopback module across reboot.
  --video-nr N         Loopback device number (default: 10).
  --label TEXT         Loopback device label (default: "StudioCast Camera").
  --build              Configure + build (Ninja) into --build-dir.
  --build-dir DIR      Build directory (default: ./build).
  --build-type TYPE    CMake build type (default: Release).
  --maxine             Run Maxine setup helper (see scripts/setup_maxine.sh).
  -y, --yes            Assume yes for apt installs.
  -h, --help           Show help.

Examples:
  # Install dependencies + v4l2loopback, load it now, persist across reboot:
  ./scripts/setup_ubuntu22.sh --deps --v4l2loopback --load-loopback --persist-loopback

  # Build:
  ./scripts/setup_ubuntu22.sh --build --build-type Release

  # After building, set up Maxine (extract SDK tarballs + install features):
  export NGC_CLI_API_KEY="..."
  ./scripts/setup_ubuntu22.sh --maxine -- \
    --vfx-tar ~/Downloads/NVIDIA_VFX_SDK_linux_*.tar.gz \
    --ar-tar  ~/Downloads/NVIDIA_AR_SDK_linux_*.tar.gz \
    --install-features --build-dir ./build
EOF
}

YES=0
DO_DEPS=0
DO_V4L2=0
DO_LOAD_LOOP=0
DO_PERSIST_LOOP=0
VIDEO_NR=10
LABEL="StudioCast Camera"
DO_BUILD=0
BUILD_DIR="./build"
BUILD_TYPE="Release"
DO_MAXINE=0
MAXINE_ARGS=()

# Allow passing args to setup_maxine.sh after a `--` separator.
PARSE_MAXINE_ARGS=0

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

# Basic OS sanity check (best-effort).
if [[ -f /etc/os-release ]]; then
  # shellcheck disable=SC1091
  source /etc/os-release
  if [[ "${ID:-}" != "ubuntu" ]]; then
    echo "[setup] Warning: This script is tuned for Ubuntu. Detected ID=${ID:-unknown}."
  fi
fi

APT_ARGS=()
if [[ "$YES" -eq 1 ]]; then APT_ARGS+=("-y"); fi

if [[ "$DO_DEPS" -eq 1 ]]; then
  echo "[setup] Installing build/runtime dependencies..."
  sudo apt update
  sudo apt install "${APT_ARGS[@]}"     build-essential cmake ninja-build pkg-config     git curl ca-certificates     qt6-base-dev qt6-base-dev-tools qt6-tools-dev qt6-tools-dev-tools     qtbase5-dev     clang clang-format clang-tidy     v4l-utils
fi

if [[ "$DO_V4L2" -eq 1 ]]; then
  echo "[setup] Installing v4l2loopback DKMS..."
  sudo apt update
  sudo apt install "${APT_ARGS[@]}"     dkms linux-headers-$(uname -r)     v4l2loopback-dkms v4l2loopback-utils
fi

if [[ "$DO_LOAD_LOOP" -eq 1 || "$DO_PERSIST_LOOP" -eq 1 ]]; then
  LOOP_ARGS=( )
  if [[ "$DO_V4L2" -eq 1 ]]; then LOOP_ARGS+=(--install); fi
  if [[ "$DO_LOAD_LOOP" -eq 1 ]]; then LOOP_ARGS+=(--load); fi
  if [[ "$DO_PERSIST_LOOP" -eq 1 ]]; then LOOP_ARGS+=(--persist); fi
  LOOP_ARGS+=(--video-nr "$VIDEO_NR" --label "$LABEL")
  if [[ "$YES" -eq 1 ]]; then LOOP_ARGS+=(--yes); fi
  ./scripts/setup_v4l2loopback.sh "${LOOP_ARGS[@]}"
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "[setup] Configuring + building into: $BUILD_DIR (type: $BUILD_TYPE)"
  cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$BUILD_DIR"
  echo "[setup] Built. Useful commands:"
  echo "  $BUILD_DIR/studiocast --version"
  echo "  $BUILD_DIR/studiocastd"
  echo "  $BUILD_DIR/studiocastctl status"
  echo "  $BUILD_DIR/studiocast-maxine install-hints"
fi

if [[ "$DO_MAXINE" -eq 1 ]]; then
  echo "[setup] Running Maxine setup helper..."
  ./scripts/setup_maxine.sh --build-dir "$BUILD_DIR" "${MAXINE_ARGS[@]}"
fi

echo "[setup] Done."