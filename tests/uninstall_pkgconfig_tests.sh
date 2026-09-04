#!/usr/bin/env bash
#
# Offline checks for the onnxruntime.pc clean-up in
# scripts/uninstall/uninstall.sh, and for what sourcing that file does.
#
# The script is sourced, never run, so no uninstall step happens here. Only the
# two helpers below are called, and they work on a sandbox directory with a
# stub pkg-config and a stub sudo on PATH.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
UNINSTALL="${REPO_ROOT}/scripts/uninstall/uninstall.sh"

FAILURES=0
t_fail() {
  echo "FAIL: $*" >&2
  FAILURES=$((FAILURES + 1))
}
t_pass() { echo "ok: $*"; }

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT

STUB_BIN="${SANDBOX}/bin"
mkdir -p "${STUB_BIN}"

# A pkg-config that reports the search path in STUB_PC_PATH, and nothing else.
cat > "${STUB_BIN}/pkg-config" <<'STUB'
#!/usr/bin/env bash
if [[ "$1" == "--variable" && "$2" == "pc_path" ]]; then
  printf '%s\n' "${STUB_PC_PATH:-}"
  exit 0
fi
exit 1
STUB
chmod +x "${STUB_BIN}/pkg-config"

# A sudo that just runs the command, so the checks need no privileges.
cat > "${STUB_BIN}/sudo" <<'STUB'
#!/usr/bin/env bash
exec "$@"
STUB
chmod +x "${STUB_BIN}/sudo"

export PATH="${STUB_BIN}:${PATH}"

# shellcheck source=../scripts/uninstall/uninstall.sh
source "${UNINSTALL}"

# The link goes wherever pkg-config searches, so the clean-up must look there.
test_link_dirs_come_from_pkg_config() {
  local -a got=()
  export STUB_PC_PATH="/opt/first/pkgconfig:/opt/second/pkgconfig"
  mapfile -t got < <(onnxruntime_pc_link_dirs)
  unset STUB_PC_PATH

  local joined="${got[*]}"
  if [[ "${joined}" != "/opt/first/pkgconfig /opt/second/pkgconfig" ]]; then
    t_fail "expected the pkg-config search path, got [${joined}]"
  else
    t_pass "the link directories come from the pkg-config search path"
  fi
}

# Without a usable pkg-config the common directories are all we have.
test_link_dirs_fall_back_to_the_common_directories() {
  local -a got=()
  export STUB_PC_PATH=""
  mapfile -t got < <(onnxruntime_pc_link_dirs)
  unset STUB_PC_PATH

  local joined="${got[*]}"
  if [[ "${joined}" != "/usr/lib64/pkgconfig /usr/lib/pkgconfig" ]]; then
    t_fail "expected the common directories, got [${joined}]"
  else
    t_pass "the link directories fall back to the common directories"
  fi
}

# Only a symlink that points at our own .pc file may go.
test_only_our_own_symlink_is_removed() {
  local work="${SANDBOX}/links"
  local pc_file="${work}/bootstrap/onnxruntime.pc"
  mkdir -p "${work}/bootstrap" "${work}/ours" "${work}/other" "${work}/distro"
  : > "${pc_file}"
  : > "${work}/bootstrap/other.pc"

  ln -s "${pc_file}" "${work}/ours/onnxruntime.pc"
  ln -s "${work}/bootstrap/other.pc" "${work}/other/onnxruntime.pc"
  : > "${work}/distro/onnxruntime.pc"

  remove_onnxruntime_pc_links "${pc_file}" \
    "${work}/ours" "${work}/other" "${work}/distro" "${work}/absent" >/dev/null

  if [[ -e "${work}/ours/onnxruntime.pc" || -L "${work}/ours/onnxruntime.pc" ]]; then
    t_fail "the link to the bootstrap file was not removed"
  else
    t_pass "the link to the bootstrap file was removed"
  fi

  if [[ ! -L "${work}/other/onnxruntime.pc" ]]; then
    t_fail "a link to another file was removed"
  else
    t_pass "a link to another file was kept"
  fi

  if [[ ! -f "${work}/distro/onnxruntime.pc" ]]; then
    t_fail "a real file owned by the distribution was removed"
  else
    t_pass "a real file owned by the distribution was kept"
  fi
}

# Sourcing the file must only define things. A caller that sources it keeps its
# own arguments and its own shell options, and sees no output.
test_sourcing_has_no_side_effects() {
  local probe="${SANDBOX}/source_probe.sh"
  local report="${SANDBOX}/source_probe.report"
  local noise="${SANDBOX}/source_probe.noise"

  cat > "${probe}" <<'PROBE'
#!/usr/bin/env bash
# Source the uninstall script with arguments in place, then report what the
# sourcing changed in this shell.
uninstall_path="$1"
report_path="$2"
noise_path="$3"

set -- --greedy --foo

# shellcheck source=/dev/null
source "${uninstall_path}" >"${noise_path}" 2>&1

{
  printf 'args=%s\n' "$*"
  set -o | awk '$1 == "errexit" { print "errexit=" $2 }'
} > "${report_path}"
PROBE

  : > "${report}"
  : > "${noise}"

  local probe_status=0
  bash "${probe}" "${UNINSTALL}" "${report}" "${noise}" || probe_status="$?"

  if [[ "${probe_status}" -ne 0 ]]; then
    t_fail "sourcing ended the caller (exit ${probe_status}): $(tr '\n' ' ' < "${noise}")"
    return
  fi

  local args_line errexit_line
  args_line="$(grep '^args=' "${report}")"
  errexit_line="$(grep '^errexit=' "${report}")"

  if [[ "${args_line}" != "args=--greedy --foo" ]]; then
    t_fail "sourcing parsed the caller arguments, got [${args_line}]"
  else
    t_pass "sourcing leaves the caller arguments alone"
  fi

  if [[ "${errexit_line}" != "errexit=off" ]]; then
    t_fail "sourcing changed the caller shell options, got [${errexit_line}]"
  else
    t_pass "sourcing leaves errexit off in the caller"
  fi

  if [[ -s "${noise}" ]]; then
    t_fail "sourcing printed: $(tr '\n' ' ' < "${noise}")"
  else
    t_pass "sourcing prints nothing"
  fi
}

test_link_dirs_come_from_pkg_config
test_link_dirs_fall_back_to_the_common_directories
test_only_our_own_symlink_is_removed
test_sourcing_has_no_side_effects

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
