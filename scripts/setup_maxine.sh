#!/usr/bin/env bash
set -euo pipefail

# StudioCast Maxine setup helper.
#
# This script does NOT redistribute NVIDIA Maxine SDK assets.
# You must obtain the SDK tarballs yourself from NVIDIA and you must comply with NVIDIA's licensing terms.
#
# What this script CAN do:
# - Create the expected StudioCast Maxine directory layout under XDG_DATA_HOME
# - Extract user-provided Maxine SDK tarballs into the correct locations
# - (Optionally) install Maxine "features" (models/libs) for VFX/AR via the SDK's install_feature.sh scripts
#   if you provide an NGC API key (NGC_CLI_API_KEY) and a supported GPU mapping.
# - (Optionally) download Audio Effects (AFX) features via the SDK's download_features.sh script
#   if you provide an NGC API key (NGC_API_KEY).

usage() {
  cat <<'EOF'
Usage:
  scripts/setup_maxine.sh [options]

Options:
  --base DIR            Base directory (default: $XDG_DATA_HOME/studiocast/maxine or ~/.local/share/studiocast/maxine)
  --vfx-tar PATH        Path to NVIDIA_VFX_SDK_linux_<version>.tar.gz
  --ar-tar PATH         Path to NVIDIA_AR_SDK_linux_<version>.tar.gz
  --afx-tar PATH        Path to Audio_Effects_SDK.tar.gz
  --extract             Extract provided tarballs (default if any tarball arg is given)
  --install-features    Run install_feature.sh for VFX/AR (requires NGC_CLI_API_KEY)
  --install-afx-features  Download AFX features needed for the MVP (requires NGC_API_KEY)
  --afx-effects CSV     AFX effect list to pass to download_features.sh --effects (default: MVP AEC + Superres)
  --gpu ARG             Maxine --gpu argument to pass to install_feature.sh. Can be repeated.
  --build-dir DIR       Build dir containing studiocast-maxine (default: ./build). Used to auto-detect --gpu args.
  --ngc-org ORG         NGC org (default: nvidia)
  --ngc-team TEAM       NGC team (default: maxine)
  -h, --help            Show help.

Examples:
  # Extract VFX + AR SDKs into the default XDG location:
  ./scripts/setup_maxine.sh --vfx-tar ~/Downloads/NVIDIA_VFX_SDK_linux_*.tar.gz \
                            --ar-tar  ~/Downloads/NVIDIA_AR_SDK_linux_*.tar.gz

  # Install features (requires NGC_CLI_API_KEY). Auto-detect --gpu args from studiocast-maxine:
  export NGC_CLI_API_KEY="..."
  ./scripts/setup_maxine.sh --install-features --build-dir ./build

  # Install features with explicit GPU arg(s):
  export NGC_CLI_API_KEY="..."
  ./scripts/setup_maxine.sh --install-features --gpu turing

  # Download AFX features for the audio MVP (does not require --gpu):
  export NGC_API_KEY="..."
  ./scripts/setup_maxine.sh --afx-tar ~/Downloads/Audio_Effects_SDK.tar.gz --install-afx-features

  # Download a custom AFX feature set:
  export NGC_API_KEY="..."
  ./scripts/setup_maxine.sh --install-afx-features --afx-effects "superres-16k_to_48k,superres-8k_to_16k,aec-16k,aec-48k"
EOF
}

default_base() {
  local xdg="${XDG_DATA_HOME:-$HOME/.local/share}"
  echo "${xdg}/studiocast/maxine"
}

