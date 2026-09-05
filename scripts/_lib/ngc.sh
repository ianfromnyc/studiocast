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
# The key is never printed, and it never goes on curl's command line: it
# reaches curl through a --config file on a pipe, because /proc/<pid>/cmdline
# is world-readable. Every command this file logs shows the Authorization
# header as "Bearer <redacted>".
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

# The most pages of a file list to ask for. The page count comes from the
# server, and a version of a Maxine pack holds a few files, so a count near
# this limit means the answer is wrong. Without a limit, a server that reports
# a very large count makes the listing ask for hours.
SC_NGC_MAX_PAGES="${SC_NGC_MAX_PAGES:-200}"

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
  awk cut dirname head md5sum mktemp realpath sha256sum stat tail
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
    sc_ngc_err "  Fedora: sudo dnf install curl python3 coreutils gawk"
    sc_ngc_err "  Ubuntu: sudo apt-get install curl python3 coreutils gawk"
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

# Escape one value for the quoted form of a curl config file.
_sc_ngc_config_escape() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  printf '%s' "${value}"
}

# Print a curl config that carries the Authorization header.
#
# The token must not go on curl's command line. /proc/<pid>/cmdline is
# world-readable, so anything that can read the proc tree could take the token
# while a transfer runs. The callers hand this to "--config" through a process
# substitution, so curl reads it from a pipe: it is never on disk and never in
# the process table.
_sc_ngc_auth_config() {
  printf 'header = "Authorization: Bearer %s"\n' \
    "$(_sc_ngc_config_escape "${_SC_NGC_TOKEN}")"
}

# GET one URL with the current token. Prints the HTTP status code.
_sc_ngc_curl_status() {
  local url="$1"
  local out="$2"
  local status

  status="$(curl --silent --show-error --location \
    --config <(_sc_ngc_auth_config) \
    --retry "${SC_NGC_RETRIES}" --retry-delay 2 --retry-connrefused \
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
  # The raw key goes in the same way, for the same reason.
  status="$(curl --silent --show-error --location \
    --config <(printf "user = \"\$oauthtoken:%s\"\n" \
      "$(_sc_ngc_config_escape "${key}")") \
    --retry "${SC_NGC_RETRIES}" --retry-delay 2 \
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
    sc_ngc_log "GET $(sc_ngc_redacted_cmd --silent \
      --config "<file holding: Authorization: x>" "${url}")" >&2
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
  local base url out page pages merged
  local rc=0
  local listing=""

  if ! [[ "${SC_NGC_MAX_PAGES}" =~ ^[0-9]+$ ]] || [[ "${SC_NGC_MAX_PAGES}" -lt 1 ]]; then
    sc_ngc_err "SC_NGC_MAX_PAGES must be a whole number of 1 or more, not '${SC_NGC_MAX_PAGES}'."
    return 2
  fi

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
    if [[ "${pages}" -gt "${SC_NGC_MAX_PAGES}" ]]; then
      sc_ngc_err "'${resource}' version ${version} reports ${pages} pages of files, which is more than the limit of ${SC_NGC_MAX_PAGES}."
      sc_ngc_err "Raise SC_NGC_MAX_PAGES if that count is right."
      return 2
    fi

    # Join this page onto what the earlier pages gave, without blank lines and
    # without repeats.
    local entries
    entries="$(printf '%s\n' "${chunk}" | tail -n +2 | awk 'NF')"
    merged="$(printf '%s\n%s\n' "${listing}" "${entries}" | awk 'NF && !seen[$0]++')"

    # An API that ignores the page number serves page 0 every time. Such a page
    # holds files, and every one of them is already known, so there is no more
    # to get. Stop there, or a large totalPages makes this loop ask for hours.
    #
    # A page that holds no file at all is a different thing: the pages after it
    # can still hold files, so go on and do not lose them.
    if [[ -n "${entries}" && "${merged}" == "${listing}" ]]; then
      sc_ngc_err "page ${page} of the file list for '${resource}' version ${version} repeats what the pages before it gave. Stopping after ${page} of ${pages} pages."
      break
    fi
    listing="${merged}"
    page=$((page + 1))
  done

  [[ -z "${listing}" ]] || printf '%s\n' "${listing}"
}

sc_ngc_list_files() {
  sc_ngc_list_kind_files resources "$1" "$2"
}

sc_ngc_list_model_files() {
  sc_ngc_list_kind_files models "$1" "$2"
}

