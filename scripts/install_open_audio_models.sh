#!/usr/bin/env bash
set -euo pipefail

# StudioCast Open Audio model installer.
#
# Installs curated open-source model packs for the Open Audio backend.
#
# Default destination:
#   ${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_audio
#
# This script downloads model binaries (not shipped in this repo) and writes a
# model pack directory containing:
#   - model.json
#   - *.onnx
#   - LICENSE.txt

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEMPLATES_DIR="${SCRIPT_DIR}/model_packs/open_audio"

DEFAULT_DEST_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_audio"

# Curated pack(s) shipped as metadata-only templates.
MODEL_IDS=(
  "fastenhancer_s_vd_v1"
  "fastenhancer_m_vd_v1"
  "fastenhancer_l_vd_v1"
)

# Upstream download info (FastEnhancer ONNX release).
FASTENHANCER_TAG="onnx-vd-v1.0.0"
FASTENHANCER_BASE_URL="https://github.com/aask1357/fastenhancer/releases/download/${FASTENHANCER_TAG}"
FASTENHANCER_S_ASSET="fastenhancer_s.onnx"
FASTENHANCER_S_SHA256="e2d0e91bbfab4af1316bb2c41126a38c8b3cd015b93bb630d651af8fdbf7f2e8"
FASTENHANCER_M_ASSET="fastenhancer_m.onnx"
FASTENHANCER_M_SHA256="367059e724dd367c056dc906e9698ec5864c80c9a88e0597f7a2b0f81c506aaa"
FASTENHANCER_L_ASSET="fastenhancer_l.onnx"
FASTENHANCER_L_SHA256="915d4ae3871c3271e75c194e72a3ff031d5593be4f6f56ccb78df82071ef0750"

usage() {
  cat <<USAGE
StudioCast Open Audio Model Installer

Usage:
  $0 [--dest <path>] [--model <id>] [--force]

Options:
  --dest <path>   Install root for Open Audio model packs.
                 Default: ${DEFAULT_DEST_ROOT}

  --model <id>    Install a single curated model pack (repeatable).
                 If omitted, installs all curated packs.

  --force         Re-download and overwrite existing pack contents.

  --list          List curated pack IDs and exit.

  -h, --help      Show help.

After install:
  build/studiocast-open audio-list-models
  build/studiocast-open audio-self-test --model-id <id>

Notes:
  - This script does NOT require Maxine.
  - Model binaries are downloaded from upstream GitHub releases.
USAGE
}

die() {
  echo "ERROR: $*" >&2
  exit 2
}

have_cmd() { command -v "$1" >/dev/null 2>&1; }

sha256_file() {
  local f="$1"
  if have_cmd sha256sum; then
    sha256sum "$f" | awk '{print $1}'
  elif have_cmd shasum; then
    shasum -a 256 "$f" | awk '{print $1}'
  else
    die "Missing sha256sum/shasum; cannot verify downloads."
  fi
}

download_to() {
  local url="$1"
  local out="$2"

  if have_cmd curl; then
    curl -fL --retry 3 --retry-delay 1 -o "$out" "$url"
  elif have_cmd wget; then
    wget -O "$out" "$url"
  else
    die "Missing curl/wget; cannot download models."
  fi
}

