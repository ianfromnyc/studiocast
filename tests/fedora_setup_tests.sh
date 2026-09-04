#!/usr/bin/env bash
#
# Offline checks for scripts/setup/fedora.sh.
#
# The helper stops when it is sourced, so these checks call single functions
# without running a setup. Nothing here writes to the system directories the
# helper installs into: run_priv does nothing, and the cuDNN version is one no
# real install uses, so every call stops at the layout check before a real
# install would start.
#
# Each check runs in a child shell, so that a helper which ends the script
# instead of returning cannot take this script down with it.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
FEDORA_SETUP="${REPO_ROOT}/scripts/setup/fedora.sh"

FAILURES=0
t_fail() {
  echo "FAIL: $*" >&2
  FAILURES=$((FAILURES + 1))
}
t_pass() { echo "ok: $*"; }

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT

# A curl that writes the -o target instead of downloading it, and prints
# nothing without one, so the redistributable index lookup finds no SHA-256.
# With STUB_CURL_FAIL=1 it reports a failure, which is how the checks below
# drive the set -e path.
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
[[ -n "${out}" ]] || exit 0
printf 'fake archive\n' > "${out}"
STUB
chmod +x "${STUB_BIN}/curl"
export PATH="${STUB_BIN}:${PATH}"

# A version no real install uses, so the layout check always fails and the
# call ends before anything is written.
FAKE_CUDNN_VERSION="0.0.0.0-studiocast-test"

# The preamble every child shares: no server probes, no privileged commands,
# and a cuDNN that never resolves so the install path runs.
CHILD_PREAMBLE='export ORT_FLAVOR=cpu
export CUDA_MAJOR=13
export ORT_ARCH=x64
# shellcheck source=/dev/null
source "$1"
log() { :; }
warn() { :; }
run_priv() { :; }
lib_resolves() { return 1; }'

# Report the entries left in a directory, one per line, or nothing.
leftover_entries() {
  find "$1" -mindepth 1 -maxdepth 1 2>/dev/null
}

# First line of the output of a child, so that a failure message stays
# readable when the child prints a whole usage text.
first_line() { printf '%s' "${1%%$'\n'*}"; }

# Two calls in a row must each remove their own temporary directory, and
# neither may touch the EXIT trap of the caller.
test_repeated_calls_clean_up_and_keep_the_caller_trap() {
  local tmproot="${SANDBOX}/repeat-tmp"
  mkdir -p "${tmproot}"

  local child="${SANDBOX}/repeat-child.sh"
  {
    echo 'set -uo pipefail'
    echo "${CHILD_PREAMBLE}"
    cat <<'CHILD'
# Sourcing the helper turns errexit on. Turn it off again here, so that a
# return value can be observed instead of ending this shell.
set +e

trap 'echo CALLER_EXIT_TRAP_RAN' EXIT
before="$(trap -p EXIT)"
export TMPDIR="$2"
export CUDNN_VERSION="$3"
for call in 1 2; do
  ensure_cudnn_available 2>/dev/null
  echo "CALL_${call}_RETURNED_$?"
done
after="$(trap -p EXIT)"
if [[ "${before}" == "${after}" ]]; then
  echo EXIT_TRAP_UNCHANGED
else
  echo "EXIT_TRAP_CHANGED [${before}] -> [${after}]"
fi
CHILD
  } > "${child}"

  local out
  out="$(bash "${child}" "${FEDORA_SETUP}" "${tmproot}" "${FAKE_CUDNN_VERSION}" \
    2>/dev/null)"

  local marker
  for marker in CALL_1_RETURNED_1 CALL_2_RETURNED_1 EXIT_TRAP_UNCHANGED \
    CALLER_EXIT_TRAP_RAN; do
    if [[ "${out}" != *"${marker}"* ]]; then
      t_fail "expected ${marker} in the repeated calls, got: $(first_line "${out}")"
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
  {
    echo 'set -euo pipefail'
    echo "${CHILD_PREAMBLE}"
    cat <<'CHILD'
trap 'echo CALLER_EXIT_TRAP_RAN' EXIT
export TMPDIR="$2"
export CUDNN_VERSION="$3"
ensure_cudnn_available
echo NOT_REACHED
CHILD
  } > "${child}"

  local out rc=0
  out="$(STUB_CURL_FAIL=1 bash "${child}" "${FEDORA_SETUP}" "${tmproot}" \
    "${FAKE_CUDNN_VERSION}" 2>/dev/null)" || rc=$?

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

# The bootstrap root holds its libraries in lib or in lib64, so the CUDA
# preflight must follow the layout of the root it finds.
test_cuda_required_libs_follows_the_root_layout() {
  local child="${SANDBOX}/libdir-child.sh"
  {
    echo 'set -uo pipefail'
    echo "${CHILD_PREAMBLE}"
    cat <<'CHILD'
fake_root="$2"
sc_ort_installed_root() { printf '%s\n' "${fake_root}"; }
cuda_required_libs | grep '^libcufake' || echo NO_PROBE_RESULT
CHILD
  } > "${child}"

  # A stub objdump that names the directory it was pointed at.
  cat > "${STUB_BIN}/objdump" <<'STUB'
#!/usr/bin/env bash
for last; do :; done
echo "  NEEDED               libcufake_$(basename "$(dirname "${last}")").so.1"
STUB
  chmod +x "${STUB_BIN}/objdump"

  local layout root out
  for layout in lib lib64; do
    root="${SANDBOX}/ort-root-${layout}"
    mkdir -p "${root}/${layout}"
    : > "${root}/${layout}/libonnxruntime_providers_cuda.so"
    out="$(bash "${child}" "${FEDORA_SETUP}" "${root}" 2>/dev/null)"
    if [[ "${out}" != *"libcufake_${layout}.so.1"* ]]; then
      t_fail "a ${layout} root should be probed, got: $(first_line "${out}")"
    else
      t_pass "a ${layout} root is probed"
    fi
  done

  rm -f "${STUB_BIN}/objdump"
}

test_repeated_calls_clean_up_and_keep_the_caller_trap
test_download_failure_cleans_up_and_runs_the_caller_trap
test_cuda_required_libs_follows_the_root_layout

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
