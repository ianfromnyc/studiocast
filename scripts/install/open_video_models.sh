#!/usr/bin/env bash
set -euo pipefail

# StudioCast Open Video model installer.
#
# Installs curated open-source model packs for the Open Video backend.
#
# Default destination:
#   ${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_video
#
# This script downloads model binaries (not shipped in this repo) and writes a
# model pack directory containing:
#   - model.json
#   - model.onnx
#   - LICENSE.txt
#
# NOTE:
#   Some upstream ONNX exports don't match StudioCast's expected output tensor
#   names. For BiRefNet packs we apply a small deterministic ONNX graph patch
#   so the output tensor is named "alpha" and corresponds to sigmoid(logits).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TEMPLATES_DIR="${REPO_ROOT}/resources/model_packs/open_video"

DEFAULT_DEST_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_video"

# Curated pack IDs (correspond to model.json:id).
MODEL_IDS=(
  "modnet-webnn-256-fp32"
  "birefnet_lite"
  "birefnet_portrait"
)

# Upstream download info (Hugging Face direct downloads).
MODNET_URL="https://huggingface.co/onnx-community/modnet-webnn/resolve/main/onnx/model.onnx"
MODNET_SHA256="07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9"

BIREFNET_LITE_URL="https://huggingface.co/onnx-community/BiRefNet_lite-ONNX/resolve/main/onnx/model.onnx"
BIREFNET_LITE_SHA256="5600024376f572a557870a5eb0afb1e5961636bef4e1e22132025467d0f03333"

BIREFNET_PORTRAIT_URL="https://huggingface.co/onnx-community/BiRefNet-portrait-ONNX/resolve/main/onnx/model.onnx"
BIREFNET_PORTRAIT_SHA256="1ba1c8ff5a7bbfadc8d8d13fb11d7be793f91f23d9d466549e37a854f6668f99"

