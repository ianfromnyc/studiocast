# shellcheck shell=bash
#
# Shared NGC (NVIDIA GPU Cloud) REST helper for the StudioCast setup scripts.
#
# NGC keeps the Maxine SDK bundles under "resources" and the feature packs
# (models plus the feature libraries) under "models". Source this file, then:
#
#   sc_ngc_list_versions          <resource>
#   sc_ngc_latest_version         <resource>
#   sc_ngc_list_files             <resource> <version>
#   sc_ngc_download_file          <resource> <version> <file> <dest> [sha256hex] [size]
#
#   sc_ngc_list_model_versions    <model>
#   sc_ngc_list_model_files       <model> <version>
#   sc_ngc_download_model_file    <model> <version> <file> <dest> [sha256hex] [size]
#   sc_ngc_download_model_version <model> <version> <dest dir>
#
# The helper speaks to the public NGC REST API with curl and parses the answers
# with python3. It does not need the "ngc" command line tool.
#
# Authentication:
#   A modern personal key starts with "nvapi-" and is a bearer token itself.
#   An older key is not. For such a key the helper gets a JWT from
#   authn.nvidia.com and sends that instead. Every request goes through
#   sc_ngc_ensure_token first, so a download works as the first call of a run.
#   A GET that still gets a 401 with a raw older key retries the exchange once.
#
# The caller can set these before it calls a function:
#   SC_NGC_API_KEY        Key to use. Default: $NGC_API_KEY, then $NGC_CLI_API_KEY.
#   SC_NGC_ORG            NGC org. Default: nvidia.
#   SC_NGC_TEAM           NGC team. Default: maxine.
#   SC_NGC_DRY_RUN        1 prints the download commands instead of running them.
#   SC_NGC_ALT_RESOURCES  Resource names printed when NGC answers 402.
#   sc_ngc_log            Log function. The default prints "[ngc] <message>".
#
# The key is never printed. Every command this file logs shows the
# Authorization header as "Bearer <redacted>".
#
# Every function returns 0 on success and 2 on failure. No function calls exit,
# so the caller keeps control of the exit code.

if [[ -n "${STUDIOCAST_NGC_LIB_SOURCED:-}" ]]; then
  return 0
fi
STUDIOCAST_NGC_LIB_SOURCED=1

SC_NGC_HOST="${SC_NGC_HOST:-https://api.ngc.nvidia.com}"
SC_NGC_AUTHN_HOST="${SC_NGC_AUTHN_HOST:-https://authn.nvidia.com}"
SC_NGC_CATALOG_HOST="${SC_NGC_CATALOG_HOST:-https://catalog.ngc.nvidia.com}"
SC_NGC_ORG="${SC_NGC_ORG:-nvidia}"
SC_NGC_TEAM="${SC_NGC_TEAM:-maxine}"
SC_NGC_DRY_RUN="${SC_NGC_DRY_RUN:-0}"
SC_NGC_RETRIES="${SC_NGC_RETRIES:-3}"

# Resource names the caller wants listed when NGC answers 402.
if ! declare -p SC_NGC_ALT_RESOURCES >/dev/null 2>&1; then
  declare -a SC_NGC_ALT_RESOURCES=()
fi

# Bearer token in use, and where it came from ("key" or "jwt").
_SC_NGC_TOKEN=""
_SC_NGC_TOKEN_KIND=""

if ! declare -F sc_ngc_log >/dev/null 2>&1; then
  sc_ngc_log() {
    echo "[ngc] $*"
  }
fi

if ! declare -F sc_ngc_err >/dev/null 2>&1; then
  sc_ngc_err() {
    echo "[ngc] ERROR: $*" >&2
  }
fi

# Every external command the functions below run. Keep this list complete: a
# tool that is absent must be named here, not found in the middle of a
# download.
_SC_NGC_TOOLS=(
  curl python3
  awk cut dirname head md5sum mktemp sha256sum stat tail
  find
)

