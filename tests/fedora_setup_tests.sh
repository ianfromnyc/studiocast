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
# With STUB_CURL_INDEX set to a file, a call without -o prints that file, which
# is how a check below gives the lookup a real index to read.
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
if [[ -z "${out}" ]]; then
  if [[ -n "${STUB_CURL_INDEX:-}" ]]; then
    cat "${STUB_CURL_INDEX}"
  fi
  exit 0
fi
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

# The helper installs a RETURN trap of its own. With functrace on, a RETURN
# trap of the caller reaches the helper, so the helper must put it back the way
# it puts the ERR trap back. Otherwise the caller loses its own clean-up.
test_a_caller_return_trap_survives_the_call() {
  local tmproot="${SANDBOX}/return-trap-tmp"
  mkdir -p "${tmproot}"

  local child="${SANDBOX}/return-trap-child.sh"
  {
    echo 'set -uo pipefail'
    echo "${CHILD_PREAMBLE}"
    cat <<'CHILD'
# functrace passes the RETURN trap of the caller into every function it calls.
set -T

export TMPDIR="$2"
export CUDNN_VERSION="$3"

caller_with_a_return_trap() {
  trap 'echo CALLER_RETURN_TRAP_RAN' RETURN
  local before after
  before="$(trap -p RETURN)"
  ensure_cudnn_available 2>/dev/null
  after="$(trap -p RETURN)"
  if [[ "${before}" == "${after}" ]]; then
    echo RETURN_TRAP_UNCHANGED
  else
    echo "RETURN_TRAP_CHANGED [${before}] -> [${after}]"
  fi
}

caller_with_a_return_trap
CHILD
  } > "${child}"

  local out
  out="$(bash "${child}" "${FEDORA_SETUP}" "${tmproot}" "${FAKE_CUDNN_VERSION}" \
    2>/dev/null)"

  local marker
  for marker in RETURN_TRAP_UNCHANGED CALLER_RETURN_TRAP_RAN; do
    if [[ "${out}" != *"${marker}"* ]]; then
      t_fail "expected ${marker} in the RETURN trap check, got: $(first_line "${out}")"
    else
      t_pass "${marker}"
    fi
  done

  local leftovers
  leftovers="$(leftover_entries "${tmproot}")"
  if [[ -n "${leftovers}" ]]; then
    t_fail "temporary directory left behind by the RETURN trap check: ${leftovers}"
  else
    t_pass "the temporary directory was removed under a caller RETURN trap"
  fi
}

