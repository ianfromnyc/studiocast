#!/usr/bin/env bash
# Shared source-archive helper for the StudioCast packaging scripts.
#
# Source this file, then call:
#   studiocast_create_source_archive REPO_ROOT VERSION OUTPUT_PATH
#
# The helper makes StudioCast-<version>-source.tar.gz from the committed tree
# at HEAD, with the top-level prefix StudioCast-<version>/. When git cannot be
# used, it falls back to a tarball of the working tree with the same prefix.
# The AppImage flow and the RPM flow both use this helper, so both make an
# identical archive.
#
# The caller usually supplies DRY_RUN and the log/die/run/print_cmd helpers.
# This file defines safe defaults for the ones the caller does not have, so it
# also works when sourced from a minimal script.

: "${DRY_RUN:=0}"

if ! declare -F print_cmd >/dev/null 2>&1; then
  print_cmd() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
  }
fi

if ! declare -F log >/dev/null 2>&1; then
  log() {
    printf '[source-archive] %s\n' "$*" >&2
  }
fi

if ! declare -F die >/dev/null 2>&1; then
  die() {
    printf '[source-archive] ERROR: %s\n' "$*" >&2
    exit 2
  }
fi

if ! declare -F run >/dev/null 2>&1; then
  run() {
    print_cmd "$@"
    if [[ "${DRY_RUN}" -eq 1 ]]; then
      return 0
    fi
    "$@"
  }
fi

studiocast_create_source_archive() {
  local repo_root="$1"
  local version="$2"
  local archive_path="$3"

  log "Creating source archive ${archive_path}"
  run rm -f -- "${archive_path}"

  if command -v git >/dev/null 2>&1 &&
      git -C "${repo_root}" rev-parse --is-inside-work-tree >/dev/null 2>&1 &&
      git -C "${repo_root}" rev-parse --verify 'HEAD^{commit}' >/dev/null 2>&1; then
    run git -C "${repo_root}" archive \
      --format=tar.gz \
      --prefix="StudioCast-${version}/" \
      --output="${archive_path}" \
      HEAD
  else
    log "git archive is unavailable; falling back to a working-tree tarball"
    local parent_dir repo_dir
    parent_dir="$(dirname "${repo_root}")"
    repo_dir="$(basename "${repo_root}")"
    run tar -C "${parent_dir}" \
      --exclude="${repo_dir}/.git" \
      --exclude="${repo_dir}/build" \
      --exclude="${repo_dir}/dist" \
      --exclude="${repo_dir}/cmake-build-*" \
      -czf "${archive_path}" \
      --transform "s#^${repo_dir}#StudioCast-${version}#" \
      "${repo_dir}"
  fi

  if [[ "${DRY_RUN}" -eq 0 && ! -f "${archive_path}" ]]; then
    die "source archive was not created: ${archive_path}"
  fi
}