# Fail early when a tool this file needs is absent.
sc_ngc_require_tools() {
  local -a missing=()
  local tool
  for tool in "${_SC_NGC_TOOLS[@]}"; do
    command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
  done

  if [[ "${#missing[@]}" -gt 0 ]]; then
    sc_ngc_err "missing required tool(s): ${missing[*]}"
    sc_ngc_err "Install them, for example:"
    sc_ngc_err "  Fedora: sudo dnf install curl python3 coreutils gawk findutils"
    sc_ngc_err "  Ubuntu: sudo apt-get install curl python3 coreutils gawk findutils"
    return 2
  fi
}

# Print the API key, or return 1 when there is none.
sc_ngc_api_key() {
  local key="${SC_NGC_API_KEY:-${NGC_API_KEY:-${NGC_CLI_API_KEY:-}}}"
  [[ -n "${key}" ]] || return 1
  printf '%s' "${key}"
}

sc_ngc_have_key() {
  sc_ngc_api_key >/dev/null 2>&1
}

# NGC keeps SDK bundles under "resources" and feature packs under "models".
# Both kinds use the same URL shape, so one set of functions serves both.
sc_ngc_kind_url() {
  printf '%s/v2/org/%s/team/%s/%s/%s' \
    "${SC_NGC_HOST}" "${SC_NGC_ORG}" "${SC_NGC_TEAM}" "$1" "$2"
}

sc_ngc_resource_url() {
  sc_ngc_kind_url resources "$1"
}

sc_ngc_model_url() {
  sc_ngc_kind_url models "$1"
}

sc_ngc_catalog_url() {
  printf '%s/orgs/%s/teams/%s/%s/%s' \
    "${SC_NGC_CATALOG_HOST}" "${SC_NGC_ORG}" "${SC_NGC_TEAM}" "${2:-resources}" "$1"
}

# Percent-encode one file path so it can go into a URL.
sc_ngc_url_encode_path() {
  python3 - "$1" <<'PY'
import sys
from urllib.parse import quote
print(quote(sys.argv[1], safe="/"))
PY
}

sc_ngc_human_bytes() {
  python3 - "$1" <<'PY'
import sys
try:
    n = float(sys.argv[1])
except ValueError:
    print(sys.argv[1])
    raise SystemExit(0)
for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
    if n < 1024 or unit == "TiB":
        print(f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B")
        break
    n /= 1024
PY
}

# Print one curl command with the key removed, so a log or a dry run is safe.
sc_ngc_redacted_cmd() {
  local part
  local out=""
  for part in "$@"; do
    if [[ "${part}" == Authorization:* ]]; then
      part="Authorization: Bearer <redacted>"
    fi
    out+=" $(printf '%q' "${part}")"
  done
  printf 'curl%s\n' "${out}"
}

# GET one URL with the current token. Prints the HTTP status code.
_sc_ngc_curl_status() {
  local url="$1"
  local out="$2"
  local status

  status="$(curl --silent --show-error --location \
    --retry "${SC_NGC_RETRIES}" --retry-delay 2 --retry-connrefused \
    --header "Authorization: Bearer ${_SC_NGC_TOKEN}" \
    --header "Accept: application/json" \
    --output "${out}" --write-out '%{http_code}' \
    "${url}")" || status="000"

  printf '%s' "${status:-000}"
}

# Swap an older key for a JWT. Modern "nvapi-" keys do not work here.
_sc_ngc_exchange_token() {
  local key="$1"
  local url="${SC_NGC_AUTHN_HOST}/token?service=ngc&scope=group/ngc:${SC_NGC_ORG}"
  local out status token

  out="$(mktemp)"
  status="$(curl --silent --show-error --location \
    --retry "${SC_NGC_RETRIES}" --retry-delay 2 \
    --user "\$oauthtoken:${key}" \
    --header "Accept: application/json" \
    --output "${out}" --write-out '%{http_code}' \
    "${url}" 2>/dev/null)" || status="000"

  if [[ "${status}" != 2* ]]; then
    rm -f "${out}"
    return 1
  fi

  token="$(python3 - "${out}" <<'PY'
import json, sys
try:
    with open(sys.argv[1], "rb") as fh:
        print(json.load(fh).get("token", ""))
except Exception:
    print("")
PY
)"
  rm -f "${out}"

  [[ -n "${token}" ]] || return 1
  _SC_NGC_TOKEN="${token}"
  _SC_NGC_TOKEN_KIND="jwt"
  return 0
}

