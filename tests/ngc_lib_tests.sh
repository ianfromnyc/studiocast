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
#
# The log keeps the command line and the config file apart, so a check can say
# where a credential was: "argv: " is the command line, which any process on
# the machine can read from /proc, and "config: " is the file curl was handed,
# which no other process sees.
STUB_BIN="${SANDBOX}/bin"
mkdir -p "${STUB_BIN}"
cat > "${STUB_BIN}/curl" <<'STUB'
#!/usr/bin/env bash
printf 'argv: %s\n' "$*" >> "${NGC_CURL_LOG}"

out=""
url=""
config=""
write_out=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    -K|--config)
      config="$2"
      shift 2
      continue
      ;;
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

if [[ -n "${config}" && -r "${config}" ]]; then
  while IFS= read -r config_line; do
    printf 'config: %s\n' "${config_line}" >> "${NGC_CURL_LOG}"
  done < "${config}"
fi

# ${NGC_STUB_CURL_EXIT} fails the way curl does, and writes nothing.
if [[ -n "${NGC_STUB_CURL_EXIT:-}" ]]; then
  printf 'curl: (%s) stub failure\n' "${NGC_STUB_CURL_EXIT}" >&2
  exit "${NGC_STUB_CURL_EXIT}"
fi

# ${NGC_STUB_REFUSE_RESUME} answers a resume of a file that already holds
# bytes the way a storage backend does: HTTP 416. The value is the exit code
# curl gives for that answer, because the two curl generations disagree:
# curl 7.x makes --fail turn the 416 into exit 22, while curl 8.x calls the
# same answer a success and exits 0. Both write nothing to the output file.
# A transfer that starts from nothing is served as usual.
if [[ -n "${NGC_STUB_REFUSE_RESUME:-}" && -s "${out}" ]]; then
  if [[ "${NGC_STUB_REFUSE_RESUME}" -ne 0 ]]; then
    printf 'curl: (%s) The requested URL returned error: 416\n' \
      "${NGC_STUB_REFUSE_RESUME}" >&2
  fi
  [[ "${write_out}" -eq 0 ]] || printf '416'
  exit "${NGC_STUB_REFUSE_RESUME}"
fi

# ${NGC_STUB_HTTP_FAIL} answers with another status at or above 400, which
# --fail turns into curl exit 22 as well. The bytes already transferred stay
# where they are, the way curl leaves them.
if [[ -n "${NGC_STUB_HTTP_FAIL:-}" ]]; then
  printf 'curl: (22) The requested URL returned error: %s\n' \
    "${NGC_STUB_HTTP_FAIL}" >&2
  [[ "${write_out}" -eq 0 ]] || printf '%s' "${NGC_STUB_HTTP_FAIL}"
  exit 22
fi

# ${NGC_STUB_STATUS} answers with an HTTP status other than 200, for the
# checks of the message that each status produces.
status="${NGC_STUB_STATUS:-200}"

if [[ "${status}" != 200 ]]; then
  [[ -z "${out}" ]] || : > "${out}"
elif [[ "${url}" == *"/token?"* ]]; then
  [[ -z "${out}" ]] || printf '{"token": "stub-jwt"}' > "${out}"
elif [[ "${url}" == */files || "${url}" == */files\?* ]]; then
  # Page N of a listing comes from "${NGC_STUB_LISTING}.pageN" when that file
  # is there, so a check can serve more than one page.
  listing="${NGC_STUB_LISTING:-/dev/null}"
  if [[ "${url}" == *page-number=* ]]; then
    page="${url##*page-number=}"
    page="${page%%&*}"
    [[ ! -f "${listing}.page${page}" ]] || listing="${listing}.page${page}"
  fi
  [[ -z "${out}" ]] || cat "${listing}" > "${out}"
elif [[ "${url}" == *.md5 ]]; then
  [[ -z "${out}" ]] || printf '%s  payload\n' "${NGC_STUB_MD5:-}" > "${out}"
else
  [[ -z "${out}" ]] || printf 'stub payload\n' > "${out}"
fi

