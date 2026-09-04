#!/usr/bin/env bash
#
# Offline checks for scripts/_lib/onnxruntime.sh.
#
# Nothing here writes to the system directories the helper installs into. The
# sc_ort_priv hook does nothing, and the version is one no real install uses,
# so every call stops at the header check before a real install would start.
#
# Each check runs in a child shell, so that a helper which ends the script
# instead of returning cannot take this script down with it.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ORT_LIB="${REPO_ROOT}/scripts/_lib/onnxruntime.sh"

FAILURES=0
t_fail() {
  echo "FAIL: $*" >&2
  FAILURES=$((FAILURES + 1))
}
t_pass() { echo "ok: $*"; }

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT

# A curl that writes the -o target instead of downloading it. With
# STUB_CURL_FAIL=1 it reports a failure, which is how the checks below drive
# the set -e path.
STUB_BIN="${SANDBOX}/bin"
mkdir -p "${STUB_BIN}"
cat > "${STUB_BIN}/curl" <<'STUB'
#!/usr/bin/env bash
out=""
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "-o" ]]; then
    out="$2"
    shift 2
    continue
  fi
  shift
done
if [[ "${STUB_CURL_FAIL:-0}" == "1" ]]; then
  echo "stub curl: refusing to download" >&2
  exit 22
fi
[[ -n "${out}" ]] || exit 1
printf 'fake tarball\n' > "${out}"
STUB
chmod +x "${STUB_BIN}/curl"
export PATH="${STUB_BIN}:${PATH}"

FAKE_VERSION="0.0.0-studiocast-test"
FAKE_ASSET="studiocast-test-asset"
FAKE_URL="https://example.invalid/${FAKE_ASSET}.tgz"

# Report the entries left in a directory, one per line, or nothing.
leftover_entries() {
  find "$1" -mindepth 1 -maxdepth 1 2>/dev/null
}

# Two calls in a row must each remove their own temporary directory, and
# neither may touch the EXIT trap of the caller.
test_repeated_calls_clean_up_and_keep_the_caller_trap() {
  local tmproot="${SANDBOX}/repeat-tmp"
  mkdir -p "${tmproot}"

  local child="${SANDBOX}/repeat-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
sc_ort_log() { :; }
sc_ort_priv() { :; }
# shellcheck source=/dev/null
source "$1"
trap 'echo CALLER_EXIT_TRAP_RAN' EXIT
before="$(trap -p EXIT)"
export TMPDIR="$2"
for call in 1 2; do
  sc_ort_install_tarball "$3" "$4" "$5" 2>/dev/null
  echo "CALL_${call}_RETURNED_$?"
done
after="$(trap -p EXIT)"
if [[ "${before}" == "${after}" ]]; then
  echo EXIT_TRAP_UNCHANGED
else
  echo "EXIT_TRAP_CHANGED [${before}] -> [${after}]"
fi
CHILD

  local out
  out="$(bash "${child}" "${ORT_LIB}" "${tmproot}" \
    "${FAKE_VERSION}" "${FAKE_ASSET}" "${FAKE_URL}" 2>/dev/null)"

  local marker
  for marker in CALL_1_RETURNED_1 CALL_2_RETURNED_1 EXIT_TRAP_UNCHANGED \
    CALLER_EXIT_TRAP_RAN; do
    if [[ "${out}" != *"${marker}"* ]]; then
      t_fail "expected ${marker} in the output of the repeated calls: ${out}"
    else
      t_pass "${marker}"
    fi
  done

  local leftovers
  leftovers="$(leftover_entries "${tmproot}")"
  if [[ -n "${leftovers}" ]]; then
    t_fail "temporary directories left behind: ${leftovers}"
  else
    t_pass "both temporary directories were removed"
  fi
}

# When the download fails, set -e ends the script. The temporary directory must
# still go, and the EXIT trap of the caller must still run.
test_download_failure_cleans_up_and_runs_the_caller_trap() {
  local tmproot="${SANDBOX}/errexit-tmp"
  mkdir -p "${tmproot}"

  local child="${SANDBOX}/errexit-child.sh"
  cat > "${child}" <<'CHILD'
set -euo pipefail
sc_ort_log() { :; }
sc_ort_priv() { :; }
# shellcheck source=/dev/null
source "$1"
trap 'echo CALLER_EXIT_TRAP_RAN' EXIT
export TMPDIR="$2"
sc_ort_install_tarball "$3" "$4" "$5"
echo NOT_REACHED
CHILD

  local out rc=0
  out="$(STUB_CURL_FAIL=1 bash "${child}" "${ORT_LIB}" "${tmproot}" \
    "${FAKE_VERSION}" "${FAKE_ASSET}" "${FAKE_URL}" 2>/dev/null)" || rc=$?

  if [[ "${rc}" -eq 0 ]]; then
    t_fail "a failed download should end the script with a non-zero status"
  fi
  if [[ "${out}" == *NOT_REACHED* ]]; then
    t_fail "the script continued after a failed download"
  fi
  if [[ "${out}" != *CALLER_EXIT_TRAP_RAN* ]]; then
    t_fail "the EXIT trap of the caller did not run after a failed download"
  else
    t_pass "the EXIT trap of the caller ran after a failed download"
  fi

  local leftovers
  leftovers="$(leftover_entries "${tmproot}")"
  if [[ -n "${leftovers}" ]]; then
    t_fail "temporary directory left behind after a failed download: ${leftovers}"
  else
    t_pass "the temporary directory was removed after a failed download"
  fi
}

# A bootstrap root keeps its libraries in lib or in lib64. Everything that
# looks inside a root must follow the layout of that root.
test_libdir_follows_the_layout_of_the_root() {
  # Hooks the library expects from its caller. Nothing below calls them.
  sc_ort_log() { :; }
  sc_ort_priv() { :; }
  # shellcheck source=../scripts/_lib/onnxruntime.sh
  source "${ORT_LIB}"

  local root_lib64="${SANDBOX}/root-lib64"
  local root_lib="${SANDBOX}/root-lib"
  mkdir -p "${root_lib64}/lib" "${root_lib64}/lib64" "${root_lib}/lib"

  local got
  got="$(sc_ort_libdir "${root_lib64}")"
  if [[ "${got}" != "${root_lib64}/lib64" ]]; then
    t_fail "a root with lib64 should use it, got ${got}"
  else
    t_pass "a root with lib64 uses lib64"
  fi

  got="$(sc_ort_libdir "${root_lib}")"
  if [[ "${got}" != "${root_lib}/lib" ]]; then
    t_fail "a root with lib only should use it, got ${got}"
  else
    t_pass "a root with lib only uses lib"
  fi
}

test_repeated_calls_clean_up_and_keep_the_caller_trap
test_download_failure_cleans_up_and_runs_the_caller_trap
test_libdir_follows_the_layout_of_the_root

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