BASE="$(default_base)"
VFX_TAR=""
AR_TAR=""
AFX_TAR=""
DO_EXTRACT=0
DO_INSTALL_FEATURES=0
DO_INSTALL_AFX_FEATURES=0
AFX_EFFECTS_CSV=""
declare -a GPU_ARGS=()
BUILD_DIR="./build"
NGC_ORG="nvidia"
NGC_TEAM="maxine"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base) BASE="${2:-}"; shift 2 ;;
    --vfx-tar) VFX_TAR="${2:-}"; DO_EXTRACT=1; shift 2 ;;
    --ar-tar) AR_TAR="${2:-}"; DO_EXTRACT=1; shift 2 ;;
    --afx-tar) AFX_TAR="${2:-}"; DO_EXTRACT=1; shift 2 ;;
    --extract) DO_EXTRACT=1; shift ;;
    --install-features) DO_INSTALL_FEATURES=1; shift ;;
    --install-afx-features) DO_INSTALL_AFX_FEATURES=1; shift ;;
    --afx-effects) AFX_EFFECTS_CSV="${2:-}"; shift 2 ;;
    --gpu) GPU_ARGS+=("${2:-}"); shift 2 ;;
    --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
    --ngc-org) NGC_ORG="${2:-}"; shift 2 ;;
    --ngc-team) NGC_TEAM="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1"; usage; exit 2 ;;
  esac
done

mkdir -p "$BASE"

VFX_ROOT="${BASE}/VideoFX"
AR_ROOT="${BASE}/ARSDK"
AFX_ROOT="${BASE}/Audio_Effects_SDK"

if [[ "$DO_EXTRACT" -eq 1 ]]; then
  echo "[maxine] Base: $BASE"

  if [[ -n "$VFX_TAR" ]]; then
    echo "[maxine] Extracting VFX: $VFX_TAR"
    tar -xvf "$VFX_TAR" -C "$BASE"
    if [[ ! -d "$VFX_ROOT" ]]; then
      echo "[maxine] ERROR: Expected VFX root at $VFX_ROOT after extraction."
      echo "[maxine] Make sure the VFX SDK tarball contains a top-level 'VideoFX' directory."
      exit 1
    fi
  fi

  if [[ -n "$AR_TAR" ]]; then
    echo "[maxine] Extracting AR: $AR_TAR"
    tar -xvf "$AR_TAR" -C "$BASE"
    if [[ ! -d "$AR_ROOT" ]]; then
      echo "[maxine] ERROR: Expected AR root at $AR_ROOT after extraction."
      echo "[maxine] Make sure the AR SDK tarball contains a top-level 'ARSDK' directory."
      exit 1
    fi
  fi

  if [[ -n "$AFX_TAR" ]]; then
    echo "[maxine] Extracting AFX: $AFX_TAR"
    mkdir -p "$BASE"
    ( cd "$BASE" && tar xvf "$AFX_TAR" --one-top-level Audio_Effects_SDK )
    if [[ ! -d "$AFX_ROOT" ]]; then
      echo "[maxine] ERROR: Expected AFX root at $AFX_ROOT after extraction."
      exit 1
    fi
  fi
fi

# If user didn't specify --gpu args, try to derive them from studiocast-maxine (best source of truth).
if [[ "${#GPU_ARGS[@]}" -eq 0 && "$DO_INSTALL_FEATURES" -eq 1 ]]; then
  MAXINE_BIN=""
  if [[ -x "${BUILD_DIR}/studiocast-maxine" ]]; then
    MAXINE_BIN="${BUILD_DIR}/studiocast-maxine"
  elif command -v studiocast-maxine >/dev/null 2>&1; then
    MAXINE_BIN="$(command -v studiocast-maxine)"
  fi

  if [[ -n "$MAXINE_BIN" ]]; then
    echo "[maxine] Auto-detecting Maxine --gpu args via: $MAXINE_BIN gpu list"
    # Example output includes: "(maxine --gpu turing)"
    mapfile -t GPU_ARGS < <("$MAXINE_BIN" gpu list | sed -n 's/.*(maxine --gpu \([^)]\+\)).*/\1/p' | sort -u)
  fi
fi

