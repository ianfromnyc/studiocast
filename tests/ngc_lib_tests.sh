#!/usr/bin/env bash
#
# Offline checks for scripts/_lib/ngc.sh.
#
# Nothing here reaches the network. A stub curl on the PATH answers every
# request and records what it was asked for, and the keys below are made up, so
# no real NGC account is involved.
#
# Each check runs in a child shell, so that a helper which ends the script
# instead of returning cannot take this script down with it.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NGC_LIB="${REPO_ROOT}/scripts/_lib/ngc.sh"

FAILURES=0
t_fail() {
  echo "FAIL: $*" >&2
  FAILURES=$((FAILURES + 1))
}
t_pass() { echo "ok: $*"; }

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT

# A curl that never leaves the machine. It appends every call to
# ${NGC_CURL_LOG}, answers a token exchange with a JWT, and writes a payload
# for anything else.
STUB_BIN="${SANDBOX}/bin"
mkdir -p "${STUB_BIN}"
cat > "${STUB_BIN}/curl" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "${NGC_CURL_LOG}"

out=""
url=""
write_out=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o|--output)
      out="$2"
      shift 2
      continue
      ;;
    --write-out)
      write_out=1
      shift 2
      continue
      ;;
    http://*|https://*)
      url="$1"
      ;;
  esac
  shift
done

if [[ "${url}" == *"/token?"* ]]; then
  [[ -z "${out}" ]] || printf '{"token": "stub-jwt"}' > "${out}"
elif [[ "${url}" == */files || "${url}" == */files\?* ]]; then
  [[ -z "${out}" ]] || cat "${NGC_STUB_LISTING:-/dev/null}" > "${out}"
elif [[ "${url}" == *.md5 ]]; then
  [[ -z "${out}" ]] || printf '%s  payload\n' "${NGC_STUB_MD5:-}" > "${out}"
else
  [[ -z "${out}" ]] || printf 'stub payload\n' > "${out}"
fi

[[ "${write_out}" -eq 0 ]] || printf '200'
exit 0
STUB
chmod +x "${STUB_BIN}/curl"
export PATH="${STUB_BIN}:${PATH}"

# Made-up keys. The first has the shape of a modern personal key, which is a
# bearer token on its own. The second has the shape of an older key, which the
# helper must swap for a JWT.
MODERN_KEY="nvapi-studiocast-test-not-a-real-key"
LEGACY_KEY="studiocast-test-not-a-real-key"

# Hosts that resolve nowhere, so a stub that ever stopped working would fail
# instead of reaching NVIDIA.
AUTHN_HOST="https://authn.invalid"

# Run one download as the first call of a fresh shell and report the token
# state that call left behind.
#
# Arguments: <api key> <log file>
run_first_call_download() {
  local key="$1"
  local log="$2"

  local child="${SANDBOX}/download-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
sc_ngc_log() { :; }
sc_ngc_err() { :; }
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="$3"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=1
sc_ngc_download_file test-resource 1.0 "sub/dir/file.bin" "$4" >/dev/null 2>&1
echo "RC=$?"
echo "TOKEN=[${_SC_NGC_TOKEN}]"
echo "KIND=[${_SC_NGC_TOKEN_KIND}]"
CHILD

  : > "${log}"
  NGC_CURL_LOG="${log}" bash "${child}" "${NGC_LIB}" "${key}" "${AUTHN_HOST}" \
    "${SANDBOX}/downloads/file.bin" 2>/dev/null
  rm -f "${SANDBOX}/downloads/file.bin"
}

# A download can be the first call of a run. It must set the bearer token
# itself instead of expecting a GET to have set it.
test_a_first_call_download_sets_the_token() {
  local log="${SANDBOX}/modern.log"
  local out
  out="$(run_first_call_download "${MODERN_KEY}" "${log}")"

  if [[ "${out}" != *"RC=0"* ]]; then
    t_fail "the download did not succeed: ${out}"
  else
    t_pass "a first call download succeeds"
  fi

  if [[ "${out}" == *"TOKEN=[]"* ]]; then
    t_fail "a first call download left the bearer token empty: ${out}"
  else
    t_pass "a first call download sets the bearer token"
  fi

  if [[ "${out}" != *"KIND=[key]"* ]]; then
    t_fail "a modern key should stay the bearer token: ${out}"
  else
    t_pass "a modern key stays the bearer token"
  fi

  if grep -q '/token?' "${log}"; then
    t_fail "a modern key should not go through the token exchange"
  else
    t_pass "a modern key skips the token exchange"
  fi

  if ! grep -q "Authorization: Bearer ${MODERN_KEY}" "${log}"; then
    t_fail "the download did not send the key as a bearer token"
  else
    t_pass "the download sends the key as a bearer token"
  fi
}

