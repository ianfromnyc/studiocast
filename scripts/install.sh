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
  open-video-models   Download/install curated Open Video model packs
  full                Install user-service and model packs

Examples:
  ./scripts/install.sh full
  ./scripts/install.sh full --build-dir ./build
  ./scripts/install.sh user-service --build-dir ./build
  ./scripts/install.sh open-audio-models --list
  ./scripts/install.sh open-video-models --list
EOF
}

full_usage() {
  cat <<'EOF'
Usage:
  ./scripts/install.sh full [options]

Options:
  --build-dir DIR       CMake build dir containing built StudioCast binaries.
                        Default: auto-detect first existing of:
                          <repo>/build, <repo>/cmake-build-release, <repo>/cmake-build-debug
  --no-link-bins        Skip creating/updating ~/.local/bin symlinks (user-service)
  -y, --yes             Assume yes (skip confirmation prompts) (user-service)

  --audio-dest PATH     Destination root for Open Audio model packs (forwarded to open-audio-models --dest)
  --video-dest PATH     Destination root for Open Video model packs (forwarded to open-video-models --dest)
  --audio-model ID      Install only a specific Open Audio model pack (repeatable)
  --video-model ID      Install only a specific Open Video model pack (repeatable)
  --force               Re-download and overwrite existing model pack contents (audio+video)

  --list                List curated audio+video model pack IDs and exit
  --dry-run             Print the commands that would be executed and exit

  --skip-system-check   Skip system prerequisite checks (ONNX Runtime + v4l2loopback).

  -h, --help            Show help
EOF
}

print_cmd() {
  printf '+ '
  printf '%q ' "$@"
  printf '\n'
}