# Put a bearer token for the current API key in _SC_NGC_TOKEN.
#
# A modern "nvapi-" key is a bearer token itself, so it is used as it stands.
# An older key is not, so it goes through the authn.nvidia.com exchange here.
# Every path that talks to NGC calls this first, which is why a download works
# as the first call of a run.
#
# Returns 2 when there is no key. A failed exchange is not fatal: the raw key
# stays in place, and the caller reports the status NGC answers with.
sc_ngc_ensure_token() {
  local key

  if ! key="$(sc_ngc_api_key)"; then
    sc_ngc_err "no NGC API key. Export NGC_API_KEY (or NGC_CLI_API_KEY) first."
    return 2
  fi

  [[ -z "${_SC_NGC_TOKEN}" ]] || return 0

  _SC_NGC_TOKEN="${key}"
  _SC_NGC_TOKEN_KIND="key"

  if [[ "${key}" != nvapi-* ]]; then
    # This line goes to stderr, because a caller may read the answer of the
    # function that called this one through a command substitution.
    sc_ngc_log "The NGC key is not an 'nvapi-' key. Asking authn.nvidia.com for a token..." >&2
    _sc_ngc_exchange_token "${key}" || true
  fi

  return 0
}

# List the other names the caller knows for the same component.
_sc_ngc_print_alternates() {
  local name

  [[ "${#SC_NGC_ALT_RESOURCES[@]}" -gt 0 ]] || return 0

  sc_ngc_err "Alternate names for this component:"
  for name in "${SC_NGC_ALT_RESOURCES[@]}"; do
    sc_ngc_err "  ${name}"
  done
}

# Explain one HTTP status in words the user can act on.
_sc_ngc_explain_status() {
  local status="$1"
  local resource="$2"
  local url="$3"

  case "${status}" in
    000)
      sc_ngc_err "could not reach ${SC_NGC_HOST}. Check the network and try again."
      ;;
    401)
      sc_ngc_err "NGC rejected the API key (401)."
      sc_ngc_err "Set a valid key: export NGC_API_KEY=... (get one at https://ngc.nvidia.com -> Setup -> API key)."
      ;;
    402)
      sc_ngc_err "this NGC account has no entitlement for '${resource}' (402 Payment Required)."
      sc_ngc_err "See $(sc_ngc_catalog_url "${resource}") and request access there."
      _sc_ngc_print_alternates
      ;;
    403)
      sc_ngc_err "NGC refused the request for '${resource}' (403 Forbidden)."
      sc_ngc_err "Either there is no such name in ${SC_NGC_ORG}/${SC_NGC_TEAM}, or the key cannot read it."
      _sc_ngc_print_alternates
      ;;
    404)
      sc_ngc_err "NGC knows no such resource or version (404): ${url}"
      sc_ngc_err "Check the resource name and the version, or list versions first."
      ;;
    *)
      sc_ngc_err "NGC answered HTTP ${status} for ${url}"
      ;;
  esac
}

