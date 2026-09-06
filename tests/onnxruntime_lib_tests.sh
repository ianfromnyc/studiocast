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

# The helper installs a RETURN trap of its own. With functrace on, a RETURN
# trap of the caller reaches the helper, so the helper must put it back the way
# it puts the ERR trap back. Otherwise the caller loses its own clean-up.
test_a_caller_return_trap_survives_the_call() {
  local tmproot="${SANDBOX}/return-trap-tmp"
  mkdir -p "${tmproot}"

  local child="${SANDBOX}/return-trap-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
# functrace passes the RETURN trap of the caller into every function it calls.
set -T
sc_ort_log() { :; }
sc_ort_priv() { :; }
# shellcheck source=/dev/null
source "$1"
export TMPDIR="$2"
VERSION="$3"
ASSET="$4"
URL="$5"

caller_with_a_return_trap() {
  trap 'echo CALLER_RETURN_TRAP_RAN' RETURN
  local before after
  before="$(trap -p RETURN)"
  sc_ort_install_tarball "${VERSION}" "${ASSET}" "${URL}" 2>/dev/null || true
  after="$(trap -p RETURN)"
  if [[ "${before}" == "${after}" ]]; then
    echo RETURN_TRAP_UNCHANGED
  else
    echo "RETURN_TRAP_CHANGED [${before}] -> [${after}]"
  fi
}

caller_with_a_return_trap
CHILD

  local out
  out="$(bash "${child}" "${ORT_LIB}" "${tmproot}" \
    "${FAKE_VERSION}" "${FAKE_ASSET}" "${FAKE_URL}" 2>/dev/null)"

  local marker
  for marker in RETURN_TRAP_UNCHANGED CALLER_RETURN_TRAP_RAN; do
    if [[ "${out}" != *"${marker}"* ]]; then
      t_fail "expected ${marker} in the output of the RETURN trap check: ${out}"
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

# A bootstrap root keeps its libraries in lib or in lib64. Everything that
# looks inside a root must follow the layout of that root.
test_libdir_follows_the_layout_of_the_root() {
  # Hooks the library expects from its caller. Nothing below calls them.
  sc_ort_log() { :; }
  sc_ort_priv() { :; }
  # shellcheck source-path=SCRIPTDIR
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

# More than one bootstrap root can be installed at a time. A caller that names
# the root it wants must get that one, and a caller that names none must get
# the newest one.
test_a_preferred_root_wins_over_the_newest_root() {
  local base="${SANDBOX}/installed-roots"
  local older="${base}/1.28.0/onnxruntime-linux-x64-gpu_cuda12-1.28.0"
  local newer="${base}/1.29.0/onnxruntime-linux-x64-gpu_cuda13-1.29.0"
  mkdir -p "${older}/include" "${newer}/include"
  : > "${older}/include/onnxruntime_cxx_api.h"
  : > "${newer}/include/onnxruntime_cxx_api.h"

  local child="${SANDBOX}/installed-root-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
sc_ort_log() { :; }
sc_ort_priv() { :; }
# shellcheck source=/dev/null
source "$1"
echo "PREFERRED_[$(sc_ort_installed_root "$2")]"
echo "NEWEST_[$(sc_ort_installed_root)]"
echo "MISSING_[$(sc_ort_installed_root "$3")]"
CHILD

  local out
  out="$(STUDIOCAST_ORT_INSTALL_DIR="${base}" bash "${child}" "${ORT_LIB}" \
    "${older}" "${base}/9.9.9/not-installed" 2>/dev/null)"

  if [[ "${out}" != *"PREFERRED_[${older}]"* ]]; then
    t_fail "a preferred root should win, got: ${out}"
  else
    t_pass "a preferred root wins over the newest root"
  fi

  if [[ "${out}" != *"NEWEST_[${newer}]"* ]]; then
    t_fail "without a preferred root the newest one should win, got: ${out}"
  else
    t_pass "without a preferred root the newest one wins"
  fi

  if [[ "${out}" != *"MISSING_[${newer}]"* ]]; then
    t_fail "a preferred root that is not installed should fall back to the newest one, got: ${out}"
  else
    t_pass "a preferred root that is not installed falls back to the newest one"
  fi
}

# The .so symlink must point at the newest library in the directory. Version
# order is not text order: 1.10.0 is newer than 1.9.0, and comes first in a
# plain sort.
test_the_newest_so_is_picked_by_version() {
  local child="${SANDBOX}/newest-so-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
sc_ort_log() { :; }
sc_ort_priv() { :; }
# shellcheck source=/dev/null
source "$1"
echo "NEWEST_[$(sc_ort_newest_so "$2")]"
echo "EMPTY_[$(sc_ort_newest_so "$3")]"
CHILD

  local libdir="${SANDBOX}/so-versions"
  local emptydir="${SANDBOX}/so-none"
  mkdir -p "${libdir}" "${emptydir}"
  : > "${libdir}/libonnxruntime.so.1.9.0"
  : > "${libdir}/libonnxruntime.so.1.10.0"

  local out
  out="$(bash "${child}" "${ORT_LIB}" "${libdir}" "${emptydir}" 2>/dev/null)"

  if [[ "${out}" != *"NEWEST_[${libdir}/libonnxruntime.so.1.10.0]"* ]]; then
    t_fail "the newest library should be picked by version, got: ${out}"
  else
    t_pass "the newest library is picked by version"
  fi

  if [[ "${out}" != *'EMPTY_[]'* ]]; then
    t_fail "a directory without a library should give nothing, got: ${out}"
  else
    t_pass "a directory without a library gives nothing"
  fi
}

# Linking onnxruntime.pc is best effort. A pkg-config that fails, and a
# pkg-config that names no search path, must both leave the caller running
# under set -e.
test_pkgconfig_link_survives_a_broken_pkg_config() {
  local child="${SANDBOX}/pkgconfig-link-child.sh"
  cat > "${child}" <<'CHILD'
set -euo pipefail
sc_ort_log() { :; }
sc_ort_priv() { :; }
# shellcheck source=/dev/null
source "$1"
sc_ort_link_pkgconfig "$2"
echo HELPER_RETURNED_0
echo CALLER_CONTINUED
CHILD

  local mode
  for mode in fail empty; do
    local casedir="${SANDBOX}/pkgconfig-${mode}"
    mkdir -p "${casedir}/bin" "${casedir}/pkgconfig"

    local pc_file="${casedir}/pkgconfig/onnxruntime.pc"
    printf 'Name: onnxruntime\n' > "${pc_file}"

    # --exists must report the module missing, so the helper goes on to link.
    # Every other call either fails, as it does without a pkg-config.pc file,
    # or prints nothing.
    cat > "${casedir}/bin/pkg-config" <<STUB
#!/usr/bin/env bash
if [[ "\$1" == "--exists" ]]; then
  exit 1
fi
if [[ "${mode}" == "fail" ]]; then
  echo "stub pkg-config: no pkg-config.pc here" >&2
  exit 1
fi
exit 0
STUB
    chmod +x "${casedir}/bin/pkg-config"

    local out rc=0
    out="$(PATH="${casedir}/bin:${PATH}" bash "${child}" "${ORT_LIB}" \
      "${pc_file}" 2>/dev/null)" || rc=$?

    if [[ "${rc}" -ne 0 ]]; then
      t_fail "the ${mode} pkg-config stub ended the caller with status ${rc}"
    elif [[ "${out}" != *HELPER_RETURNED_0* ]] ||
      [[ "${out}" != *CALLER_CONTINUED* ]]; then
      t_fail "the ${mode} pkg-config stub stopped the caller: ${out}"
    else
      t_pass "the ${mode} pkg-config stub leaves the caller running"
    fi
  done
}

test_repeated_calls_clean_up_and_keep_the_caller_trap
test_download_failure_cleans_up_and_runs_the_caller_trap
test_a_caller_return_trap_survives_the_call
test_libdir_follows_the_layout_of_the_root
test_a_preferred_root_wins_over_the_newest_root
test_the_newest_so_is_picked_by_version
test_pkgconfig_link_survives_a_broken_pkg_config

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
