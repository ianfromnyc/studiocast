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
#
# This text is a script for the child shell, so the quotes must keep "$1" as
# it is. The child expands it against its own arguments.
# shellcheck disable=SC2016  # the child shell expands this, not this shell
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

# More than one bootstrap root can be installed at a time. The helper must use
# the root the options name, not the newest root on disk.
test_the_options_pick_the_bootstrap_root() {
  local base="${SANDBOX}/root-choice"
  local older="${base}/1.28.0/onnxruntime-linux-x64-gpu_cuda12-1.28.0"
  local newer="${base}/1.29.0/onnxruntime-linux-x64-gpu_cuda13-1.29.0"
  mkdir -p "${older}/include" "${newer}/include"
  : > "${older}/include/onnxruntime_cxx_api.h"
  : > "${newer}/include/onnxruntime_cxx_api.h"

  local child="${SANDBOX}/root-choice-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
export ORT_FLAVOR=gpu
export ORT_ARCH=x64
export CUDA_MAJOR=12
# shellcheck source=/dev/null
source "$1"
log() { :; }
warn() { :; }
run_priv() { :; }
ORT_VERSION=1.28.0
echo "REQUESTED_[$(requested_onnxruntime_root)]"
echo "SELECTED_[$(installed_onnxruntime_root)]"
CHILD

  local out
  out="$(STUDIOCAST_ORT_INSTALL_DIR="${base}" bash "${child}" "${FEDORA_SETUP}" \
    2>/dev/null)"

  if [[ "${out}" != *"REQUESTED_[${older}]"* ]]; then
    t_fail "the options should name the 1.28.0 cuda12 root, got: ${out}"
  else
    t_pass "the options name the root they ask for"
  fi

  if [[ "${out}" != *"SELECTED_[${older}]"* ]]; then
    t_fail "the root of the options should win over the newest root, got: ${out}"
  else
    t_pass "the root of the options wins over the newest root"
  fi
}

# The default version is a selection too: a run with no options installs the
# default and must then build against it, not against a newer root that some
# earlier run left on disk.
test_the_default_version_picks_its_own_root() {
  local base="${SANDBOX}/root-default"
  local default_root="${base}/1.29.0/onnxruntime-linux-x64-gpu_cuda13-1.29.0"
  local newer="${base}/1.30.0/onnxruntime-linux-x64-gpu_cuda13-1.30.0"
  mkdir -p "${default_root}/include" "${newer}/include"
  : > "${default_root}/include/onnxruntime_cxx_api.h"
  : > "${newer}/include/onnxruntime_cxx_api.h"

  local child="${SANDBOX}/root-default-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
export ORT_FLAVOR=gpu
export ORT_ARCH=x64
export CUDA_MAJOR=13
# shellcheck source=/dev/null
source "$1"
log() { :; }
warn() { :; }
run_priv() { :; }
echo "SELECTED_[$(installed_onnxruntime_root)]"
CHILD

  local out
  out="$(STUDIOCAST_ORT_INSTALL_DIR="${base}" bash "${child}" "${FEDORA_SETUP}" \
    2>/dev/null)"

  if [[ "${out}" != *"SELECTED_[${default_root}]"* ]]; then
    t_fail "the default version should name its own root, got: ${out}"
  else
    t_pass "the default version picks its own root"
  fi
}

# The cpu flavor installs no bootstrap root at all, so no option of a cpu run
# asks for one. A gpu asset name would only give the build step advice about a
# root that this run never creates.
test_the_cpu_flavor_asks_for_no_bootstrap_root() {
  local child="${SANDBOX}/root-cpu-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
export ORT_FLAVOR=cpu
export ORT_ARCH=x64
export CUDA_MAJOR=13
# shellcheck source=/dev/null
source "$1"
log() { :; }
warn() { :; }
run_priv() { :; }
ORT_VERSION=1.28.0
echo "REQUESTED_[$(requested_onnxruntime_root)]"
CHILD

  local out
  out="$(STUDIOCAST_ORT_INSTALL_DIR="${SANDBOX}/root-cpu" \
    bash "${child}" "${FEDORA_SETUP}" 2>/dev/null)"

  if [[ "${out}" != *"REQUESTED_[]"* ]]; then
    t_fail "the cpu flavor should ask for no bootstrap root, got: ${out}"
  else
    t_pass "the cpu flavor asks for no bootstrap root"
  fi
}