[[ "${write_out}" -eq 0 ]] || printf '%s' "${status}"
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

  # /proc/<pid>/cmdline is world-readable, so a credential on curl's command
  # line can be read by anything on the machine while the transfer runs.
  if grep -q "^argv:.*${MODERN_KEY}" "${log}"; then
    t_fail "the key went on curl's command line: $(grep '^argv:' "${log}")"
  else
    t_pass "the key stays off curl's command line"
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

  # The token exchange sends the raw key, which must stay off the command line
  # as well.
  if grep -q "^argv:.*${LEGACY_KEY}" "${log}"; then
    t_fail "the older key went on curl's command line: $(grep '^argv:' "${log}")"
  else
    t_pass "the older key stays off curl's command line"
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
# Arguments: <md5 the stub serves> <dest dir> [keep, to leave the dir as it is]
run_nested_model_version_download() {
  local md5="$1"
  local destdir="$2"
  local keep="${3:-}"

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

  [[ -n "${keep}" ]] || rm -rf "${destdir}"
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

# Several feature packs share one destination directory, because
# rest_download_sdk_features hands every feature the same <root>/lib/models.
# A download must therefore check and remove only the .md5 companions it
# fetched itself, not every one it finds in the destination.
test_an_unrelated_md5_in_the_destination_is_left_alone() {
  local destdir="${SANDBOX}/model-shared"
  rm -rf "${destdir}"
  mkdir -p "${destdir}/other"
  printf 'EARLIER FEATURE PAYLOAD\n' > "${destdir}/other/earlier.trtpkg"
  printf '00000000000000000000000000000000  earlier.trtpkg\n' \
    > "${destdir}/other/earlier.trtpkg.md5"

  local out
  out="$(run_nested_model_version_download "${PAYLOAD_MD5}" "${destdir}" keep)"

  if [[ "${out}" != *"RC=0"* ]]; then
    t_fail "an unrelated md5 in the destination failed the download: ${out}"
  else
    t_pass "an unrelated md5 in the destination does not fail the download"
  fi

  if [[ ! -f "${destdir}/other/earlier.trtpkg" ]]; then
    t_fail "the download removed a payload of an earlier download"
  else
    t_pass "a payload of an earlier download is left alone"
  fi

  if [[ ! -f "${destdir}/other/earlier.trtpkg.md5" ]]; then
    t_fail "the download removed the md5 of an earlier download"
  else
    t_pass "the md5 of an earlier download is left alone"
  fi
}

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

# A transfer that stops leaves a .part file, and the next run resumes it. A
# storage backend that refuses that resume answers HTTP 416, which --fail
# turns into curl exit 22, not the exit 33 of a plain refusal. The helper must
# drop the partial file and start over, or every later run repeats the same
# refusal and the download never finishes.
#
# ${1} is the exit code curl gives for that 416: 22 on curl 7.x, 0 on curl
# 8.x, which calls the answer a success. The helper must start over for both,
# because the answer is the same event.
run_refused_resume_check() {
  local curl_exit="$1"
  local dir="${SANDBOX}/refused-resume-${curl_exit}"
  local dest="${dir}/model.trtpkg"
  rm -rf "${dir}"
  mkdir -p "${dir}"
  printf 'BYTES FROM A TRANSFER THAT STOPPED' > "${dest}.part"

  local out
  export NGC_STUB_REFUSE_RESUME="${curl_exit}"
  # No sha256 and no size, which is what NGC reports for many model files.
  # Nothing but the restart can then tell a whole file from a stale part.
  out="$(run_single_file_download "${dest}" "" "")"
  unset NGC_STUB_REFUSE_RESUME

  if [[ "${out}" != *"RC=0"* ]]; then
    t_fail "a refused resume (curl exit ${curl_exit}) should start over and succeed: ${out}"
  else
    t_pass "a refused resume (curl exit ${curl_exit}) starts over and succeeds"
  fi

  if [[ -e "${dest}.part" ]]; then
    t_fail "the stale partial file was kept (curl exit ${curl_exit}): $(cat "${dest}.part")"
  else
    t_pass "the stale partial file is gone (curl exit ${curl_exit})"
  fi

  if [[ "$(cat "${dest}" 2>/dev/null)" != "stub payload" ]]; then
    t_fail "the stale part was promoted (curl exit ${curl_exit}): $(cat "${dest}" 2>/dev/null)"
  else
    t_pass "the restart wrote the whole file (curl exit ${curl_exit})"
  fi
}

# curl 7.x: --fail turns the 416 into exit 22.
test_a_refused_resume_starts_over() {
  run_refused_resume_check 22
}

# curl 8.x, which Fedora 44 ships: the same 416 is a success to curl, so the
# status is the only sign of it. A helper that reads the exit code alone
# promotes the stale part and calls the run a success.
test_a_refused_resume_starts_over_on_curl_8() {
  run_refused_resume_check 0
}

# curl exit 0 does not mean the file arrived. A status that carries no body
# for us, such as 204, still exits 0 with --fail, and leaves an empty or a
# stale partial file. The helper must refuse such a transfer instead of
# moving what is there to the destination and calling the run a success.
test_a_transfer_that_curl_calls_a_success_needs_a_200() {
  local dir="${SANDBOX}/no-content"
  local dest="${dir}/model.trtpkg"
  rm -rf "${dir}"
  mkdir -p "${dir}"

  local out
  export NGC_STUB_STATUS=204
  out="$(run_single_file_download "${dest}" "" "")"
  unset NGC_STUB_STATUS

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a status that is not 200 or 206 should fail the download: ${out}"
  else
    t_pass "a status that is not 200 or 206 fails the download"
  fi

  if [[ "${out}" != *"HTTP 204"* ]]; then
    t_fail "the failure did not name the HTTP status: ${out}"
  else
    t_pass "the failure of a curl that exits 0 names the HTTP status"
  fi

  if [[ -e "${dest}" ]]; then
    t_fail "an empty transfer was moved to the destination"
  else
    t_pass "an empty transfer is not moved to the destination"
  fi
}

# A download that gives up with the partial file still there must name it, so
# the user can remove it instead of watching every later run fail the same way.
test_a_failed_download_names_the_partial_file() {
  local dir="${SANDBOX}/kept-part"
  local dest="${dir}/model.trtpkg"
  rm -rf "${dir}"
  mkdir -p "${dir}"
  printf 'BYTES FROM A TRANSFER THAT STOPPED' > "${dest}.part"

  # curl exit 7 is "could not connect". The bytes already there are still
  # good, so the helper keeps them for the next resume.
  local out
  export NGC_STUB_CURL_EXIT=7
  out="$(run_single_file_download "${dest}" "" "${STUB_PAYLOAD_BYTES}")"
  unset NGC_STUB_CURL_EXIT

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a download that never connects should return 2: ${out}"
  else
    t_pass "a download that never connects returns 2"
  fi

  if [[ ! -e "${dest}.part" ]]; then
    t_fail "a network error threw away the bytes that were already there"
  else
    t_pass "a network error keeps the partial file for a resume"
  fi

  if [[ "${out}" != *"${dest}.part"* ]]; then
    t_fail "the failure did not name the partial file: ${out}"
  else
    t_pass "the failure names the partial file"
  fi
}

# --fail turns every HTTP status at or above 400 into curl exit 22, so the
# exit code alone cannot tell a refused resume from an expired token, a stale
# signed URL or a backend that is down. Only a refused resume may throw the
# bytes away; every other answer must keep them for the next run.
test_a_rejected_download_keeps_the_partial_file() {
  local dir="${SANDBOX}/rejected"
  local dest="${dir}/model.trtpkg"
  rm -rf "${dir}"
  mkdir -p "${dir}"
  printf 'BYTES FROM A TRANSFER THAT STOPPED' > "${dest}.part"

  local out
  export NGC_STUB_HTTP_FAIL=403
  out="$(run_single_file_download "${dest}" "" "${STUB_PAYLOAD_BYTES}")"
  unset NGC_STUB_HTTP_FAIL

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a download the server refuses should return 2: ${out}"
  else
    t_pass "a download the server refuses returns 2"
  fi

  if [[ ! -s "${dest}.part" ]]; then
    t_fail "HTTP 403 threw away the bytes that were already there"
  else
    t_pass "an HTTP status that is not 416 keeps the partial file"
  fi

  if [[ "${out}" != *"HTTP 403"* ]]; then
    t_fail "the failure did not name the HTTP status: ${out}"
  else
    t_pass "the failure names the HTTP status"
  fi
}

# NGC answers a long file list one page at a time. The helper must walk every
# page and join them, and must drop a repeat, because an API that ignores the
# page number would otherwise serve page 0 forever.
test_a_file_list_walks_every_page() {
  local listing="${SANDBOX}/paged.json"
  cat > "${listing}" <<'JSON'
{
  "modelFiles": [{"path": "first.trtpkg", "sizeInBytes": 1}],
  "paginationInfo": {"totalPages": 2}
}
JSON
  cat > "${listing}.page1" <<'JSON'
{
  "modelFiles": [
    {"path": "first.trtpkg", "sizeInBytes": 1},
    {"path": "second.trtpkg", "sizeInBytes": 2}
  ],
  "paginationInfo": {"totalPages": 2}
}
JSON

  local child="${SANDBOX}/paged-child.sh"
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
sc_ngc_list_model_files test-model 1.0
CHILD

  local log="${SANDBOX}/paged.log"
  : > "${log}"
  local out
  out="$(NGC_CURL_LOG="${log}" NGC_STUB_LISTING="${listing}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>/dev/null)"

  if ! grep -q 'page-number=1' "${log}"; then
    t_fail "the listing did not ask for the second page"
  else
    t_pass "the listing asks for every page"
  fi

  if [[ "${out}" != *first.trtpkg* || "${out}" != *second.trtpkg* ]]; then
    t_fail "the listing lost a file: ${out}"
  else
    t_pass "the listing holds the files of both pages"
  fi

  local first_count
  first_count="$(grep -c '^first\.trtpkg' <<< "${out}")"
  if [[ "${first_count}" != "1" ]]; then
    t_fail "a repeated file was listed ${first_count} times: ${out}"
  else
    t_pass "a file that both pages hold is listed once"
  fi
}