run_full() {
  local build_dir=""
  local no_link_bins="0"
  local yes="0"
  local force="0"
  local list="0"
  local dry_run="0"
  local skip_system_check="0"
  local audio_dest=""
  local video_dest=""
  local -a audio_models=()
  local -a video_models=()

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --build-dir)
        [[ $# -ge 2 ]] || { echo "[install] ERROR: --build-dir requires a path" >&2; exit 2; }
        build_dir="$2"
        shift 2
        ;;
      --no-link-bins)
        no_link_bins="1"
        shift
        ;;
      -y|--yes)
        yes="1"
        shift
        ;;
      --audio-dest)
        [[ $# -ge 2 ]] || { echo "[install] ERROR: --audio-dest requires a path" >&2; exit 2; }
        audio_dest="$2"
        shift 2
        ;;
      --video-dest)
        [[ $# -ge 2 ]] || { echo "[install] ERROR: --video-dest requires a path" >&2; exit 2; }
        video_dest="$2"
        shift 2
        ;;
      --audio-model)
        [[ $# -ge 2 ]] || { echo "[install] ERROR: --audio-model requires an id" >&2; exit 2; }
        audio_models+=("$2")
        shift 2
        ;;
      --video-model)
        [[ $# -ge 2 ]] || { echo "[install] ERROR: --video-model requires an id" >&2; exit 2; }
        video_models+=("$2")
        shift 2
        ;;
      --force)
        force="1"
        shift
        ;;
      --list)
        list="1"
        shift
        ;;
      --dry-run)
        dry_run="1"
        shift
        ;;
      --skip-system-check)
        skip_system_check="1"
        shift
        ;;
      -h|--help)
        full_usage
        exit 0
        ;;
      *)
        echo "[install] ERROR: Unknown argument for full: $1" >&2
        full_usage
        exit 2
        ;;
    esac
  done

  if [[ "${list}" -eq 1 ]]; then
    "${SCRIPT_DIR}/install/open_audio_models.sh" --list
    "${SCRIPT_DIR}/install/open_video_models.sh" --list
    exit 0
  fi

  local repo_root
  repo_root="$(cd "${SCRIPT_DIR}/.." && pwd)"

  default_build_dir() {
    local d
    for d in "${repo_root}/build" "${repo_root}/cmake-build-release" "${repo_root}/cmake-build-debug"; do
      if [[ -d "${d}" ]]; then
        echo "${d}"
        return 0
      fi
    done
    echo "${repo_root}/build"
  }

  if [[ -z "${build_dir}" ]]; then
    build_dir="$(default_build_dir)"
  fi

  # Expand "~" manually for convenience.
  if [[ "${build_dir}" == "~"* ]]; then
    build_dir="${build_dir/#~/${HOME}}"
  fi

  if [[ ! -d "${build_dir}" ]]; then
    echo "[install] ERROR: Build dir not found: ${build_dir}" >&2
    echo "[install] Hint: configure+build first, e.g.:" >&2
    echo "[install]   cmake -S . -B '${repo_root}/build' -DCMAKE_BUILD_TYPE=Release" >&2
    echo "[install]   cmake --build '${repo_root}/build'" >&2
    exit 2
  fi
  build_dir="$(cd "${build_dir}" && pwd)"

  system_preflight() {
    local failures=0
    local warn_only=0

    # Keep this lightweight + non-invasive. No sudo, no installs.
    echo "[install] System preflight (no sudo)..." >&2

    have_cmd() { command -v "$1" >/dev/null 2>&1; }

    if ! have_cmd python3; then
      echo "[install] ERROR: python3 not found (required by model installers)." >&2
      failures=1
    fi

    if ! have_cmd curl && ! have_cmd wget; then
      echo "[install] ERROR: curl or wget is required to download model packs." >&2
      failures=1
    fi

    # ONNX Runtime: preferred via pkg-config (build-time), but we also accept runtime-only installs.
    local ort_ok=0
    if have_cmd pkg-config && pkg-config --exists onnxruntime; then
      ort_ok=1
    elif have_cmd ldconfig && ldconfig -p 2>/dev/null | grep -q 'libonnxruntime\\.so'; then
      # Runtime lib present, but rebuilds may fail if pkg-config isn't available.
      ort_ok=1
      warn_only=1
      echo "[install] WARN: libonnxruntime.so found via ldconfig, but pkg-config 'onnxruntime' is missing." >&2
      echo "[install]       Rebuilding may fail. Recommended: ./scripts/setup.sh --deps" >&2
    fi
    if [[ "${ort_ok}" -ne 1 ]]; then
      echo "[install] ERROR: ONNX Runtime not found." >&2
      echo "[install]        Fix (Ubuntu-family):" >&2
      echo "[install]          ./scripts/setup.sh --deps" >&2
      failures=1
    fi

    # v4l2loopback: module presence is required for the virtual camera device.
    local video_nr="${STUDIOCAST_V4L2_VIDEO_NR:-10}"
    if have_cmd modinfo && modinfo v4l2loopback >/dev/null 2>&1; then
      :
    else
      echo "[install] ERROR: v4l2loopback kernel module not available (modinfo v4l2loopback failed)." >&2
      echo "[install]        Fix (Ubuntu-family):" >&2
      echo "[install]          ./scripts/setup.sh --v4l2loopback" >&2
      echo "[install]        Or (more explicit):" >&2
      echo "[install]          ./scripts/setup/v4l2loopback.sh --install" >&2
      failures=1
    fi

    # Device node: not fatal (module may not be loaded yet), but warn loudly.
    if [[ ! -e "/dev/video${video_nr}" ]]; then
      echo "[install] WARN: /dev/video${video_nr} not found. The module may not be loaded yet." >&2
      echo "[install]       To load now + persist:" >&2
      echo "[install]         ./scripts/setup.sh --load-loopback --persist-loopback --video-nr ${video_nr}" >&2
      echo "[install]       Or:" >&2
      echo "[install]         ./scripts/setup/v4l2loopback.sh --load --persist --video-nr ${video_nr}" >&2
    fi

    # PulseAudio utils (or PipeWire pulse shim) are used for diagnostics and common setups.
    if ! have_cmd pactl; then
      echo "[install] WARN: pactl not found. Audio routing/diagnostics may be limited." >&2
      echo "[install]       Fix (Ubuntu-family): ./scripts/setup.sh --deps" >&2
    fi

    if [[ "${failures}" -ne 0 ]]; then
      echo "[install]" >&2
      echo "[install] Preflight failed. Please run the setup helper first:" >&2
      echo "[install]   ./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback" >&2
      echo "[install]" >&2
      echo "[install] If you intentionally want to proceed anyway, re-run with: --skip-system-check" >&2
      exit 2
    fi

    if [[ "${warn_only}" -ne 0 ]]; then
      echo "[install] Preflight warnings detected; continuing." >&2
    fi
  }

  if [[ "${skip_system_check}" -eq 0 ]]; then
    system_preflight
  fi

  if [[ ! -f "${build_dir}/CMakeCache.txt" ]]; then
    echo "[install] ERROR: ${build_dir} does not look like a configured CMake build dir (missing CMakeCache.txt)" >&2
    exit 2
  fi

  local -a service_args=()
  service_args+=(--build-dir "${build_dir}")
  if [[ "${no_link_bins}" -eq 1 ]]; then
    service_args+=(--no-link-bins)
  fi
  if [[ "${yes}" -eq 1 ]]; then
    service_args+=(--yes)
  fi

  local -a audio_args=()
  if [[ -n "${audio_dest}" ]]; then
    audio_args+=(--dest "${audio_dest}")
  fi
  for id in "${audio_models[@]}"; do
    audio_args+=(--model "${id}")
  done
  if [[ "${force}" -eq 1 ]]; then
    audio_args+=(--force)
  fi

  local -a video_args=()
  if [[ -n "${video_dest}" ]]; then
    video_args+=(--dest "${video_dest}")
  fi
  for id in "${video_models[@]}"; do
    video_args+=(--model "${id}")
  done
  if [[ "${force}" -eq 1 ]]; then
    video_args+=(--force)
  fi

  local -a build_targets=(
    studiocastd
    studiocastctl
    studiocast-probe
    studiocast
    studiocast-video
    studiocast-audio
    studiocast-open
  )

  if [[ "${dry_run}" -eq 1 ]]; then
    print_cmd cmake --build "${build_dir}" --target "${build_targets[@]}"
    print_cmd "${SCRIPT_DIR}/install/user_service.sh" "${service_args[@]}"
    print_cmd "${SCRIPT_DIR}/install/open_audio_models.sh" "${audio_args[@]}"
    print_cmd "${SCRIPT_DIR}/install/open_video_models.sh" "${video_args[@]}"
    exit 0
  fi

  cmake --build "${build_dir}" --target "${build_targets[@]}"

  "${SCRIPT_DIR}/install/user_service.sh" "${service_args[@]}"
  "${SCRIPT_DIR}/install/open_audio_models.sh" "${audio_args[@]}"
  "${SCRIPT_DIR}/install/open_video_models.sh" "${video_args[@]}"
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
  open-video-models)
    exec "${SCRIPT_DIR}/install/open_video_models.sh" "$@"
    ;;
  full)
    run_full "$@"
    ;;
  *)
    echo "[install] ERROR: Unknown command: $cmd" >&2
    usage
    exit 2
    ;;
esac
