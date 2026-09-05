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

# The early check must name every external tool the helper runs. A PATH
# without awk must end the call there, with a message that says which tool is
# missing, instead of failing in the middle of a listing.
test_a_missing_awk_is_reported_early() {
  local toolbin="${SANDBOX}/no-awk-bin"
  mkdir -p "${toolbin}"

  # Every tool the helper runs, except awk.
  local tool src
  for tool in curl python3 sha256sum md5sum mktemp cut head tail stat find \
    dirname; do
    src="$(command -v "${tool}")" || continue
    ln -sf "${src}" "${toolbin}/${tool}"
  done

  local child="${SANDBOX}/no-awk-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
# shellcheck source=/dev/null
source "$1"
export PATH="$2"
sc_ngc_require_tools
echo "RC=$?"
CHILD

  local bash_bin out
  bash_bin="$(command -v bash)"
  out="$("${bash_bin}" "${child}" "${NGC_LIB}" "${toolbin}" 2>&1)"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a PATH without awk should return 2: ${out}"
  else
    t_pass "a PATH without awk returns 2"
  fi

  if [[ "${out}" != *awk* ]]; then
    t_fail "the early check did not name awk: ${out}"
  else
    t_pass "the early check names awk"
  fi
}

# The payload the stub curl writes for a file, in bytes. A listing that
# reports this size makes the download accept the stub answer.
STUB_PAYLOAD_BYTES=13

# Run one model version download whose listing holds the given file path.
# Print the return code of the download.
#
# Arguments: <file path NGC reports> <dest dir>
run_model_version_download_with_path() {
  local path="$1"
  local destdir="$2"

  local listing="${SANDBOX}/traversal-listing.json"
  NGC_TEST_PATH="${path}" NGC_TEST_SIZE="${STUB_PAYLOAD_BYTES}" \
    python3 - "${listing}" <<'PY'
import json, os, sys
doc = {"modelFiles": [{"path": os.environ["NGC_TEST_PATH"],
                       "sizeInBytes": int(os.environ["NGC_TEST_SIZE"])}],
       "paginationInfo": {"totalPages": 1}}
with open(sys.argv[1], "w", encoding="utf-8") as fh:
    json.dump(doc, fh)
PY

  local child="${SANDBOX}/traversal-child.sh"
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
  mkdir -p "${destdir}"
  NGC_CURL_LOG="${SANDBOX}/traversal.log" \
    NGC_STUB_LISTING="${listing}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" "${destdir}" 2>/dev/null
}

# NGC names every file of a version, and that name becomes a local path. A
# name that leaves the destination directory must be refused before anything
# is created or written, or the download overwrites a file of the user's.
test_a_path_that_leaves_the_destination_is_refused() {
  local base="${SANDBOX}/traversal"
  local destdir="${base}/dl/models/dest"
  # Where "../../victim" from that destination lands.
  local victim="${base}/dl/victim"

  rm -rf "${base}"
  mkdir -p "${victim}"
  printf 'ORIGINAL USER FILE\n' > "${victim}/.bashrc"

  local out
  out="$(run_model_version_download_with_path "../../victim/.bashrc" \
    "${destdir}")"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a path that leaves the destination should return 2: ${out}"
  else
    t_pass "a path that leaves the destination returns 2"
  fi

  if [[ "$(cat "${victim}/.bashrc")" != "ORIGINAL USER FILE" ]]; then
    t_fail "the file outside the destination was overwritten"
  else
    t_pass "the file outside the destination is untouched"
  fi

  # A refusal must also leave no part file behind outside the destination.
  local stray
  stray="$(find "${base}" -name '*.part' 2>/dev/null)"
  if [[ -n "${stray}" ]]; then
    t_fail "the refused path left a part file behind: ${stray}"
  else
    t_pass "a refused path leaves no part file"
  fi
}