# totalPages comes from the server. An API that ignores the page number and
# serves the same page every time must not make the helper ask for all of
# them. The loop stops at the first page that adds no new file. The count here
# is under SC_NGC_MAX_PAGES, so only the repeat can stop the loop.
test_a_page_that_adds_nothing_stops_the_listing() {
  local listing="${SANDBOX}/repeat.json"
  cat > "${listing}" <<'JSON'
{
  "modelFiles": [{"path": "only.trtpkg", "sizeInBytes": 1}],
  "paginationInfo": {"totalPages": 150}
}
JSON

  local child="${SANDBOX}/repeat-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=1
sc_ngc_list_model_files test-model 1.0
CHILD

  local log="${SANDBOX}/repeat.log"
  local errfile="${SANDBOX}/repeat.err"
  : > "${log}"
  local out
  out="$(timeout 30 env NGC_CURL_LOG="${log}" NGC_STUB_LISTING="${listing}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>"${errfile}")"

  local requests
  requests="$(grep -c '^argv:' "${log}")"
  if [[ "${requests}" -gt 3 ]]; then
    t_fail "a repeated page was asked for ${requests} times"
  else
    t_pass "a page that adds nothing stops the listing"
  fi

  if [[ "${out}" != *only.trtpkg* ]]; then
    t_fail "the listing lost the only file: ${out}"
  else
    t_pass "the listing keeps what the pages did hold"
  fi

  local err
  err="$(cat "${errfile}")"
  if [[ "${err}" != *"repeats"* ]]; then
    t_fail "the stop did not say why it stopped: ${err}"
  else
    t_pass "the stop says why it stopped"
  fi

  # Nothing failed here: the loop stopped where it must and the listing is
  # whole. A note that says ERROR sends the user to look for a fault.
  if [[ "${err}" == *ERROR* ]]; then
    t_fail "a listing that worked printed an error: ${err}"
  else
    t_pass "a listing that worked prints no error"
  fi

  # stdout carries the listing, so the note must not go there.
  if [[ "${out}" == *"[ngc]"* ]]; then
    t_fail "a log line went into the listing: ${out}"
  else
    t_pass "the note stays out of the listing"
  fi
}

