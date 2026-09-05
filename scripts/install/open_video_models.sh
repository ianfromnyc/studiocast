#!/usr/bin/env bash
set -euo pipefail

# StudioCast Open Video model installer.
#
# Installs metadata templates from this repo and downloads model binaries
# (ONNX / dlib assets) from a Hugging Face repo.
#
# Default destination:
#   ${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_video
#
# Layout mirrors resources/model_packs/open_video:
#   <dest>/matting/<pack_dir>/...
#   <dest>/face_detection/<pack_dir>/...
#   <dest>/face_landmarks/<pack_dir>/...
#   <dest>/eye_contact/<pack_dir>/...
#   <dest>/video_denoise/<pack_dir>/...

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TEMPLATES_DIR="${REPO_ROOT}/resources/model_packs/open_video"

DEFAULT_DEST_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/studiocast/models/open_video"

# Hosted download info (Hugging Face repo containing StudioCast-packaged model artifacts).
#
# You can override these at runtime:
#   STUDIOCAST_HF_REPO_ID=10dallasj/studiocast
#   STUDIOCAST_HF_REV=main
#   STUDIOCAST_HF_OPEN_VIDEO_PREFIX=open_video   (or empty if you uploaded to repo root)
#   STUDIOCAST_HF_OPEN_VIDEO_FALLBACK_PREFIX=open_cuda   (legacy; optional)
HF_REPO_ID="${STUDIOCAST_HF_REPO_ID:-10dallasj/studiocast}"
HF_REV="${STUDIOCAST_HF_REV:-main}"
HF_OPEN_VIDEO_PREFIX="${STUDIOCAST_HF_OPEN_VIDEO_PREFIX:-}"
HF_OPEN_VIDEO_FALLBACK_PREFIX="${STUDIOCAST_HF_OPEN_VIDEO_FALLBACK_PREFIX:-}"
HF_BASE_URL="https://huggingface.co/${HF_REPO_ID}/resolve/${HF_REV}"