if [[ "$DO_INSTALL_FEATURES" -eq 1 ]]; then
  if [[ -z "${NGC_CLI_API_KEY:-}" ]]; then
    echo "[maxine] ERROR: NGC_CLI_API_KEY is not set."
    echo "[maxine] Export your NGC API key (do not commit it):"
    echo '  export NGC_CLI_API_KEY="..."'
    exit 1
  fi

  if [[ "${#GPU_ARGS[@]}" -eq 0 ]]; then
    echo "[maxine] ERROR: No --gpu args detected."
    echo "[maxine] Either:"
    echo "  - Build and run ./build/studiocast-maxine gpu list, then re-run this script, OR"
    echo "  - Provide --gpu manually (e.g. --gpu turing / ampere / ada depending on your system)."
    exit 1
  fi

  if [[ ! -d "${VFX_ROOT}/features" ]]; then
    echo "[maxine] ERROR: VFX features directory not found at ${VFX_ROOT}/features"
    exit 1
  fi
  if [[ ! -d "${AR_ROOT}/features" ]]; then
    echo "[maxine] ERROR: AR features directory not found at ${AR_ROOT}/features"
    exit 1
  fi

  echo "[maxine] Installing VFX/AR features for GPU args: ${GPU_ARGS[*]}"
  echo "[maxine] Using NGC org/team: ${NGC_ORG}/${NGC_TEAM}"

  for gpu in "${GPU_ARGS[@]}"; do
    echo "[maxine] VFX install_feature.sh --gpu ${gpu}"
    ( cd "${VFX_ROOT}/features" && ./install_feature.sh --gpu "${gpu}" --feature all --ngc-org "${NGC_ORG}" --ngc-team "${NGC_TEAM}" )

    echo "[maxine] AR install_feature.sh --gpu ${gpu}"
    ( cd "${AR_ROOT}/features" && ./install_feature.sh --gpu "${gpu}" --feature all --ngc-org "${NGC_ORG}" --ngc-team "${NGC_TEAM}" )
  done

  echo "[maxine] Feature install complete."
  echo "[maxine] You can now verify with:"
  echo "  ${BUILD_DIR}/studiocast-maxine doctor"
fi

if [[ "$DO_INSTALL_AFX_FEATURES" -eq 1 ]]; then
  if [[ -z "${NGC_API_KEY:-}" ]]; then
    echo "[maxine] ERROR: NGC_API_KEY is not set."
    echo "[maxine] Export your NGC API key (do not commit it):"
    echo '  export NGC_API_KEY="..."'
    exit 1
  fi

  if [[ ! -d "${AFX_ROOT}/features" ]]; then
    echo "[maxine] ERROR: AFX features directory not found at ${AFX_ROOT}/features"
    echo "[maxine] Make sure you extracted Audio_Effects_SDK.tar.gz so that ${AFX_ROOT} exists."
    exit 1
  fi

  # MVP minimal list: AEC + Superres.
  AFX_EFFECTS_DEFAULT="superres-16k_to_48k,superres-8k_to_16k,aec-16k,aec-48k"
  AFX_EFFECTS="${AFX_EFFECTS_CSV:-$AFX_EFFECTS_DEFAULT}"
  if [[ -z "$AFX_EFFECTS" ]]; then
    echo "[maxine] ERROR: --afx-effects was provided but empty."
    exit 1
  fi

  echo "[maxine] Downloading AFX features: ${AFX_EFFECTS}"
  ( cd "${AFX_ROOT}/features" && ./download_features.sh --effects "${AFX_EFFECTS}" )

  echo "[maxine] AFX feature download complete."
  echo "[maxine] You can now verify with:"
  echo "  ${BUILD_DIR}/studiocast-maxine doctor"
  echo "  ${BUILD_DIR}/studiocastctl status"
fi

echo "[maxine] Roots:"
echo "  VFX: $VFX_ROOT"
echo "  AR : $AR_ROOT"
echo "  AFX: $AFX_ROOT"
echo "[maxine] Done."