# Join a destination directory with one file path that NGC reported.
#
# The path comes from the answer of a server, and it goes straight into a local
# file name. A path that is absolute, or that holds a ".." component, would put
# the file outside the destination directory and overwrite whatever is there.
# A path with a tab or a newline also breaks the tab separated listing that
# carried it. All of those are refused here, before anything is created.
#
# Prints the joined path. Returns 2 and says why when the path is not usable.
#
# Arguments: <dest dir> <path from the listing>
sc_ngc_safe_dest() {
  local destdir="$1"
  local path="$2"
  local dest real_dest real_base

  if [[ -z "${path}" ]]; then
    sc_ngc_err "the file list holds an empty path. Refusing it."
    return 2
  fi

  if [[ "${path}" == /* ]]; then
    sc_ngc_err "the file list holds an absolute path: '${path}'. Refusing it."
    return 2
  fi

  if [[ "${path}" == *$'\t'* || "${path}" == *$'\n'* ]]; then
    sc_ngc_err "the file list holds a path with a tab or a newline. Refusing it."
    return 2
  fi

  # The slashes on both ends make this test whole components, so a name such
  # as "..config" is kept and "a/../b" is not.
  case "/${path}/" in
    */../*)
      sc_ngc_err "the file list holds a path that leaves the destination: '${path}'. Refusing it."
      return 2
      ;;
  esac

  dest="${destdir}/${path}"

  # A symlink in the destination, or a name this check did not think of, could
  # still lead out. Compare the resolved paths as well. -m works on names that
  # are not there yet, which is the normal case before a download.
  real_base="$(realpath -m -- "${destdir}")" || return 2
  real_dest="$(realpath -m -- "${dest}")" || return 2
  if [[ "${real_dest}" != "${real_base}/"* ]]; then
    sc_ngc_err "'${path}' would land outside ${destdir}. Refusing it."
    return 2
  fi

  printf '%s\n' "${dest}"
}

# Compare a file with what NGC reported about it.
#
# NGC gives no sha256 for many model files, and sometimes no size either. A
# file cannot be checked against nothing, so that case gets its own answer
# instead of counting as verified.
#
# Return codes:
#   0  the file is there and every value NGC gave matches. The names of the
#      values that were compared go to stdout, for the caller's log.
#   1  the file is not there, or a value does not match.
#   3  the file is there, but NGC gave neither a sha256 nor a size, so nothing
#      was compared.
#
# Arguments: <path> [sha256hex] [size in bytes]
sc_ngc_file_is_verified() {
  local path="$1"
  local sha256="${2:-}"
  local size="${3:-}"
  local actual
  local checked=""

  [[ -f "${path}" ]] || return 1

  if [[ -n "${size}" ]]; then
    actual="$(stat -c '%s' "${path}")"
    [[ "${actual}" == "${size}" ]] || return 1
    checked="size"
  fi

  if [[ -n "${sha256}" ]]; then
    actual="$(sha256sum "${path}" | cut -d' ' -f1)"
    [[ "${actual}" == "${sha256}" ]] || return 1
    [[ -z "${checked}" ]] || checked+=" and "
    checked+="sha256"
  fi

  [[ -n "${checked}" ]] || return 3

  printf '%s' "${checked}"
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
  local url encoded part attempt status http_code checked
  local vrc restarted=0
  local -a progress=(--silent --show-error)

  # Show a progress bar for a person, and stay quiet in a log file.
  if [[ -t 1 ]]; then
    progress=(--progress-bar)
  fi

  vrc=0
  checked="$(sc_ngc_file_is_verified "${dest}" "${sha256}" "${size}")" || vrc=$?
  if [[ "${vrc}" -eq 0 ]]; then
    sc_ngc_log "${relpath}: already downloaded, ${checked} verified."
    return 0
  fi
  if [[ "${vrc}" -eq 3 ]]; then
    # A file that cannot be checked is not a verified file. Fetch it again
    # rather than keep a copy that may be truncated or from another version.
    sc_ngc_log "${relpath}: already there, but NGC reports no sha256 and no size. Downloading it again."
  fi

  encoded="$(sc_ngc_url_encode_path "${relpath}")"
  url="$(sc_ngc_kind_url "${kind}" "${resource}")/versions/${version}/files/${encoded}"
  part="${dest}.part"

  if [[ "${SC_NGC_DRY_RUN}" == "1" ]]; then
    sc_ngc_log "Would download ${relpath} ($(sc_ngc_human_bytes "${size:-0}")) to ${dest}"
    sc_ngc_log "  $(sc_ngc_redacted_cmd --fail --location --continue-at - \
      --config "<file holding: Authorization: x>" --output "${part}" "${url}")"
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
    # --write-out prints the status of the answer on stdout. --fail turns
    # every status at or above 400 into the same exit code, 22, so the status
    # is the only way to tell one refusal from another below.
    status=0
    http_code="$(curl --fail --location "${progress[@]}" \
      --config <(_sc_ngc_auth_config) \
      --retry "${SC_NGC_RETRIES}" --retry-delay 2 --retry-connrefused \
      --continue-at - \
      --write-out '%{http_code}' \
      --output "${part}" \
      "${url}")" || status=$?

    # A storage backend refuses a resume with HTTP 416. Throw the partial file
    # away and start over, one time. Without this the same refusal comes back
    # in every later run, because the partial file stays where it is.
    #
    # The status decides this, not the exit code, because the two curl
    # generations disagree about a 416: curl 7.x makes --fail turn it into
    # exit 22, while curl 8.x, which Fedora 44 ships, calls the same answer a
    # success. The answer is the same event for both.
    #
    # Only 416. An expired token (401), a signed URL that went stale (403),
    # a backend that is down (503) and a disk that filled up (exit 23) all
    # leave good bytes in the partial file, and the next run resumes them.
    if [[ "${http_code}" == "416" ]]; then
      if [[ "${restarted}" -eq 0 && -s "${part}" ]]; then
        restarted=1
        sc_ngc_err "download of ${relpath} failed (HTTP 416, the server refuses to resume). Removing ${part} and starting over."
        rm -f "${part}"
        continue
      fi
      sc_ngc_err "download of ${relpath} failed (HTTP 416, the server refuses to resume)."
      continue
    fi

    if [[ "${status}" -ne 0 ]]; then
      # curl exit 33 means the server refuses a resume; start over.
      if [[ "${status}" -eq 33 ]]; then
        rm -f "${part}"
        continue
      fi
      if [[ "${http_code}" =~ ^[0-9]+$ ]] && [[ "${http_code}" != "000" ]]; then
        sc_ngc_err "download of ${relpath} failed (HTTP ${http_code}, curl exit ${status})."
      else
        sc_ngc_err "download of ${relpath} failed (curl exit ${status})."
      fi
      continue
    fi

    # curl exit 0 is not proof that the file arrived. Only 200 (the whole
    # file) and 206 (a resume the server honoured) put the bytes of the file
    # in the partial file. Keep the partial file, because a status that is not
    # a refused resume leaves the bytes of the earlier run where they are.
    if [[ "${http_code}" != "200" && "${http_code}" != "206" ]]; then
      sc_ngc_err "download of ${relpath} failed (HTTP ${http_code})."
      continue
    fi

    vrc=0
    checked="$(sc_ngc_file_is_verified "${part}" "${sha256}" "${size}")" || vrc=$?
    if [[ "${vrc}" -eq 0 ]]; then
      mv -f "${part}" "${dest}"
      sc_ngc_log "${relpath}: downloaded and ${checked} verified."
      return 0
    fi
    if [[ "${vrc}" -eq 3 ]]; then
      # Nothing to compare the transfer with. Keep it, because a re-download
      # would give the same unchecked bytes, and say plainly that it was not
      # checked. A model file's .md5 companion is checked separately.
      mv -f "${part}" "${dest}"
      sc_ngc_log "${relpath}: downloaded. NGC reports no sha256 and no size, so nothing was checked here."
      return 0
    fi

    sc_ngc_err "${relpath}: size or sha256 mismatch. Removing the file and trying again."
    rm -f "${part}"
  done

  if [[ -s "${part}" ]]; then
    sc_ngc_err "the partial file ${part} is kept, so the next run can resume it. Remove it if the next run fails the same way."
  fi
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
  local -a fetched=()

  listing="$(sc_ngc_list_model_files "${model}" "${version}")" || return 2
  if [[ -z "${listing}" ]]; then
    sc_ngc_err "NGC lists no files for model '${model}' version ${version}."
    return 2
  fi

  while IFS=$'\t' read -r path size sha; do
    [[ -n "${path}" ]] || continue
    dest="$(sc_ngc_safe_dest "${destdir}" "${path}")" || return 2
    sc_ngc_download_model_file "${model}" "${version}" "${path}" "${dest}" "${sha}" "${size}" || return 2
    fetched+=("${dest}")
  done <<< "${listing}"

  [[ "${SC_NGC_DRY_RUN}" != "1" ]] || return 0

  # Check the .md5 companions of this call only. A model file path can name a
  # subdirectory, so they do not all land at the top of the destination, and
  # several feature packs share one destination: rest_download_sdk_features
  # gives every feature the same <root>/lib/models. A sweep of the whole
  # directory would therefore check, and then remove, the companions of an
  # earlier pack, and a mismatch there would delete that pack's model file.
  for md5file in ${fetched[@]+"${fetched[@]}"}; do
    [[ "${md5file}" == *.md5 && -f "${md5file}" ]] || continue
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
  done
}