# The flavor gets its default below the source guard, together with the system
# probe that finds it. A shell that sources the helper, which is what the guard
# is for, has no flavor at all. Reading one must not stop that shell under
# "set -u": with no flavor there is no gpu run, and thus no bootstrap root.
#
# The arch and the CUDA major get their defaults below the guard as well, so a
# sourced caller that sets a gpu flavor and nothing else must not stop there
# either. The root such a caller gets is not a useful one; the point is that
# the shell stays alive to see it.
test_a_sourced_caller_needs_no_flavor() {
  local child="${SANDBOX}/no-flavor-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
# shellcheck source=/dev/null
source "$1"
log() { :; }
warn() { :; }
run_priv() { :; }
echo "REQUESTED_[$(requested_onnxruntime_root)]"
CHILD

  local out rc=0
  out="$(env -u ORT_FLAVOR -u ORT_ARCH -u CUDA_MAJOR \
    STUDIOCAST_ORT_INSTALL_DIR="${SANDBOX}/no-flavor" \
    bash "${child}" "${FEDORA_SETUP}" 2>&1)" || rc=$?

  if [[ "${out}" == *"unbound variable"* ]]; then
    t_fail "a sourced caller without a flavor should get no error, got: ${out}"
  elif [[ "${rc}" -ne 0 ]]; then
    t_fail "a sourced caller without a flavor ended with status ${rc}: ${out}"
  elif [[ "${out}" != *"REQUESTED_[]"* ]]; then
    t_fail "no flavor should ask for no bootstrap root, got: ${out}"
  else
    t_pass "a sourced caller without a flavor asks for no bootstrap root"
  fi

  rc=0
  out="$(env -u ORT_ARCH -u CUDA_MAJOR ORT_FLAVOR=gpu \
    STUDIOCAST_ORT_INSTALL_DIR="${SANDBOX}/no-flavor" \
    bash "${child}" "${FEDORA_SETUP}" 2>&1)" || rc=$?

  if [[ "${out}" == *"unbound variable"* ]]; then
    t_fail "a sourced caller with only a flavor should get no error, got: ${out}"
  elif [[ "${rc}" -ne 0 ]]; then
    t_fail "a sourced caller with only a flavor ended with status ${rc}: ${out}"
  else
    t_pass "a sourced caller with only a flavor keeps its shell alive"
  fi
}

# Only a CUDA major the user passed can be a bad option value. A major that
# comes from the toolkit on the machine belongs to the report, not to the
# option check: the cpu flavor never reads it, and the message would name an
# option the user never passed.
#
# CUDA_MAJOR from the environment takes the same path as the detected one, so
# the checks below set it instead of faking a toolkit.
test_only_an_explicit_cuda_major_is_an_option_error() {
  local os_release="${SANDBOX}/os-release"
  cat > "${os_release}" <<'RELEASE'
ID=fedora
VERSION_ID=44
RELEASE

  local empty_roots="${SANDBOX}/no-bootstrap"
  mkdir -p "${empty_roots}"

  local out rc=0
  out="$(CUDA_MAJOR=11 STUDIOCAST_OS_RELEASE="${os_release}" \
    STUDIOCAST_ORT_INSTALL_DIR="${empty_roots}" \
    bash "${FEDORA_SETUP}" --onnxruntime-flavor cpu 2>&1)" || rc=$?
  if [[ "${rc}" -ne 0 ]]; then
    t_fail "a detected CUDA major stopped the cpu path with status ${rc}: $(first_line "${out}")"
  else
    t_pass "a detected CUDA major leaves the cpu path running"
  fi

  rc=0
  out="$(CUDA_MAJOR=11 STUDIOCAST_OS_RELEASE="${os_release}" \
    STUDIOCAST_ORT_INSTALL_DIR="${empty_roots}" \
    bash "${FEDORA_SETUP}" --check-cuda 2>&1)" || rc=$?
  if [[ "${rc}" -eq 0 ]]; then
    t_fail "an unsupported CUDA major should make the preflight fail"
  elif [[ "${out}" != *"FAIL  CUDA major 11"* ]]; then
    t_fail "the preflight should report the CUDA major, got: ${out}"
  else
    t_pass "the preflight reports an unsupported CUDA major as a failure"
  fi

  rc=0
  out="$(STUDIOCAST_OS_RELEASE="${os_release}" \
    STUDIOCAST_ORT_INSTALL_DIR="${empty_roots}" \
    bash "${FEDORA_SETUP}" --cuda-major 11 2>&1)" || rc=$?
  if [[ "${rc}" -ne 2 ]]; then
    t_fail "--cuda-major 11 should end with status 2, got ${rc}"
  elif [[ "${out}" != *"--cuda-major must be one of: 12|13"* ]]; then
    t_fail "--cuda-major 11 should name the option, got: $(first_line "${out}")"
  else
    t_pass "--cuda-major 11 is an option error"
  fi
}