# GET one JSON document into a file.
# Arguments: <url> <out file> [resource name for the error text]
sc_ngc_api_get() {
  local url="$1"
  local out="$2"
  local resource="${3:-}"
  local key status

  sc_ngc_ensure_token || return 2
  key="$(sc_ngc_api_key)"

  # These two lines go to stderr, because a caller may read the answer of this
  # function through a command substitution.
  if [[ "${SC_NGC_DRY_RUN}" == "1" ]]; then
    sc_ngc_log "GET $(sc_ngc_redacted_cmd --silent --header "Authorization: x" "${url}")" >&2
  fi

  status="$(_sc_ngc_curl_status "${url}" "${out}")"

  # An older key is not a bearer token. Swap it for a JWT and try once more.
  if [[ "${status}" == "401" && "${_SC_NGC_TOKEN_KIND}" == "key" && "${key}" != nvapi-* ]]; then
    sc_ngc_log "NGC rejected the raw key (401). Trying the authn.nvidia.com token exchange..." >&2
    if _sc_ngc_exchange_token "${key}"; then
      status="$(_sc_ngc_curl_status "${url}" "${out}")"
    fi
  fi

  if [[ "${status}" == 2* ]]; then
    return 0
  fi

  _sc_ngc_explain_status "${status}" "${resource}" "${url}"
  return 2
}

# Print "<version>\t<status>\t<size in bytes>" for every version of an item.
# Arguments: <kind> <name>, where kind is "resources" or "models".
sc_ngc_list_kind_versions() {
  local kind="$1"
  local name="$2"
  local url out
  local rc=0

  url="$(sc_ngc_kind_url "${kind}" "${name}")/versions?page-size=200"
  out="$(mktemp)"

  if ! sc_ngc_api_get "${url}" "${out}" "${name}"; then
    rm -f "${out}"
    return 2
  fi

  python3 - "${out}" <<'PY' || rc=$?
import json, sys
with open(sys.argv[1], "rb") as fh:
    doc = json.load(fh)
for key in ("recipeVersions", "modelVersions"):
    for v in doc.get(key) or []:
        print("\t".join([
            str(v.get("versionId", "")),
            str(v.get("status", "")),
            str(v.get("totalSizeInBytes", "")),
        ]))
PY
  rm -f "${out}"

  if [[ "${rc}" -ne 0 ]]; then
    sc_ngc_err "could not read the version list for '${name}'."
    return 2
  fi
}

sc_ngc_list_versions() {
  sc_ngc_list_kind_versions resources "$1"
}

sc_ngc_list_model_versions() {
  sc_ngc_list_kind_versions models "$1"
}

# Print the newest version of a resource that finished uploading.
sc_ngc_latest_version() {
  local resource="$1"
  local url out version
  local rc=0

  url="$(sc_ngc_resource_url "${resource}")/versions"
  out="$(mktemp)"

  if ! sc_ngc_api_get "${url}" "${out}" "${resource}"; then
    rm -f "${out}"
    return 2
  fi

  version="$(python3 - "${out}" <<'PY'
import json, sys
with open(sys.argv[1], "rb") as fh:
    doc = json.load(fh)
versions = doc.get("recipeVersions") or []
ready = [str(v.get("versionId", "")) for v in versions
         if v.get("status") == "UPLOAD_COMPLETE" and v.get("versionId")]
latest = str((doc.get("recipe") or {}).get("latestVersionIdStr") or "")
if latest and latest in ready:
    print(latest)
elif ready:
    print(ready[0])
PY
)" || rc=$?
  rm -f "${out}"

  if [[ "${rc}" -ne 0 || -z "${version}" ]]; then
    sc_ngc_err "no finished version found for '${resource}'."
    return 2
  fi

  printf '%s\n' "${version}"
}