# A page in the middle that holds no file is not a page that repeats an
# earlier one: the pages after it can still hold files. The loop must go on,
# or a listing loses files and still reports success.
test_an_empty_middle_page_does_not_end_the_listing() {
  local listing="${SANDBOX}/gap.json"
  cat > "${listing}" <<'JSON'
{
  "modelFiles": [{"path": "first.trtpkg", "sizeInBytes": 1}],
  "paginationInfo": {"totalPages": 3}
}
JSON
  cat > "${listing}.page1" <<'JSON'
{
  "modelFiles": [],
  "paginationInfo": {"totalPages": 3}
}
JSON
  cat > "${listing}.page2" <<'JSON'
{
  "modelFiles": [{"path": "last.trtpkg", "sizeInBytes": 2}],
  "paginationInfo": {"totalPages": 3}
}
JSON

  local child="${SANDBOX}/gap-child.sh"
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
sc_ngc_list_model_files test-model 1.0
CHILD

  local out
  out="$(timeout 30 env NGC_CURL_LOG="${SANDBOX}/gap.log" \
    NGC_STUB_LISTING="${listing}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>/dev/null)"

  if [[ "${out}" != *first.trtpkg* ]]; then
    t_fail "the listing lost the file of the first page: ${out}"
  else
    t_pass "an empty page keeps what the pages before it gave"
  fi

  if [[ "${out}" != *last.trtpkg* ]]; then
    t_fail "an empty middle page ended the listing early: ${out}"
  else
    t_pass "an empty middle page does not end the listing"
  fi
}