# An option that takes a value must say so when the value is missing. Without
# a check, "shift 2" on the last argument fails and set -e ends the script with
# no output at all.
test_an_option_without_a_value_says_so() {
  local option out rc
  for option in --video-nr --build-type --cuda-major; do
    rc=0
    out="$(bash "${FEDORA_SETUP}" "${option}" 2>&1)" || rc=$?
    if [[ "${rc}" -ne 2 ]]; then
      t_fail "${option} without a value should end with status 2, got ${rc}"
    elif [[ "${out}" != *"${option} needs a value"* ]]; then
      t_fail "${option} without a value should say so, got: $(first_line "${out}")"
    else
      t_pass "${option} without a value says so"
    fi
  done
}

# CMake reads the bootstrap through pkg-config, so the preflight must ask
# pkg-config. On Fedora the .pc file lies outside the pkg-config search path,
# so the file alone says nothing about whether a build can find it.
test_the_preflight_asks_pkg_config() {
  local casedir mode out rc
  for mode in resolves missing_module missing_file; do
    casedir="${SANDBOX}/pkgconfig-preflight-${mode}"
    mkdir -p "${casedir}/bin" "${casedir}/pkgconfig"

    local pc_file="${casedir}/pkgconfig/onnxruntime.pc"
    if [[ "${mode}" != "missing_file" ]]; then
      printf 'Name: onnxruntime\n' > "${pc_file}"
    fi

    cat > "${casedir}/bin/pkg-config" <<STUB
#!/usr/bin/env bash
if [[ "\$1" == "--exists" ]]; then
  [[ "${mode}" == "resolves" ]]
  exit \$?
fi
if [[ "\$1" == "--modversion" ]]; then
  echo 1.29.0
fi
exit 0
STUB
    chmod +x "${casedir}/bin/pkg-config"

    local child="${SANDBOX}/pkgconfig-preflight-child.sh"
    cat > "${child}" <<'CHILD'
set -uo pipefail
export ORT_FLAVOR=cpu
export CUDA_MAJOR=13
export ORT_ARCH=x64
# shellcheck source=/dev/null
source "$1"
report_pkgconfig_check "$2"
echo "CHECK_RETURNED_$?"
CHILD

    rc=0
    out="$(PATH="${casedir}/bin:${PATH}" bash "${child}" "${FEDORA_SETUP}" \
      "${pc_file}" 2>&1)" || rc=$?

    case "${mode}" in
      resolves)
        if [[ "${out}" != *"PASS"* || "${out}" != *CHECK_RETURNED_0* ]]; then
          t_fail "a pkg-config that resolves onnxruntime should pass, got: ${out}"
        else
          t_pass "a pkg-config that resolves onnxruntime passes"
        fi ;;
      missing_module)
        if [[ "${out}" != *"FAIL"* || "${out}" != *CHECK_RETURNED_1* ]]; then
          t_fail "a .pc file pkg-config cannot see should fail, got: ${out}"
        else
          t_pass "a .pc file pkg-config cannot see fails"
        fi ;;
      missing_file)
        if [[ "${out}" != *"FAIL"* || "${out}" != *CHECK_RETURNED_1* ]]; then
          t_fail "a missing .pc file should fail, got: ${out}"
        else
          t_pass "a missing .pc file fails"
        fi ;;
    esac
  done
}