install_pack_fastenhancer_variant() {
  local pack_id="$1"
  local asset="$2"
  local expected_sha256="$3"
  local dest_root="$4"
  local force="$5"

  local pack_dir="${dest_root}/${pack_id}"
  local tpl_dir="${TEMPLATES_DIR}/${pack_id}"

  if [[ ! -d "${tpl_dir}" ]]; then
    die "Template missing: ${tpl_dir}"
  fi

  mkdir -p "${pack_dir}"

  # Copy metadata files (model.json + license) from template.
  cp -f "${tpl_dir}/model.json" "${pack_dir}/model.json"
  cp -f "${tpl_dir}/LICENSE.txt" "${pack_dir}/LICENSE.txt"
  # Optional README.
  if [[ -f "${tpl_dir}/README.txt" ]]; then
    cp -f "${tpl_dir}/README.txt" "${pack_dir}/README.txt"
  fi

  local onnx_path="${pack_dir}/${asset}"
  local url="${FASTENHANCER_BASE_URL}/${asset}"

  if [[ -f "${onnx_path}" && "${force}" != "1" ]]; then
    # Verify existing file.
    local got
    got="$(sha256_file "${onnx_path}")"
    if [[ "${got}" == "${expected_sha256}" ]]; then
      echo "✓ ${pack_id}: already installed (sha256 OK)"
      return 0
    fi
    echo "⚠ ${pack_id}: existing ONNX checksum mismatch; re-downloading (use --force to silence)."
  fi

  echo "→ Downloading ${pack_id}: ${url}"
  local tmp
  tmp="$(mktemp)"
  trap 'rm -f "${tmp}"' RETURN

  download_to "${url}" "${tmp}"

  local got
  got="$(sha256_file "${tmp}")"
  if [[ "${got}" != "${expected_sha256}" ]]; then
    die "Checksum mismatch for ${asset}. Expected ${expected_sha256}, got ${got}"
  fi

  mv -f "${tmp}" "${onnx_path}"
  trap - RETURN

  echo "✓ ${pack_id}: installed"
}

main() {
  local dest_root="${DEFAULT_DEST_ROOT}"
  local force="0"
  local -a selected_models=()

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --dest)
        [[ $# -ge 2 ]] || die "--dest requires a path"
        dest_root="$2"
        shift 2
        ;;
      --model)
        [[ $# -ge 2 ]] || die "--model requires an id"
        selected_models+=("$2")
        shift 2
        ;;
      --force)
        force="1"
        shift
        ;;
      --list)
        echo "Curated Open Audio packs:"
        for id in "${MODEL_IDS[@]}"; do
          echo "  - ${id}"
        done
        exit 0
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "Unknown argument: $1 (use --help)"
        ;;
    esac
  done

  # Expand "~" manually for convenience.
  if [[ "${dest_root}" == "~"* ]]; then
    dest_root="${dest_root/#~/${HOME}}"
  fi

  mkdir -p "${dest_root}"

  local -a to_install=()
  if [[ "${#selected_models[@]}" -eq 0 ]]; then
    to_install=("${MODEL_IDS[@]}")
  else
    to_install=("${selected_models[@]}")
  fi

  echo "StudioCast Open Audio model install"
  echo "Destination: ${dest_root}"
  echo

  for id in "${to_install[@]}"; do
    case "${id}" in
      fastenhancer_s_vd_v1)
        install_pack_fastenhancer_variant "fastenhancer_s_vd_v1" "${FASTENHANCER_S_ASSET}" "${FASTENHANCER_S_SHA256}" "${dest_root}" "${force}"
        ;;
      fastenhancer_m_vd_v1)
        install_pack_fastenhancer_variant "fastenhancer_m_vd_v1" "${FASTENHANCER_M_ASSET}" "${FASTENHANCER_M_SHA256}" "${dest_root}" "${force}"
        ;;
      fastenhancer_l_vd_v1)
        install_pack_fastenhancer_variant "fastenhancer_l_vd_v1" "${FASTENHANCER_L_ASSET}" "${FASTENHANCER_L_SHA256}" "${dest_root}" "${force}"
        ;;
      *)
        die "Unknown model id '${id}'. Use --list."
        ;;
    esac
  done

  echo
  echo "Next steps:"
  echo "  1) Validate discovery:"
  echo "       build/studiocast-open audio-list-models"
  echo "  2) Validate ONNX Runtime session + IO:"
  echo "       build/studiocast-open audio-self-test --model-id fastenhancer_s_vd_v1"
  echo "       build/studiocast-open audio-self-test --model-id fastenhancer_m_vd_v1"
  echo "       build/studiocast-open audio-self-test --model-id fastenhancer_l_vd_v1"
  echo
  echo "GUI:"
  echo "  - Audio → Microphone Effects → Backend: OPEN_SOURCE or AUTO"
  echo "  - Model: Default (auto) or a FastEnhancer variant"
  echo
  echo "Daemon:"
  echo "  - Run: build/studiocastd"
  echo "  - Inspect: build/studiocastctl status"
  echo
  echo "Docs:"
  echo "  - docs/open_audio_install.md"
}

main "$@"