# A page limit that is not a number reads as 0 in an arithmetic test, which
# refuses every listing. Say what is wrong with the setting instead.
test_a_page_limit_that_is_not_a_number_is_refused() {
  local listing="${SANDBOX}/badlimit.json"
  cat > "${listing}" <<'JSON'
{
  "modelFiles": [{"path": "only.trtpkg", "sizeInBytes": 1}],
  "paginationInfo": {"totalPages": 1}
}
JSON

  local child="${SANDBOX}/badlimit-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=1
export SC_NGC_MAX_PAGES=all
sc_ngc_list_model_files test-model 1.0 >/dev/null
echo "RC=$?"
CHILD

  local out
  out="$(timeout 30 env NGC_CURL_LOG="${SANDBOX}/badlimit.log" \
    NGC_STUB_LISTING="${listing}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>&1)"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a page limit that is not a number should end the call: ${out}"
  else
    t_pass "a page limit that is not a number ends the call"
  fi

  if [[ "${out}" != *"SC_NGC_MAX_PAGES must be"* ]]; then
    t_fail "the refusal did not say what is wrong with the setting: ${out}"
  else
    t_pass "the refusal says what SC_NGC_MAX_PAGES needs"
  fi
}

# A retry count that is not a number goes to curl as it stands. curl then
# prints its usage text for each attempt and stops, which names nothing the
# user can correct. The setting must be refused before the first request.
test_a_retry_count_that_is_not_a_number_is_refused() {
  local listing="${SANDBOX}/badretry.json"
  cat > "${listing}" <<'JSON'
{
  "modelFiles": [{"path": "only.trtpkg", "sizeInBytes": 1}],
  "paginationInfo": {"totalPages": 1}
}
JSON

  local child="${SANDBOX}/badretry-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=abc
sc_ngc_list_model_files test-model 1.0 >/dev/null
echo "RC=$?"
CHILD

  local log="${SANDBOX}/badretry.log"
  : > "${log}"

  local out
  out="$(timeout 30 env NGC_CURL_LOG="${log}" \
    NGC_STUB_LISTING="${listing}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>&1)"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a retry count that is not a number should end the call: ${out}"
  else
    t_pass "a retry count that is not a number ends the call"
  fi

  if [[ "${out}" != *"SC_NGC_RETRIES must be"* ]]; then
    t_fail "the refusal did not say what is wrong with the setting: ${out}"
  else
    t_pass "the refusal says what SC_NGC_RETRIES needs"
  fi

  if [[ -s "${log}" ]]; then
    t_fail "a bad retry count still made a request: $(cat "${log}")"
  else
    t_pass "a bad retry count makes no request"
  fi
}

# A retry count below zero is not a count curl can use either.
test_a_negative_retry_count_is_refused() {
  local child="${SANDBOX}/negretry-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=-1
sc_ngc_list_model_versions test-model >/dev/null
echo "RC=$?"
CHILD

  local out
  out="$(timeout 30 env NGC_CURL_LOG="${SANDBOX}/negretry.log" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>&1)"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a retry count below zero should end the call: ${out}"
  else
    t_pass "a retry count below zero ends the call"
  fi

  if [[ "${out}" != *"SC_NGC_RETRIES must be"* ]]; then
    t_fail "the refusal did not name SC_NGC_RETRIES: ${out}"
  else
    t_pass "the refusal names SC_NGC_RETRIES"
  fi
}