usage() {
  cat <<USAGE
StudioCast Open Video Model Installer

Usage:
  $0 [--dest <path>] [--model <id>] [--force]

Options:
  --dest <path>   Install root for Open Video model packs.
                 Default: ${DEFAULT_DEST_ROOT}

  --model <id>    Install a single model pack by model.json:id (repeatable).
                 If omitted, installs all packs found under:
                   resources/model_packs/open_video/

  --force         Re-download and overwrite existing pack contents.

  --list          List discovered pack IDs and exit.

  --include-placeholders
                 Also attempt to install packs whose ids contain 'placeholder'
                 (default: skip them).

  -h, --help      Show help.

Notes:
  - Model binaries are downloaded from Hugging Face.
  - Configure the source repo via env vars:
      STUDIOCAST_HF_REPO_ID=10dallasj/studiocast
      STUDIOCAST_HF_REV=main
      STUDIOCAST_HF_OPEN_VIDEO_PREFIX=open_video   (or empty if uploaded to repo root)
      STUDIOCAST_HF_OPEN_VIDEO_FALLBACK_PREFIX=open_cuda (optional legacy)
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

urlencode_path() {
  # URL-encode each path segment but preserve '/'.
  python3 - <<'PY' "$1"
import sys
from urllib.parse import quote
p = sys.argv[1]
parts = p.split('/')
print('/'.join(quote(x, safe='') for x in parts))
PY
}

hf_join() {
  # Join path components with '/'. Empty components are skipped.
  local out=""
  local part
  for part in "$@"; do
    [[ -n "${part}" ]] || continue
    if [[ -z "${out}" ]]; then
      out="${part}"
    else
      out="${out%/}/${part#/}"
    fi
  done
  echo "${out}"
}

subject_alt_for_rel() {
  # If the relative path begins with matting/ or segmentation/, return matting/
  local rel="$1"
  if [[ "${rel}" == matting/* ]]; then
    echo "matting/${rel#matting/}"
  elif [[ "${rel}" == segmentation/* ]]; then
    echo "matting/${rel#segmentation/}"
  else
    echo ""
  fi
}

hf_url_candidates_for_pack_file() {
  # Arguments:
  #   $1: pack relative dir under templates root (e.g. 'matting/Good Quality')
  #   $2: filename within that dir (e.g. 'model.onnx')
  local rel_dir="$1"
  local filename="$2"

  local alt_rel
  alt_rel="$(subject_alt_for_rel "${rel_dir}")"

  # Try configured prefix first, then optional fallback prefix, then repo root.
  # Also try the matting<->segmentation alt path to support renames.
  local candidates=()
  local p
  for p in "${HF_OPEN_VIDEO_PREFIX}" "${HF_OPEN_VIDEO_FALLBACK_PREFIX}" ""; do
    candidates+=("$(hf_join "${p}" "${rel_dir}" "${filename}")")
    if [[ -n "${alt_rel}" ]]; then
      candidates+=("$(hf_join "${p}" "${alt_rel}" "${filename}")")
    fi
  done

  local line
  for line in "${candidates[@]}"; do
    # Skip empties (can happen if rel_dir empty, though it shouldn't).
    [[ -n "${line}" ]] || continue
    echo "${HF_BASE_URL}/$(urlencode_path "${line}")"
  done
}

python_pack_meta() {
  # Prints key=value lines:
  #   ID=...
  #   TASK=...
  #   DISPLAY=...
  #   MATTING_IN=... (empty if not a matting pack)
  #   MATTING_OUT=... (empty if not a matting pack)
  #   FILE=<name>\t<kind>\t<sha256>\t<role>
  python3 - "$1" <<'PY'
import json
import sys
from pathlib import Path

mj = Path(sys.argv[1])
d = json.loads(mj.read_text(encoding='utf-8'))

schema = int(d.get('schema_version') or 1)
id_ = str(d.get('id') or '').strip()
task = str(d.get('task') or '').strip()
display = str(d.get('display_name') or d.get('name') or d.get('title') or '').strip()

mat_in = ''
mat_out = ''
if task == 'matting':
    try:
        mat_in = str(d.get('input', {}).get('name') or '').strip()
        mat_out = str(d.get('output', {}).get('name') or '').strip()
    except Exception:
        pass

files = []
if schema >= 2:
    for f in d.get('files', []) or []:
        name = str(f.get('name') or '').strip()
        kind = str(f.get('kind') or '').strip()
        sha = str(f.get('sha256') or '').strip()
        role = str(f.get('role') or '').strip()
        if name:
            files.append((name, kind, sha, role))
else:
    # Legacy schema v1 matting packs.
    name = str(d.get('onnx_filename') or '').strip()
    if name:
        files.append((name, 'onnx', '', ''))

print(f"ID={id_}")
print(f"TASK={task}")
print(f"DISPLAY={display}")
print(f"MATTING_IN={mat_in}")
print(f"MATTING_OUT={mat_out}")
for name, kind, sha, role in files:
    print(f"FILE={name}\t{kind}\t{sha}\t{role}")
PY
}

copy_template_metadata() {
  local tpl_dir="$1"
  local dst_dir="$2"

  mkdir -p "${dst_dir}"

  # Copy all regular files from the template directory except model binaries.
  # (We expect binaries to be downloaded from HF.)
  local f
  while IFS= read -r -d '' f; do
    local base
    base="$(basename "${f}")"
    case "${base}" in
      *.onnx|*.onnx.gz|*.dat)
        continue
        ;;
    esac
    cp -f "${f}" "${dst_dir}/${base}"
  done < <(find "${tpl_dir}" -maxdepth 1 -type f -print0)
}

verify_onnx_io_names() {
  local onnx_path="$1"
  local expected_in="$2"
  local expected_out="$3"

  [[ -n "${expected_in}" && -n "${expected_out}" ]] || return 0

  python3 - "${onnx_path}" "${expected_in}" "${expected_out}" <<'PY' >/dev/null
import sys
try:
    import onnx
except Exception as e:
    print(
        f"WARN: cannot import onnx python package; skipping IO-name validation for {sys.argv[1]} ({e})",
        file=sys.stderr,
    )
    raise SystemExit(0)

m = onnx.load(sys.argv[1])
ins = [i.name for i in m.graph.input]
outs = [o.name for o in m.graph.output]
if not ins or not outs:
    raise SystemExit(f"ONNX graph has no inputs/outputs: {sys.argv[1]}")
if ins[0] != sys.argv[2]:
    raise SystemExit(f"Unexpected ONNX input name: got '{ins[0]}', expected '{sys.argv[2]}'")
if outs[0] != sys.argv[3]:
    raise SystemExit(f"Unexpected ONNX output name: got '{outs[0]}', expected '{sys.argv[3]}'")
PY
}

patch_birefnet_output_to_alpha() {
  # Some BiRefNet exports expose logits with a non-'alpha' output name.
  # StudioCast matting expects a single alpha output in [0..1].
  # This rewrite:
  #   - adds Sigmoid(logits) -> alpha
  #   - updates graph output to alpha
  local in_onnx="$1"
  local out_onnx="$2"
  local expected_out_name="$3"  # typically 'alpha'

  python3 - <<'PY' "${in_onnx}" "${out_onnx}" "${expected_out_name}"
import sys
try:
    import onnx
    from onnx import helper, TensorProto
except Exception as e:
    print("ERROR: Missing python dependency: onnx. Install it with:")
    print("  python3 -m pip install --user onnx")
    raise

in_path = sys.argv[1]
out_path = sys.argv[2]
expected = sys.argv[3]

m = onnx.load(in_path)
g = m.graph

if len(g.output) != 1:
    raise SystemExit(f"Expected exactly 1 output, got {len(g.output)}")

old_out = g.output[0]
old_name = old_out.name

# If it already matches, keep as-is.
if old_name == expected:
    onnx.save(m, out_path)
    raise SystemExit(0)

# Create a new output value info with the same type/shape.
new_out = helper.make_tensor_value_info(
    expected,
    old_out.type.tensor_type.elem_type or TensorProto.FLOAT,
    [d.dim_value if d.dim_value > 0 else None for d in old_out.type.tensor_type.shape.dim],
)

# Add Sigmoid node.
node = helper.make_node(
    "Sigmoid",
    inputs=[old_name],
    outputs=[expected],
    name="studiocast_sigmoid_alpha",
)

# Append node and replace graph outputs.
g.node.append(node)
del g.output[:]
g.output.append(new_out)

onnx.checker.check_model(m)
onnx.save(m, out_path)
PY
}

is_placeholder_id() {
  local id="$1"
  [[ "${id}" == *placeholder* ]]
}

install_pack_from_template() {
  local tpl_dir="$1"         # absolute path to pack dir in templates
  local rel_dir="$2"         # rel path under templates root (subject/pack_dir)
  local dest_root="$3"
  local force="$4"

  local meta
  meta="$(python_pack_meta "${tpl_dir}/model.json")"

  local id=""
  local task=""
  local display=""
  local mat_in=""
  local mat_out=""
  local -a file_lines=()

  local line
  while IFS= read -r line; do
    case "${line}" in
      ID=*) id="${line#ID=}" ;;
      TASK=*) task="${line#TASK=}" ;;
      DISPLAY=*) display="${line#DISPLAY=}" ;;
      MATTING_IN=*) mat_in="${line#MATTING_IN=}" ;;
      MATTING_OUT=*) mat_out="${line#MATTING_OUT=}" ;;
      FILE=*) file_lines+=("${line#FILE=}") ;;
    esac
  done <<< "${meta}"

  if [[ -z "${id}" || -z "${task}" ]]; then
    die "Invalid model.json in ${tpl_dir} (missing id/task)"
  fi

  local dest_dir="${dest_root}/${rel_dir}"
  mkdir -p "${dest_dir}"

  # Copy template metadata (LICENSE/model.json/etc.).
  copy_template_metadata "${tpl_dir}" "${dest_dir}"

  if [[ ${#file_lines[@]} -eq 0 ]]; then
    echo "⚠ ${id}: no files listed in model.json; installed metadata only (${rel_dir})"
    return 0
  fi

  local fline
  for fline in "${file_lines[@]}"; do
    local name kind sha role
    # A record has four fields. Name them all, so that the layout of the line
    # stays readable, although this loop reads the name and the checksum only.
    # shellcheck disable=SC2034  # kind and role name the fields they hold
    IFS=$'\t' read -r name kind sha role <<< "${fline}"

    [[ -n "${name}" ]] || continue

    local dst_file="${dest_dir}/${name}"

    if [[ -f "${dst_file}" && "${force}" != "1" ]]; then
      if [[ -n "${sha}" ]]; then
        local got
        got="$(sha256_file "${dst_file}")"
        if [[ "${got}" == "${sha}" ]]; then
          echo "✓ ${id}: ${name} already installed (sha256 OK)"
          continue
        fi
        echo "⚠ ${id}: existing ${name} checksum mismatch; re-downloading (use --force to silence)."
      else
        echo "✓ ${id}: ${name} already present (no sha256 in model.json to verify)"
        continue
      fi
    fi

    local url_candidates
    url_candidates="$(hf_url_candidates_for_pack_file "${rel_dir}" "${name}")"

    echo "→ Downloading ${id}/${name} from HF repo: ${HF_REPO_ID}"
    local tmp
    tmp="$(mktemp)"
    trap 'rm -f "${tmp}"' RETURN

    local ok="0"
    while IFS= read -r url; do
      [[ -n "${url}" ]] || continue
      if download_to "${url}" "${tmp}"; then
        ok="1"
        break
      fi
      echo "⚠ download failed: ${url}" >&2
    done <<< "${url_candidates}"

    if [[ "${ok}" != "1" ]]; then
      die "Failed to download ${rel_dir}/${name} from HF repo ${HF_REPO_ID}.\nTried:\n${url_candidates}"
    fi

    # Verify checksum if provided.
    if [[ -n "${sha}" ]]; then
      local got
      got="$(sha256_file "${tmp}")"
      if [[ "${got}" != "${sha}" ]]; then
        die "Checksum mismatch for ${id}/${name}. Expected ${sha}, got ${got}"
      fi
    else
      echo "⚠ ${id}/${name}: no sha256 in model.json; download not verified"
    fi

    mkdir -p "$(dirname "${dst_file}")"

    # Special-case: BiRefNet matting packs need a graph rewrite to expose alpha.
    if [[ "${task}" == "matting" && ("${id}" == "birefnet_lite" || "${id}" == "birefnet_portrait") && "${name}" == *.onnx ]]; then
      local patched
      patched="$(mktemp)"
      patch_birefnet_output_to_alpha "${tmp}" "${patched}" "${mat_out:-alpha}"
      mv -f "${patched}" "${dst_file}"
    else
      mv -f "${tmp}" "${dst_file}"
    fi

    trap - RETURN

    # Best-effort IO validation for matting packs (helps catch mismatched exports).
    if [[ "${task}" == "matting" && "${name}" == *.onnx ]]; then
      verify_onnx_io_names "${dst_file}" "${mat_in}" "${mat_out}"
    fi

    echo "✓ ${id}: installed ${name}"
  done

  # Basic summary line.
  if [[ -n "${display}" ]]; then
    echo "✓ Installed pack: ${id} (${task}) — ${display}"
  else
    echo "✓ Installed pack: ${id} (${task})"
  fi
}

list_discovered_packs() {
  local include_placeholders="$1"

  if [[ ! -d "${TEMPLATES_DIR}" ]]; then
    die "Templates directory missing: ${TEMPLATES_DIR}"
  fi

  echo "Discovered Open Video packs in: ${TEMPLATES_DIR}"

  local mj
  while IFS= read -r -d '' mj; do
    local tpl_dir
    tpl_dir="$(dirname "${mj}")"
    local rel_dir
    rel_dir="${tpl_dir#"${TEMPLATES_DIR}/"}"

    local meta
    meta="$(python_pack_meta "${mj}")"

    local id="" task="" display=""
    local line
    while IFS= read -r line; do
      case "${line}" in
        ID=*) id="${line#ID=}" ;;
        TASK=*) task="${line#TASK=}" ;;
        DISPLAY=*) display="${line#DISPLAY=}" ;;
      esac
    done <<< "${meta}"

    [[ -n "${id}" ]] || continue

    if is_placeholder_id "${id}" && [[ "${include_placeholders}" != "1" ]]; then
      printf '  - %s (%s)  [%s]  (skipped by default: placeholder)\n' "${id}" "${task}" "${rel_dir}"
      continue
    fi

    if [[ -n "${display}" ]]; then
      printf '  - %s (%s)  [%s]  — %s\n' "${id}" "${task}" "${rel_dir}" "${display}"
    else
      printf '  - %s (%s)  [%s]\n' "${id}" "${task}" "${rel_dir}"
    fi
  done < <(find "${TEMPLATES_DIR}" -type f -name model.json -print0 | sort -z)
}

main() {
  local dest_root="${DEFAULT_DEST_ROOT}"
  local force="0"
  local include_placeholders="0"
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
      --include-placeholders)
        include_placeholders="1"
        shift
        ;;
      --list)
        list_discovered_packs "${include_placeholders}"
        return 0
        ;;
      -h|--help)
        usage
        return 0
        ;;
      *)
        die "Unknown arg: $1 (use --help)"
        ;;
    esac
  done

  if [[ ! -d "${TEMPLATES_DIR}" ]]; then
    die "Templates directory missing: ${TEMPLATES_DIR}"
  fi

  mkdir -p "${dest_root}"

  # Install packs.
  local mj
  local installed_any="0"

  while IFS= read -r -d '' mj; do
    local tpl_dir
    tpl_dir="$(dirname "${mj}")"
    local rel_dir
    rel_dir="${tpl_dir#"${TEMPLATES_DIR}/"}"

    local meta
    meta="$(python_pack_meta "${mj}")"

    local id="" task=""
    local line
    while IFS= read -r line; do
      case "${line}" in
        ID=*) id="${line#ID=}" ;;
        TASK=*) task="${line#TASK=}" ;;
      esac
    done <<< "${meta}"

    [[ -n "${id}" ]] || continue

    if is_placeholder_id "${id}" && [[ "${include_placeholders}" != "1" ]]; then
      echo "→ Skipping placeholder pack: ${id} (${rel_dir})"
      continue
    fi

    if [[ ${#selected_models[@]} -gt 0 ]]; then
      local match="0"
      local want
      for want in "${selected_models[@]}"; do
        if [[ "${want}" == "${id}" ]]; then
          match="1"
          break
        fi
      done
      if [[ "${match}" != "1" ]]; then
        continue
      fi
    fi

    install_pack_from_template "${tpl_dir}" "${rel_dir}" "${dest_root}" "${force}"
    installed_any="1"
  done < <(find "${TEMPLATES_DIR}" -type f -name model.json -print0 | sort -z)

  if [[ "${installed_any}" != "1" ]]; then
    if [[ ${#selected_models[@]} -gt 0 ]]; then
      die "No packs matched the requested --model id(s). Use --list to see available ids."
    fi
    die "No packs discovered under ${TEMPLATES_DIR}."
  fi

  echo ""
  echo "Done. Installed model packs under: ${dest_root}"
}

main "$@"