# A redistributable index can exist and still have no entry for the archive,
# for example when NVIDIA renames a file. That means "no published SHA-256",
# not an error. Under set -e the lookup must return 0 and print nothing, so
# that the caller can log the missing checksum and go on.
test_a_missing_index_entry_gives_an_empty_sha256() {
  local index="${SANDBOX}/redistrib-without-entry.json"
  cat > "${index}" <<'JSON'
{
  "release_date": "2025-01-01",
  "cudnn": {
    "linux-x86_64": {
      "relative_path": "cudnn/linux-x86_64/some-other-archive.tar.xz",
      "sha256": "1111111111111111111111111111111111111111111111111111111111111111"
    }
  }
}
JSON

  local child="${SANDBOX}/index-entry-child.sh"
  {
    echo 'set -euo pipefail'
    echo "${CHILD_PREAMBLE}"
    cat <<'CHILD'
export CUDNN_VERSION="$2"
sha256="$(cudnn_published_sha256 "$(cudnn_archive_name)")"
echo LOOKUP_RETURNED_0
echo "LOOKUP_OUTPUT_[${sha256}]"
CHILD
  } > "${child}"

  local out rc=0
  out="$(STUB_CURL_INDEX="${index}" bash "${child}" "${FEDORA_SETUP}" \
    "${FAKE_CUDNN_VERSION}" 2>/dev/null)" || rc=$?

  if [[ "${rc}" -ne 0 ]]; then
    t_fail "an index without the entry ended the script with status ${rc}"
  elif [[ "${out}" != *LOOKUP_RETURNED_0* ]]; then
    t_fail "the lookup did not return 0, got: $(first_line "${out}")"
  elif [[ "${out}" != *'LOOKUP_OUTPUT_[]'* ]]; then
    t_fail "the lookup should print nothing, got: $(first_line "${out}")"
  else
    t_pass "an index without the entry gives an empty SHA-256 and returns 0"
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

# The objdump probe only adds to the fixed list, so a provider objdump cannot
# read must not end the caller. Under set -e the call must return 0 and give
# back the fixed list.
test_a_failing_objdump_keeps_the_fixed_lib_list() {
  local child="${SANDBOX}/objdump-failure-child.sh"
  {
    echo 'set -euo pipefail'
    echo "${CHILD_PREAMBLE}"
    cat <<'CHILD'
fake_root="$2"
sc_ort_installed_root() { printf '%s\n' "${fake_root}"; }
libs="$(cuda_required_libs)"
echo PROBE_RETURNED_0
printf '%s\n' "${libs}"
CHILD
  } > "${child}"

  # A stub objdump that reports a file it cannot read.
  cat > "${STUB_BIN}/objdump" <<'STUB'
#!/usr/bin/env bash
echo "stub objdump: unrecognized file format" >&2
exit 1
STUB
  chmod +x "${STUB_BIN}/objdump"

  local root="${SANDBOX}/ort-root-objdump-failure"
  mkdir -p "${root}/lib"
  : > "${root}/lib/libonnxruntime_providers_cuda.so"

  local out rc=0
  out="$(bash "${child}" "${FEDORA_SETUP}" "${root}" 2>/dev/null)" || rc=$?

  rm -f "${STUB_BIN}/objdump"

  if [[ "${rc}" -ne 0 ]]; then
    t_fail "a failing objdump ended the script with status ${rc}"
    return
  fi
  if [[ "${out}" != *PROBE_RETURNED_0* ]]; then
    t_fail "the probe did not return 0, got: $(first_line "${out}")"
    return
  fi

  local soname
  for soname in libcuda.so.1 libcudart.so.13 libcublas.so.13 libcublasLt.so.13 \
    libcurand.so.10 libnvrtc.so.13 libcudnn.so.9; do
    if [[ "${out}" != *"${soname}"* ]]; then
      t_fail "a failing objdump dropped ${soname} from the fixed list"
      return
    fi
  done
  t_pass "a failing objdump keeps the fixed library list"
}

# The source guard promises definitions only. A shell that sources the helper
# must keep its own shell options, and must see no output.
test_sourcing_keeps_the_caller_shell_options() {
  local child="${SANDBOX}/source-guard-child.sh"
  cat > "${child}" <<'CHILD'
set +e
set +u
set +o pipefail
# shellcheck source=/dev/null
source "$1"
for opt in errexit nounset pipefail; do
  if [[ -o "${opt}" ]]; then
    echo "OPTION_ON_${opt}"
  else
    echo "OPTION_OFF_${opt}"
  fi
done
CHILD

  local out expected
  out="$(bash "${child}" "${FEDORA_SETUP}" 2>&1)"
  expected=$'OPTION_OFF_errexit\nOPTION_OFF_nounset\nOPTION_OFF_pipefail'
  if [[ "${out}" != "${expected}" ]]; then
    t_fail "sourcing the helper changed the shell or printed something: ${out}"
  else
    t_pass "sourcing the helper keeps the shell options of the caller"
  fi
}

test_repeated_calls_clean_up_and_keep_the_caller_trap
test_download_failure_cleans_up_and_runs_the_caller_trap
test_a_caller_return_trap_survives_the_call
test_a_missing_index_entry_gives_an_empty_sha256
test_cuda_required_libs_follows_the_root_layout
test_a_failing_objdump_keeps_the_fixed_lib_list
test_sourcing_keeps_the_caller_shell_options

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