# An absolute path from the listing names a file anywhere on the machine.
test_an_absolute_path_is_refused() {
  local destdir="${SANDBOX}/traversal-abs/dest"
  local victim="${SANDBOX}/traversal-abs/victim.txt"

  rm -rf "${SANDBOX}/traversal-abs"
  mkdir -p "${SANDBOX}/traversal-abs"
  printf 'ORIGINAL USER FILE\n' > "${victim}"

  local out
  out="$(run_model_version_download_with_path "${victim}" "${destdir}")"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "an absolute path should return 2: ${out}"
  else
    t_pass "an absolute path returns 2"
  fi

  if [[ "$(cat "${victim}")" != "ORIGINAL USER FILE" ]]; then
    t_fail "an absolute path from the listing overwrote the file it named"
  else
    t_pass "an absolute path from the listing writes nothing"
  fi
}

# Download one file into <dest> with the sha256 and size the caller gives.
# Print the log of the helper and the return code.
#
# Arguments: <dest> <sha256 or empty> <size or empty>
run_single_file_download() {
  local dest="$1"
  local sha="$2"
  local size="$3"

  local child="${SANDBOX}/single-file-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=1
sc_ngc_download_kind_file models test-model 1.0 model.trtpkg "$3" "$4" "$5"
echo "RC=$?"
CHILD

  NGC_CURL_LOG="${SANDBOX}/single-file.log" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" "${dest}" "${sha}" "${size}" \
    2>&1
}

# NGC reports no sha256 for many model files. A file that is already there
# cannot be verified against nothing, so the helper must not keep it and must
# not say it verified a sha256.
test_a_file_with_no_hash_and_no_size_is_downloaded_again() {
  local dest="${SANDBOX}/vacuous/model.trtpkg"
  rm -rf "${SANDBOX}/vacuous"
  mkdir -p "${SANDBOX}/vacuous"
  printf 'TRUNCATED GARBAGE' > "${dest}"

  local out
  out="$(run_single_file_download "${dest}" "" "")"

  if [[ "${out}" != *"RC=0"* ]]; then
    t_fail "a download with no hash and no size should return 0: ${out}"
  else
    t_pass "a download with no hash and no size returns 0"
  fi

  if [[ "$(cat "${dest}")" == "TRUNCATED GARBAGE" ]]; then
    t_fail "the unverifiable file was kept instead of downloaded again"
  else
    t_pass "an unverifiable file is downloaded again"
  fi

  if [[ "${out}" == *"sha256 verified"* ]]; then
    t_fail "the log claims a sha256 check that did not happen: ${out}"
  else
    t_pass "the log claims no sha256 check"
  fi
}

# When NGC reports a size but no hash, the log must name the size, not a
# sha256 nobody checked.
test_a_size_only_check_is_named_a_size_check() {
  local dest="${SANDBOX}/size-only/model.trtpkg"
  rm -rf "${SANDBOX}/size-only"
  mkdir -p "${SANDBOX}/size-only"

  local out
  out="$(run_single_file_download "${dest}" "" "${STUB_PAYLOAD_BYTES}")"

  if [[ "${out}" != *"RC=0"* ]]; then
    t_fail "a download checked by size should return 0: ${out}"
  else
    t_pass "a download checked by size returns 0"
  fi

  if [[ "${out}" == *"sha256 verified"* ]]; then
    t_fail "a size-only check was logged as a sha256 check: ${out}"
  else
    t_pass "a size-only check is not logged as a sha256 check"
  fi

  if [[ "${out}" != *"size verified"* ]]; then
    t_fail "a size-only check did not name the size: ${out}"
  else
    t_pass "a size-only check names the size"
  fi
}

test_a_first_call_download_sets_the_token
test_a_first_call_download_exchanges_an_older_key
test_no_key_is_reported
test_a_nested_md5_is_verified_and_removed
test_a_nested_md5_mismatch_fails
test_a_missing_awk_is_reported_early
test_a_path_that_leaves_the_destination_is_refused
test_an_absolute_path_is_refused
test_a_file_with_no_hash_and_no_size_is_downloaded_again
test_a_size_only_check_is_named_a_size_check

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