# An older key is not a bearer token, so a first call download must go through
# the authn.nvidia.com exchange before it asks NGC for the file.
test_a_first_call_download_exchanges_an_older_key() {
  local log="${SANDBOX}/legacy.log"
  local out
  out="$(run_first_call_download "${LEGACY_KEY}" "${log}")"

  if [[ "${out}" != *"RC=0"* ]]; then
    t_fail "the download did not succeed: ${out}"
  else
    t_pass "a first call download with an older key succeeds"
  fi

  if ! grep -q "${AUTHN_HOST}/token?" "${log}"; then
    t_fail "an older key did not reach the token exchange: $(cat "${log}")"
  else
    t_pass "an older key reaches the token exchange"
  fi

  if [[ "${out}" != *"KIND=[jwt]"* ]]; then
    t_fail "an older key should end up with a JWT: ${out}"
  else
    t_pass "an older key ends up with a JWT"
  fi

  if ! grep -q "Authorization: Bearer stub-jwt" "${log}"; then
    t_fail "the download did not send the JWT: $(cat "${log}")"
  else
    t_pass "the download sends the JWT"
  fi
}

# Without a key there is nothing to send, and the helper must say so instead of
# asking NGC with an empty bearer token.
test_no_key_is_reported() {
  local child="${SANDBOX}/nokey-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY SC_NGC_API_KEY
sc_ngc_log() { :; }
sc_ngc_err() { :; }
# shellcheck source=/dev/null
source "$1"
sc_ngc_download_file test-resource 1.0 "file.bin" "$2" >/dev/null 2>&1
echo "RC=$?"
CHILD

  local log="${SANDBOX}/nokey.log"
  : > "${log}"
  local out
  out="$(NGC_CURL_LOG="${log}" bash "${child}" "${NGC_LIB}" \
    "${SANDBOX}/downloads/nokey.bin" 2>/dev/null)"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a download without a key should return 2: ${out}"
  else
    t_pass "a download without a key returns 2"
  fi

  if [[ -s "${log}" ]]; then
    t_fail "a download without a key still called curl: $(cat "${log}")"
  else
    t_pass "a download without a key calls no curl"
  fi
}

# The md5 of the payload the stub curl writes for a model file.
PAYLOAD_MD5="a034a9f16f32f0f19a6bc5f6828c593e"

# Download one model version whose only file sits in a subdirectory, with the
# .md5 companion next to it. Print the return code of the download.
#
# Arguments: <md5 the stub serves> <dest dir>
run_nested_model_version_download() {
  local md5="$1"
  local destdir="$2"

  local listing="${SANDBOX}/listing.json"
  cat > "${listing}" <<'JSON'
{
  "modelFiles": [
    {"path": "sub/dir/model.trtpkg"},
    {"path": "sub/dir/model.trtpkg.md5"}
  ],
  "paginationInfo": {"totalPages": 1}
}
JSON

  local child="${SANDBOX}/model-version-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
sc_ngc_log() { :; }
sc_ngc_err() { :; }
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=1
sc_ngc_download_model_version test-model 1.0 "$3" >/dev/null 2>&1
echo "RC=$?"
CHILD

  rm -rf "${destdir}"
  NGC_CURL_LOG="${SANDBOX}/model-version.log" \
    NGC_STUB_LISTING="${listing}" \
    NGC_STUB_MD5="${md5}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" "${destdir}" 2>/dev/null
}

# A model file path can name a subdirectory, so the .md5 companion lands there
# too. It must be checked and removed where it is, not only at the top of the
# destination directory.
test_a_nested_md5_is_verified_and_removed() {
  local destdir="${SANDBOX}/model-good"
  local out
  out="$(run_nested_model_version_download "${PAYLOAD_MD5}" "${destdir}")"

  if [[ "${out}" != *"RC=0"* ]]; then
    t_fail "a model version with a nested file did not download: ${out}"
  else
    t_pass "a model version with a nested file downloads"
  fi

  if [[ ! -f "${destdir}/sub/dir/model.trtpkg" ]]; then
    t_fail "the nested model file is missing after a matching md5"
  else
    t_pass "a matching md5 keeps the nested model file"
  fi

  local leftover
  leftover="$(find "${destdir}" -name '*.md5' 2>/dev/null)"
  if [[ -n "${leftover}" ]]; then
    t_fail "the nested md5 file was left behind: ${leftover}"
  else
    t_pass "a verified nested md5 file is removed"
  fi
}

# A nested .md5 that does not match must fail the download and take the file
# with it, the same as one at the top of the destination directory.
test_a_nested_md5_mismatch_fails() {
  local destdir="${SANDBOX}/model-bad"
  local out
  out="$(run_nested_model_version_download "00000000000000000000000000000000" \
    "${destdir}")"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a nested md5 mismatch should return 2: ${out}"
  else
    t_pass "a nested md5 mismatch returns 2"
  fi

  if [[ -f "${destdir}/sub/dir/model.trtpkg" ]]; then
    t_fail "a nested md5 mismatch left the model file in place"
  else
    t_pass "a nested md5 mismatch removes the model file"
  fi
}

test_a_first_call_download_sets_the_token
test_a_first_call_download_exchanges_an_older_key
test_no_key_is_reported
test_a_nested_md5_is_verified_and_removed
test_a_nested_md5_mismatch_fails

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
