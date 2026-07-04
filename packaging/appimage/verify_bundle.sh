#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VERSION="$(tr -d '[:space:]' < "${REPO_ROOT}/VERSION")"
ARCH="$(uname -m)"
DIST_DIR="${REPO_ROOT}/dist/appimage"
APPDIR=""
REQUIRE_APPIMAGE=0

usage() {
  cat <<EOF
Verify StudioCast installer packaging artifacts.

Usage:
  packaging/appimage/verify_bundle.sh [options]

Options:
  --dist-dir DIR          Artifact output directory.
                          Default: ${DIST_DIR}
  --appdir DIR            Staged AppDir path. Defaults to the AppDir expected
                          under --dist-dir for VERSION and uname -m.
  --require-appimage      Require and verify the AppImage artifact.
  --help                  Show this help.
EOF
}

die() {
  printf '[verify-appimage] ERROR: %s\n' "$*" >&2
  exit 2
}

log() {
  printf '[verify-appimage] %s\n' "$*" >&2
}

require_file() {
  local path="$1"
  [[ -f "${path}" ]] || die "missing file: ${path}"
}

require_executable() {
  local path="$1"
  [[ -x "${path}" ]] || die "missing executable: ${path}"
}

checksum_contains() {
  local checksum_file="$1"
  local artifact="$2"
  grep -F "  $(basename "${artifact}")" "${checksum_file}" >/dev/null ||
    die "checksum file does not reference $(basename "${artifact}")"
}

tarball_contains() {
  local tarball="$1"
  local entry="$2"
  tar -tzf "${tarball}" "${entry}" >/dev/null ||
    die "AppDir tarball does not contain ${entry}"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --dist-dir)
        [[ $# -ge 2 ]] || die "--dist-dir requires a directory"
        DIST_DIR="$2"
        shift 2
        ;;
      --appdir)
        [[ $# -ge 2 ]] || die "--appdir requires a directory"
        APPDIR="$2"
        shift 2
        ;;
      --require-appimage)
        REQUIRE_APPIMAGE=1
        shift
        ;;
      --help|-h)
        usage
        exit 0
        ;;
      *)
        die "Unknown option: $1"
        ;;
    esac
  done
}

main() {
  parse_args "$@"

  local bundle_basename="StudioCast-Installer-${VERSION}-${ARCH}"
  local appimage_path="${DIST_DIR}/${bundle_basename}.AppImage"
  local appdir_tarball="${DIST_DIR}/${bundle_basename}.AppDir.tar.gz"
  local source_archive_name="StudioCast-${VERSION}-source.tar.gz"
  local source_archive_path="${DIST_DIR}/${source_archive_name}"
  local checksum_file="${DIST_DIR}/${bundle_basename}.sha256"

  if [[ -z "${APPDIR}" ]]; then
    APPDIR="${DIST_DIR}/${bundle_basename}.AppDir"
  fi

  local appdir_base
  appdir_base="$(basename "${APPDIR}")"

  require_file "${appdir_tarball}"
  require_file "${source_archive_path}"
  require_file "${checksum_file}"
  require_executable "${APPDIR}/usr/bin/studiocast-installer"
  require_executable "${APPDIR}/usr/share/studiocast/installer/studiocast-installer-backend"
  require_file "${APPDIR}/usr/share/applications/studiocast-installer.desktop"
  require_file "${APPDIR}/studiocast-installer.desktop"
  require_file "${APPDIR}/usr/share/studiocast/source/${source_archive_name}"

  if [[ "${REQUIRE_APPIMAGE}" -eq 1 ]]; then
    require_executable "${appimage_path}"
    checksum_contains "${checksum_file}" "${appimage_path}"
  elif [[ -f "${appimage_path}" ]]; then
    checksum_contains "${checksum_file}" "${appimage_path}"
  fi

  checksum_contains "${checksum_file}" "${appdir_tarball}"
  checksum_contains "${checksum_file}" "${source_archive_path}"
  (cd "${DIST_DIR}" && sha256sum --check "$(basename "${checksum_file}")")

  tarball_contains "${appdir_tarball}" "${appdir_base}/usr/bin/studiocast-installer"
  tarball_contains "${appdir_tarball}" \
    "${appdir_base}/usr/share/studiocast/installer/studiocast-installer-backend"
  tarball_contains "${appdir_tarball}" \
    "${appdir_base}/usr/share/applications/studiocast-installer.desktop"
  tarball_contains "${appdir_tarball}" \
    "${appdir_base}/usr/share/studiocast/source/${source_archive_name}"

  if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate \
      "${APPDIR}/usr/share/applications/studiocast-installer.desktop"
  else
    log "desktop-file-validate not found; skipping desktop metadata validation."
  fi

  log "Packaging artifact checks passed for ${bundle_basename}."
}

main "$@"