usage() {
  cat <<USAGE
StudioCast Open Video Model Installer

Installs curated Open Video (matting/segmentation) model packs under:
  ${DEFAULT_DEST_ROOT}

Usage:
  $0 [--dest <path>] [--model <id>] [--force]

Options:
  --dest <path>   Install root for Open Video model packs.
                 Default: ${DEFAULT_DEST_ROOT}

  --model <id>    Install a single curated model pack (repeatable).
                 If omitted, installs all curated packs.

  --force         Re-download and overwrite existing pack contents.

  --list          List curated pack IDs and exit.

  -h, --help      Show help.

After install:
  build/studiocast-open video-list-models --task matting
  build/studiocast-open video-self-test --task matting --cpu-only

Notes:
  - Model binaries are downloaded from upstream Hugging Face model repos.
  - BiRefNet models are patched so the output tensor is named "alpha".
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

python_check_onnx_import() {
  python3 -c 'import onnx' >/dev/null 2>&1 || die "python3 + onnx package is required (try: python3 -m pip install --user onnx)"
}

verify_onnx_io_names() {
  local onnx_path="$1"
  local expected_in="$2"
  local expected_out="$3"

  python3 - "$onnx_path" "$expected_in" "$expected_out" <<'PY'
import sys

import onnx

path, want_in, want_out = sys.argv[1], sys.argv[2], sys.argv[3]
m = onnx.load(path)

ins = [i.name for i in m.graph.input]
outs = [o.name for o in m.graph.output]

if not ins or not outs:
    print("model must have at least 1 input and 1 output", file=sys.stderr)
    raise SystemExit(2)

got_in = ins[0]
got_out = outs[0]
if want_in and got_in != want_in:
    print(f"input name mismatch: model='{got_in}' expected='{want_in}'", file=sys.stderr)
    raise SystemExit(3)
if want_out and got_out != want_out:
    print(f"output name mismatch: model='{got_out}' expected='{want_out}'", file=sys.stderr)
    raise SystemExit(4)
raise SystemExit(0)
PY
}

patch_output_sigmoid_alpha() {
  local in_onnx="$1"
  local out_onnx="$2"
  local expected_out_name="$3"

  python_check_onnx_import

  python3 - "$in_onnx" "$out_onnx" "$expected_out_name" <<'PY'
import sys

import onnx
from onnx import helper

in_path, out_path, want_out = sys.argv[1], sys.argv[2], sys.argv[3]
m = onnx.load(in_path)
g = m.graph

if len(g.output) < 1:
    print("model has no graph outputs", file=sys.stderr)
    raise SystemExit(2)

# If output already matches, just ensure it's the only output.
if g.output[0].name == want_out and len(g.output) == 1:
    onnx.checker.check_model(m)
    onnx.save(m, out_path)
    raise SystemExit(0)

old_out_vi = g.output[0]
old_out_name = old_out_vi.name

if old_out_name == want_out:
    # Output name is correct, but there are multiple outputs.
    keep = old_out_vi
    del g.output[:]
    g.output.extend([keep])
    onnx.checker.check_model(m)
    onnx.save(m, out_path)
    raise SystemExit(0)

# Avoid name collisions.
existing = set([i.name for i in g.input] + [o.name for o in g.output] +
               [v.name for v in g.value_info] + [t.name for t in g.initializer])
if want_out in existing:
    print(f"cannot patch: tensor name '{want_out}' already exists in graph", file=sys.stderr)
    raise SystemExit(3)

node_name = "studiocast_sigmoid_alpha"
sig = helper.make_node("Sigmoid", inputs=[old_out_name], outputs=[want_out], name=node_name)
g.node.append(sig)

new_out_vi = onnx.ValueInfoProto()
new_out_vi.CopyFrom(old_out_vi)
new_out_vi.name = want_out

del g.output[:]
g.output.extend([new_out_vi])

onnx.checker.check_model(m)
onnx.save(m, out_path)
PY
}

copy_template_metadata() {
  local tpl_dir="$1"
  local pack_dir="$2"

  cp -f "${tpl_dir}/model.json" "${pack_dir}/model.json"
  cp -f "${tpl_dir}/LICENSE.txt" "${pack_dir}/LICENSE.txt"

  if [[ -f "${tpl_dir}/README.txt" ]]; then
    cp -f "${tpl_dir}/README.txt" "${pack_dir}/README.txt"
  fi
  if [[ -f "${tpl_dir}/README.md" ]]; then
    cp -f "${tpl_dir}/README.md" "${pack_dir}/README.md"
  fi
}

install_pack_matting() {
  local pack_id="$1"
  local tpl_rel="$2"
  local url="$3"
  local expected_sha256="$4"
  local expected_in_name="$5"
  local expected_out_name="$6"
  local needs_alpha_patch="$7"
  local dest_root="$8"
  local force="$9"

  local tpl_dir="${TEMPLATES_DIR}/${tpl_rel}"
  if [[ ! -d "${tpl_dir}" ]]; then
    die "Template missing: ${tpl_dir}"
  fi

  local pack_dir="${dest_root}/${tpl_rel}"
  mkdir -p "${pack_dir}"

  copy_template_metadata "${tpl_dir}" "${pack_dir}"

  local onnx_path="${pack_dir}/model.onnx"

  if [[ -f "${onnx_path}" && "${force}" != "1" ]]; then
    if verify_onnx_io_names "${onnx_path}" "${expected_in_name}" "${expected_out_name}" >/dev/null 2>&1; then
      echo "✓ ${pack_id}: already installed (IO names OK)"
      return 0
    fi
    echo "⚠ ${pack_id}: existing model.onnx does not match expected IO; re-installing (use --force to silence)."
  fi

  echo "→ Downloading ${pack_id}: ${url}"
  local tmp_in
  local tmp_out
  tmp_in="$(mktemp)"
  tmp_out="$(mktemp)"
  trap 'rm -f "${tmp_in}" "${tmp_out}"' RETURN

  download_to "${url}" "${tmp_in}"
  local got
  got="$(sha256_file "${tmp_in}")"
  if [[ "${got}" != "${expected_sha256}" ]]; then
    die "Checksum mismatch for ${pack_id}. Expected ${expected_sha256}, got ${got}"
  fi

  if [[ "${needs_alpha_patch}" == "1" ]]; then
    patch_output_sigmoid_alpha "${tmp_in}" "${tmp_out}" "${expected_out_name}"
    mv -f "${tmp_out}" "${onnx_path}"
    rm -f "${tmp_in}"
  else
    mv -f "${tmp_in}" "${onnx_path}"
    rm -f "${tmp_out}"
  fi

  trap - RETURN

  # Validate final file matches the manifest expectations.
  if ! verify_onnx_io_names "${onnx_path}" "${expected_in_name}" "${expected_out_name}" >/dev/null; then
    die "${pack_id}: installed model.onnx failed IO validation (see stderr above)"
  fi

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
        echo "Curated Open Video packs:"
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

  echo "StudioCast Open Video model install"
  echo "Destination: ${dest_root}"
  echo

  for id in "${to_install[@]}"; do
    case "${id}" in
      modnet-webnn-256-fp32)
        install_pack_matting \
          "modnet-webnn-256-fp32" \
          "segmentation/Good Quality" \
          "${MODNET_URL}" \
          "${MODNET_SHA256}" \
          "input" \
          "output" \
          "0" \
          "${dest_root}" \
          "${force}"
        ;;
      birefnet_lite)
        install_pack_matting \
          "birefnet_lite" \
          "segmentation/Better Quality" \
          "${BIREFNET_LITE_URL}" \
          "${BIREFNET_LITE_SHA256}" \
          "input_image" \
          "alpha" \
          "1" \
          "${dest_root}" \
          "${force}"
        ;;
      birefnet_portrait)
        install_pack_matting \
          "birefnet_portrait" \
          "segmentation/Best Quality" \
          "${BIREFNET_PORTRAIT_URL}" \
          "${BIREFNET_PORTRAIT_SHA256}" \
          "input_image" \
          "alpha" \
          "1" \
          "${dest_root}" \
          "${force}"
        ;;
      *)
        die "Unknown model id '${id}'. Use --list."
        ;;
    esac
  done

  echo
  echo "Next steps:"
  echo "  1) Validate discovery:"
  echo "       build/studiocast-open video-list-models --task matting"
  echo "  2) Validate ONNX Runtime session + IO (CPU-only is fine):"
  echo "       build/studiocast-open video-self-test --task matting --cpu-only"
  echo
  echo "GUI:"
  echo "  - Video → Virtual Background → Engine: Open CUDA or Auto"
  echo "  - Model: Default (auto) or a specific pack"
  echo
  echo "Docs:"
  echo "  - docs/open_cuda_install.md"
  echo "  - docs/open_video_model_conversion.md"
}

main "$@"