# A page count no answer can justify must end the call with a message, not
# with a loop that runs until the user stops it.
test_an_absurd_page_count_is_refused() {
  local listing="${SANDBOX}/toomany.json"
  cat > "${listing}" <<'JSON'
{
  "modelFiles": [{"path": "first.trtpkg", "sizeInBytes": 1}],
  "paginationInfo": {"totalPages": 100000}
}
JSON
  # Each page holds one new file, so nothing stops the loop but the cap.
  local page
  for page in 1 2 3; do
    cat > "${listing}.page${page}" <<JSON
{
  "modelFiles": [{"path": "file${page}.trtpkg", "sizeInBytes": 1}],
  "paginationInfo": {"totalPages": 100000}
}
JSON
  done

  local child="${SANDBOX}/toomany-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=1
export SC_NGC_MAX_PAGES=2
sc_ngc_list_model_files test-model 1.0 >/dev/null
echo "RC=$?"
CHILD

  local out
  out="$(timeout 30 env NGC_CURL_LOG="${SANDBOX}/toomany.log" \
    NGC_STUB_LISTING="${listing}" \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>&1)"

  if [[ "${out}" != *"RC=2"* ]]; then
    t_fail "a page count over the limit should end the call: ${out}"
  else
    t_pass "a page count over the limit ends the call"
  fi

  if [[ "${out}" != *SC_NGC_MAX_PAGES* ]]; then
    t_fail "the refusal did not name the setting that raises the limit: ${out}"
  else
    t_pass "the refusal names the setting that raises the limit"
  fi
}

# Every HTTP status NGC answers with must turn into a line the user can act
# on, not a bare number.
test_each_refusal_is_explained() {
  local child="${SANDBOX}/status-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
unset NGC_API_KEY NGC_CLI_API_KEY
# shellcheck source=/dev/null
source "$1"
export SC_NGC_API_KEY="$2"
export SC_NGC_AUTHN_HOST="https://authn.invalid"
export SC_NGC_HOST="https://api.invalid"
export SC_NGC_RETRIES=1
SC_NGC_ALT_RESOURCES=("other_resource_name")
sc_ngc_list_model_files test-model 1.0
echo "RC=$?"
CHILD

  local status want
  # The word each message must hold, per status.
  for status in 401 402 403 404; do
    case "${status}" in
      401) want="API key" ;;
      402) want="entitlement" ;;
      403) want="refused" ;;
      404) want="no such resource" ;;
    esac

    local out
    out="$(NGC_CURL_LOG="${SANDBOX}/status.log" NGC_STUB_STATUS="${status}" \
      bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>&1)"

    if [[ "${out}" != *"RC=2"* ]]; then
      t_fail "HTTP ${status} should end the listing with 2: ${out}"
    else
      t_pass "HTTP ${status} ends the listing with 2"
    fi

    if [[ "${out}" != *"${want}"* ]]; then
      t_fail "HTTP ${status} did not explain itself ('${want}'): ${out}"
    else
      t_pass "HTTP ${status} is explained"
    fi
  done

  # 402 and 403 also list the other names the caller knows for the component.
  local alt
  alt="$(NGC_CURL_LOG="${SANDBOX}/status.log" NGC_STUB_STATUS=402 \
    bash "${child}" "${NGC_LIB}" "${MODERN_KEY}" 2>&1)"
  if [[ "${alt}" != *other_resource_name* ]]; then
    t_fail "HTTP 402 did not list the alternate names: ${alt}"
  else
    t_pass "HTTP 402 lists the alternate names"
  fi
}

test_a_first_call_download_sets_the_token
test_a_first_call_download_exchanges_an_older_key
test_a_file_list_walks_every_page
test_each_refusal_is_explained
test_no_key_is_reported
test_a_nested_md5_is_verified_and_removed
test_a_nested_md5_mismatch_fails
test_a_missing_awk_is_reported_early
test_a_path_that_leaves_the_destination_is_refused
test_an_absolute_path_is_refused
test_a_file_with_no_hash_and_no_size_is_downloaded_again
test_a_size_only_check_is_named_a_size_check
test_an_unrelated_md5_in_the_destination_is_left_alone
test_a_refused_resume_starts_over
test_a_refused_resume_starts_over_on_curl_8
test_a_transfer_that_curl_calls_a_success_needs_a_200
test_a_failed_download_names_the_partial_file
test_a_rejected_download_keeps_the_partial_file
test_a_page_that_adds_nothing_stops_the_listing
test_an_absurd_page_count_is_refused
test_an_empty_middle_page_does_not_end_the_listing
test_a_page_limit_that_is_not_a_number_is_refused
test_a_retry_count_that_is_not_a_number_is_refused
test_a_negative_retry_count_is_refused

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
