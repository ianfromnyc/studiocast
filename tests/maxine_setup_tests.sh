#!/usr/bin/env bash
#
# Offline checks for the helper functions of scripts/setup/maxine.sh.
#
# That script runs its work at the top level, so it cannot be sourced. Each
# check therefore takes the one function it needs out of the file and runs it
# in a child shell. Nothing here reaches the network or the SDK.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MAXINE_SH="${REPO_ROOT}/scripts/setup/maxine.sh"

FAILURES=0
t_fail() {
  echo "FAIL: $*" >&2
  FAILURES=$((FAILURES + 1))
}
t_pass() { echo "ok: $*"; }

SANDBOX="$(mktemp -d)"
trap 'rm -rf "${SANDBOX}"' EXIT

# Print one shell function of maxine.sh. Functions there start at column 0 and
# end with a closing brace at column 0.
extract_function() {
  sed -n "/^$1() {\$/,/^}\$/p" "${MAXINE_SH}"
}

# Write a child script holding one function of maxine.sh, which it calls with
# the four arguments the child itself is given.
#
# Arguments: <function name> <child script path>
write_child() {
  local fn
  fn="$(extract_function "$1")"
  [[ -n "${fn}" ]] || return 1
  {
    echo 'set -uo pipefail'
    echo "${fn}"
    # The positional parameters below belong to the child, not to this shell,
    # so they must reach the file as they are written here.
    # shellcheck disable=SC2016
    printf '%s "$1" "$2" "$3" "$4"\n' "$1"
  } > "$2"
}

# The SDK's install_feature.sh pins the root it installs into. The helper
# writes a copy that points at this install instead. Both roots come from the
# file system, so a root holding a character that a text substitution reads as
# something else must still come out as itself.
test_pinning_a_root_with_special_characters() {
  local child="${SANDBOX}/pin-child.sh"
  if ! write_child sdk_script_pin_root "${child}"; then
    t_fail "sdk_script_pin_root is not in ${MAXINE_SH}"
    return
  fi

  # '#' is the sed delimiter this used to use, '&' is its whole match, and a
  # backslash is its escape.
  local pinned='/usr/local/VideoFX'
  local root='/home/u/sc#1 & co\dir/VideoFX'

  local script="${SANDBOX}/install_feature.sh"
  {
    echo '#!/usr/bin/env bash'
    printf 'VFX_SDK_PATH="%s"\n' "${pinned}"
  } > "${script}"

  local out="${SANDBOX}/install_feature_local.sh"
  bash "${child}" "${script}" "${pinned}" "${root}" "${out}"

  local got
  got="$(sed -n 's/^VFX_SDK_PATH="\(.*\)"$/\1/p' "${out}")"
  if [[ "${got}" != "${root}" ]]; then
    t_fail "expected the pinned root '${root}', got '${got}'"
  else
    t_pass "a root with #, & and a backslash survives the copy"
  fi

  if [[ "$(head -n 1 "${out}")" != "#!/usr/bin/env bash" ]]; then
    t_fail "the copy lost the first line: $(head -n 1 "${out}")"
  else
    t_pass "the copy keeps the lines it does not change"
  fi
}

# A pinned root holding a regular expression character must match as text.
test_pinning_a_root_that_looks_like_a_pattern() {
  local child="${SANDBOX}/pin-re-child.sh"
  if ! write_child sdk_script_pin_root "${child}"; then
    t_fail "sdk_script_pin_root is not in ${MAXINE_SH}"
    return
  fi

  local pinned='/opt/v.1[x]/VideoFX'
  local root='/home/u/VideoFX'

  local script="${SANDBOX}/install_feature_re.sh"
  {
    printf 'VFX_SDK_PATH="%s"\n' "${pinned}"
    # A line the pinned root would match if it were read as a pattern.
    echo 'AR_SDK_PATH="/opt/vX1Xx/VideoFX"'
  } > "${script}"

  local out="${SANDBOX}/install_feature_re_local.sh"
  bash "${child}" "${script}" "${pinned}" "${root}" "${out}"

  if ! grep -q "^VFX_SDK_PATH=\"${root}\"\$" "${out}"; then
    t_fail "the pinned root was not replaced: $(cat "${out}")"
  else
    t_pass "a pinned root with regex characters is replaced"
  fi

  if ! grep -q '^AR_SDK_PATH="/opt/vX1Xx/VideoFX"$' "${out}"; then
    t_fail "a line that only matches as a pattern was changed: $(cat "${out}")"
  else
    t_pass "only the pinned root itself is replaced"
  fi
}

# docs/maxine_install.md says a dry run writes nothing. The script must
# therefore make no directory of its own under --base or --cache-dir. This
# runs with no key, so it stops at the refusal and reaches no network.
test_a_dry_run_makes_no_directory() {
  local base="${SANDBOX}/dry/base"
  local cache="${SANDBOX}/dry/cache"
  rm -rf "${SANDBOX}/dry"

  local out
  out="$(NGC_API_KEY="" NGC_CLI_API_KEY="" SC_NGC_API_KEY="" \
    bash "${MAXINE_SH}" --base "${base}" --cache-dir "${cache}" \
    --download vfx --dry-run 2>&1)"

  if [[ "${out}" != *"needs an NGC API key"* ]]; then
    t_fail "expected the no-key refusal, got: ${out}"
  else
    t_pass "a dry run without a key refuses and says why"
  fi

  local made
  made="$(find "${SANDBOX}/dry" 2>/dev/null | sort)"
  if [[ -n "${made}" ]]; then
    t_fail "a dry run made directories: ${made}"
  else
    t_pass "a dry run makes no directory"
  fi
}

# --download-features afx falls back to the NGC REST API when the SDK's own
# features/download_features.sh is not there. docs/SETUP.md documents
# --install-afx-features as the normal route, so it must take the same path
# instead of dying on a missing script.
test_install_afx_features_falls_back_to_the_rest_api() {
  local base="${SANDBOX}/afxfallback"
  rm -rf "${base}"

  # An AFX root with a features directory, but without the SDK script.
  mkdir -p "${base}/Audio_Effects_SDK/features"

  # A curl that answers nothing, so the call stops before it asks NGC for
  # anything. The check only reads which path the script took.
  local stub="${SANDBOX}/afxfallback-bin"
  mkdir -p "${stub}"
  printf '#!/usr/bin/env bash\nexit 7\n' > "${stub}/curl"
  chmod +x "${stub}/curl"

  local out
  out="$(PATH="${stub}:${PATH}" NGC_API_KEY="not-a-real-key" \
    bash "${MAXINE_SH}" --base "${base}" --cache-dir "${SANDBOX}/afxcache" \
    --sm 86 --install-afx-features --dry-run 2>&1)"

  if [[ "${out}" == *"No such file or directory"* ]]; then
    t_fail "--install-afx-features died on the missing SDK script: ${out}"
  else
    t_pass "--install-afx-features does not die on the missing SDK script"
  fi

  if [[ "${out}" != *"NGC REST API"* ]]; then
    t_fail "--install-afx-features did not take the REST fallback: ${out}"
  else
    t_pass "--install-afx-features takes the REST fallback"
  fi
}

test_pinning_a_root_with_special_characters
test_pinning_a_root_that_looks_like_a_pattern
test_a_dry_run_makes_no_directory
test_install_afx_features_falls_back_to_the_rest_api

if [[ "${FAILURES}" -ne 0 ]]; then
  echo "${FAILURES} check(s) failed." >&2
  exit 1
fi

echo "All checks passed."