# The cuDNN archive is about 850 MB. An extracted tree whose ld.so.conf.d entry
# is gone, which is what ./scripts/uninstall.sh --greedy leaves, must not be
# downloaded again. The curl stub fails here, so a call that returns 0 proves
# that no download ran.
test_an_extracted_cudnn_tree_is_not_downloaded_again() {
  local base="${SANDBOX}/cudnn-installed"
  local root="${base}/${FAKE_CUDNN_VERSION}/cudnn-linux-x86_64-${FAKE_CUDNN_VERSION}_cuda13-archive"
  mkdir -p "${root}/lib"
  : > "${root}/lib/libcudnn.so.9"

  local tmproot="${SANDBOX}/cudnn-installed-tmp"
  mkdir -p "${tmproot}"

  local child="${SANDBOX}/cudnn-installed-child.sh"
  {
    echo 'set -euo pipefail'
    echo "${CHILD_PREAMBLE}"
    cat <<'CHILD'
export TMPDIR="$2"
export CUDNN_VERSION="$3"
ensure_cudnn_available
echo CALL_RETURNED_0
CHILD
  } > "${child}"

  local out rc=0
  out="$(STUB_CURL_FAIL=1 STUDIOCAST_CUDNN_INSTALL_DIR="${base}" \
    bash "${child}" "${FEDORA_SETUP}" "${tmproot}" "${FAKE_CUDNN_VERSION}" \
    2>/dev/null)" || rc=$?

  if [[ "${rc}" -ne 0 ]]; then
    t_fail "an extracted cuDNN tree should need no download, status ${rc}"
  elif [[ "${out}" != *CALL_RETURNED_0* ]]; then
    t_fail "an extracted cuDNN tree should end the call with 0, got: $(first_line "${out}")"
  else
    t_pass "an extracted cuDNN tree is not downloaded again"
  fi

  local leftovers
  leftovers="$(leftover_entries "${tmproot}")"
  if [[ -n "${leftovers}" ]]; then
    t_fail "the skip path made a temporary directory: ${leftovers}"
  else
    t_pass "the skip path makes no temporary directory"
  fi
}

# A version from before the CUDA split has one gpu asset, which is not the
# CUDA major the user asked for. The warning must say that, because the CUDA
# runtime rpms of that major are installed next to it.
test_the_legacy_asset_warning_names_the_cuda_major() {
  local child="${SANDBOX}/legacy-asset-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
# shellcheck source=/dev/null
source "$1"
onnxruntime_gpu_asset_name x64 13 1.24.0
CHILD

  local out
  out="$(bash "${child}" "${FEDORA_SETUP}" 2>&1)"

  if [[ "${out}" != *"onnxruntime-linux-x64-gpu-1.24.0"* ]]; then
    t_fail "the legacy asset name should be used, got: ${out}"
  else
    t_pass "the legacy asset name is used"
  fi

  if [[ "${out}" != *"not CUDA 13"* ]]; then
    t_fail "the warning should say the build is not CUDA 13, got: ${out}"
  else
    t_pass "the warning says the build is not CUDA 13"
  fi
}

# The question is whether the CUDA packages resolve, not what the repository
# that offers them is called. A repository with another id must work.
test_a_cuda_repo_with_another_id_works() {
  local casedir="${SANDBOX}/cuda-repo-id"
  mkdir -p "${casedir}/bin"

  cat > "${casedir}/bin/dnf" <<'STUB'
#!/usr/bin/env bash
case "$1" in
  repolist) echo "nvidia-cuda    NVIDIA CUDA" ;;
  repoquery) echo "cuda-cudart-13-3" ;;
esac
exit 0
STUB
  chmod +x "${casedir}/bin/dnf"

  local child="${SANDBOX}/cuda-repo-id-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
export ORT_FLAVOR=gpu
export CUDA_MAJOR=13
export ORT_ARCH=x64
# shellcheck source=/dev/null
source "$1"
run_priv() { :; }
lib_resolves() { return 1; }
cuda_search_dirs() { return 0; }
ensure_cuda_runtime
echo "CALL_RETURNED_$?"
CHILD

  local out rc=0
  out="$(PATH="${casedir}/bin:${PATH}" \
    STUDIOCAST_ORT_INSTALL_DIR="${casedir}/onnxruntime" \
    bash "${child}" "${FEDORA_SETUP}" 2>&1)" || rc=$?

  if [[ "${rc}" -ne 0 ]]; then
    t_fail "a CUDA repository with another id ended the call with status ${rc}: $(first_line "${out}")"
  elif [[ "${out}" != *"Installing the CUDA 13.3 runtime rpms"* ]]; then
    t_fail "the package suffix of the repository should be used, got: ${out}"
  else
    t_pass "a CUDA repository with another id works"
  fi
}