# Print "<path>\t<size in bytes>\t<sha256 in hex>" for every file of a version.
# The hash column is empty when NGC reports none.
# Arguments: <kind> <name> <version>.
sc_ngc_list_kind_files() {
  local kind="$1"
  local resource="$2"
  local version="$3"
  local base url out page pages
  local rc=0
  local listing=""

  base="$(sc_ngc_kind_url "${kind}" "${resource}")/versions/${version}/files"
  page=0
  pages=1

  while [[ "${page}" -lt "${pages}" ]]; do
    url="${base}"
    if [[ "${page}" -gt 0 ]]; then
      url="${base}?page-number=${page}"
    fi

    out="$(mktemp)"
    if ! sc_ngc_api_get "${url}" "${out}" "${resource}"; then
      rm -f "${out}"
      return 2
    fi

    local chunk
    chunk="$(python3 - "${out}" <<'PY'
import base64, json, sys
with open(sys.argv[1], "rb") as fh:
    doc = json.load(fh)
lines = []
for key in ("recipeFiles", "modelFiles"):
    for f in doc.get(key) or []:
        digest = ""
        b64 = f.get("sha256_base64") or ""
        if b64:
            try:
                digest = base64.b64decode(b64).hex()
            except Exception:
                digest = ""
        lines.append("\t".join([
            str(f.get("path", "")),
            str(f.get("sizeInBytes", "")),
            digest,
        ]))
info = doc.get("paginationInfo") or {}
print(int(info.get("totalPages") or 1))
print("\n".join(lines))
PY
)" || rc=$?
    rm -f "${out}"

    if [[ "${rc}" -ne 0 ]]; then
      sc_ngc_err "could not read the file list for '${resource}' version ${version}."
      return 2
    fi

    pages="$(printf '%s\n' "${chunk}" | head -n 1)"
    [[ "${pages}" =~ ^[0-9]+$ ]] || pages=1
    listing+="$(printf '%s\n' "${chunk}" | tail -n +2)"$'\n'
    page=$((page + 1))
  done

  # Drop blank lines and repeats; a repeat means the API ignored the page number.
  printf '%s' "${listing}" | awk 'NF && !seen[$0]++'
}

sc_ngc_list_files() {
  sc_ngc_list_kind_files resources "$1" "$2"
}

sc_ngc_list_model_files() {
  sc_ngc_list_kind_files models "$1" "$2"
}

# True when a file is present with the expected size and hash.
sc_ngc_file_is_verified() {
  local path="$1"
  local sha256="${2:-}"
  local size="${3:-}"
  local actual

  [[ -f "${path}" ]] || return 1

  if [[ -n "${size}" ]]; then
    actual="$(stat -c '%s' "${path}")"
    [[ "${actual}" == "${size}" ]] || return 1
  fi

  if [[ -n "${sha256}" ]]; then
    actual="$(sha256sum "${path}" | cut -d' ' -f1)"
    [[ "${actual}" == "${sha256}" ]] || return 1
  fi

  return 0
}