# A system without dnf cannot get the CUDA rpms. The helper must say so and
# stop with status 2, not end the whole script on a command that is not there.
test_a_missing_dnf_prints_the_repo_hint() {
  local casedir="${SANDBOX}/no-dnf"
  mkdir -p "${casedir}/bin"

  # A PATH with the tools the helper needs, but with no dnf on it.
  local tool
  for tool in bash cat cut dirname grep head mkdir mktemp printf rm sed sort \
    tail tr uname; do
    if command -v "${tool}" >/dev/null 2>&1; then
      ln -sf "$(command -v "${tool}")" "${casedir}/bin/${tool}"
    fi
  done

  local child="${SANDBOX}/no-dnf-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
export ORT_FLAVOR=gpu
export CUDA_MAJOR=13
export ORT_ARCH=x64
# shellcheck source=/dev/null
source "$1"
log() { :; }
run_priv() { :; }
cuda_required_libs() { printf 'libcublas.so.13\n'; }
lib_resolves() { return 1; }
# The helper turns these on after the source guard, so the call must run
# under them.
set -euo pipefail
ensure_cuda_runtime
echo "CALL_RETURNED_$?"
CHILD

  local out rc=0
  out="$(PATH="${casedir}/bin" bash "${child}" "${FEDORA_SETUP}" 2>&1)" || rc=$?

  if [[ "${rc}" -ne 2 ]]; then
    t_fail "a missing dnf should stop with status 2, got ${rc}: ${out}"
  elif [[ "${out}" != *"config-manager addrepo"* ]]; then
    t_fail "a missing dnf should print the repository hint, got: ${out}"
  else
    t_pass "a missing dnf prints the repository hint"
  fi
}

# A dnf that is there but fails is the more usual trouble: repository metadata
# that does not download, no network, a locked rpmdb. That must read as "no
# package anywhere", not end the whole script without a word. The helper must
# show what dnf said, print the hint and stop with status 2.
test_a_failing_dnf_prints_the_repo_hint() {
  local casedir="${SANDBOX}/dnf-fails"
  mkdir -p "${casedir}/bin"

  cat > "${casedir}/bin/dnf" <<'STUB'
#!/usr/bin/env bash
echo "dnf stub: cannot download repository metadata" >&2
exit 1
STUB
  chmod +x "${casedir}/bin/dnf"

  local child="${SANDBOX}/dnf-fails-child.sh"
  cat > "${child}" <<'CHILD'
set -uo pipefail
export ORT_FLAVOR=gpu
export CUDA_MAJOR=13
export ORT_ARCH=x64
# shellcheck source=/dev/null
source "$1"
log() { :; }
run_priv() { :; }
cuda_required_libs() { printf 'libcublas.so.13\n'; }
lib_resolves() { return 1; }
# The helper turns these on after the source guard, so the call must run
# under them.
set -euo pipefail
ensure_cuda_runtime
echo "CALL_RETURNED_$?"
CHILD

  local out rc=0
  out="$(PATH="${casedir}/bin:${PATH}" \
    STUDIOCAST_ORT_INSTALL_DIR="${casedir}/onnxruntime" \
    bash "${child}" "${FEDORA_SETUP}" 2>&1)" || rc=$?

  if [[ "${rc}" -ne 2 ]]; then
    t_fail "a failing dnf should stop with status 2, got ${rc}: ${out}"
  elif [[ "${out}" != *"cannot download repository metadata"* ]]; then
    t_fail "a failing dnf should show what dnf said, got: ${out}"
  elif [[ "${out}" != *"config-manager addrepo"* ]]; then
    t_fail "a failing dnf should print the repository hint, got: ${out}"
  else
    t_pass "a failing dnf shows the error and prints the repository hint"
  fi
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
test_the_options_pick_the_bootstrap_root
test_the_default_version_picks_its_own_root
test_the_cpu_flavor_asks_for_no_bootstrap_root
test_a_sourced_caller_needs_no_flavor
test_only_an_explicit_cuda_major_is_an_option_error
test_an_option_without_a_value_says_so
test_the_preflight_asks_pkg_config
test_an_extracted_cudnn_tree_is_not_downloaded_again
test_the_legacy_asset_warning_names_the_cuda_major
test_a_cuda_repo_with_another_id_works
test_a_missing_dnf_prints_the_repo_hint
test_a_failing_dnf_prints_the_repo_hint
test_sourcing_keeps_the_caller_shell_options

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