# Download one file of one item version into <dest>.
#
# Arguments: <kind> <name> <version> <file path> <dest> [sha256hex] [size in bytes]
#
# The download resumes a partial transfer, retries, and checks the hash. A file
# that is already there with the right hash is left alone.
sc_ngc_download_kind_file() {
  local kind="$1"
  local resource="$2"
  local version="$3"
  local relpath="$4"
  local dest="$5"
  local sha256="${6:-}"
  local size="${7:-}"
  local url encoded part attempt status
  local -a progress=(--silent --show-error)

  # Show a progress bar for a person, and stay quiet in a log file.
  if [[ -t 1 ]]; then
    progress=(--progress-bar)
  fi

  if sc_ngc_file_is_verified "${dest}" "${sha256}" "${size}"; then
    sc_ngc_log "${relpath}: already downloaded, sha256 verified."
    return 0
  fi

  encoded="$(sc_ngc_url_encode_path "${relpath}")"
  url="$(sc_ngc_kind_url "${kind}" "${resource}")/versions/${version}/files/${encoded}"
  part="${dest}.part"

  if [[ "${SC_NGC_DRY_RUN}" == "1" ]]; then
    sc_ngc_log "Would download ${relpath} ($(sc_ngc_human_bytes "${size:-0}")) to ${dest}"
    sc_ngc_log "  $(sc_ngc_redacted_cmd --fail --location --continue-at - \
      --header "Authorization: x" --output "${part}" "${url}")"
    return 0
  fi

  sc_ngc_ensure_token || return 2

  mkdir -p "$(dirname "${dest}")"

  for attempt in 1 2 3; do
    if [[ "${attempt}" -gt 1 ]]; then
      sc_ngc_log "${relpath}: retry ${attempt} of 3."
    fi

    sc_ngc_log "Downloading ${relpath} ($(sc_ngc_human_bytes "${size:-0}")) -> ${dest}"

    # --continue-at - resumes a part file from an earlier run. curl drops the
    # Authorization header on the redirect to the signed storage URL, which is
    # what we want: the signed URL carries its own credentials.
    status=0
    curl --fail --location "${progress[@]}" \
      --retry "${SC_NGC_RETRIES}" --retry-delay 2 --retry-connrefused \
      --continue-at - \
      --header "Authorization: Bearer ${_SC_NGC_TOKEN}" \
      --output "${part}" \
      "${url}" || status=$?

    if [[ "${status}" -ne 0 ]]; then
      # curl exit 33 means the server refuses a resume; start over.
      if [[ "${status}" -eq 33 ]]; then
        rm -f "${part}"
        continue
      fi
      sc_ngc_err "download of ${relpath} failed (curl exit ${status})."
      continue
    fi

    if sc_ngc_file_is_verified "${part}" "${sha256}" "${size}"; then
      mv -f "${part}" "${dest}"
      sc_ngc_log "${relpath}: downloaded and sha256 verified."
      return 0
    fi

    sc_ngc_err "${relpath}: size or sha256 mismatch. Removing the file and trying again."
    rm -f "${part}"
  done

  sc_ngc_err "could not download ${relpath} from '${resource}' version ${version}."
  return 2
}

sc_ngc_download_file() {
  sc_ngc_download_kind_file resources "$@"
}

sc_ngc_download_model_file() {
  sc_ngc_download_kind_file models "$@"
}

# Download every file of one model version into <dest dir>.
#
# Arguments: <model> <version> <dest dir>
#
# NGC does not always report a sha256 for a model file. The NVIDIA feature
# packs ship an <file>.md5 next to each payload, so those pairs are checked
# after the download.
sc_ngc_download_model_version() {
  local model="$1"
  local version="$2"
  local destdir="$3"
  local listing path size sha dest md5file target want got

  listing="$(sc_ngc_list_model_files "${model}" "${version}")" || return 2
  if [[ -z "${listing}" ]]; then
    sc_ngc_err "NGC lists no files for model '${model}' version ${version}."
    return 2
  fi

  while IFS=$'\t' read -r path size sha; do
    [[ -n "${path}" ]] || continue
    dest="${destdir}/${path}"
    sc_ngc_download_model_file "${model}" "${version}" "${path}" "${dest}" "${sha}" "${size}" || return 2
  done <<< "${listing}"

  [[ "${SC_NGC_DRY_RUN}" != "1" ]] || return 0

  # A model file path can name a subdirectory, so the .md5 companions do not
  # all land at the top of the destination. Check every one of them where it is.
  while IFS= read -r -d '' md5file; do
    target="${md5file%.md5}"
    [[ -f "${target}" ]] || continue
    want="$(cut -c1-32 < "${md5file}")"
    got="$(md5sum "${target}" | cut -c1-32)"
    if [[ "${want}" != "${got}" ]]; then
      sc_ngc_err "md5 mismatch for ${target#"${destdir}"/}. Removing it; run the download again."
      rm -f "${target}"
      return 2
    fi
    sc_ngc_log "${target#"${destdir}"/}: md5 verified."
    rm -f "${md5file}"
  done < <(find "${destdir}" -type f -name '*.md5' -print0)
}
