#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VERSION="$(tr -d '[:space:]' < "${REPO_ROOT}/VERSION")"
ARCH="$(uname -m)"
FEDORA_RELEASE=44
DIST_DIR="${REPO_ROOT}/dist/rpm"
BUILD_DIR="${REPO_ROOT}/build/rpm"
TOPDIR="${BUILD_DIR}/rpmbuild"
SPEC_TEMPLATE="${SCRIPT_DIR}/studiocast.spec.in"
CONTAINER_BUILD_SOURCE="${SCRIPT_DIR}/container_build.sh"
SPEC_PATH="${TOPDIR}/SPECS/studiocast.spec"
SOURCE_ARCHIVE_NAME="StudioCast-${VERSION}-source.tar.gz"
SOURCE_ARCHIVE_PATH="${TOPDIR}/SOURCES/${SOURCE_ARCHIVE_NAME}"
CHECKSUM_FILE="${DIST_DIR}/studiocast-${VERSION}-rpm.sha256"
CONTAINER_SCRIPT="${BUILD_DIR}/container-build.sh"
DOWNLOAD_DIR="${BUILD_DIR}/downloads"
DLIB_LOCK="${SCRIPT_DIR}/dlib.lock"
IMAGE="${STUDIOCAST_RPM_IMAGE:-}"
IMAGE_EXPLICIT=0
DRY_RUN=0
CLEAN=0
KEEP_DOWNLOADS=0
USE_CONTAINER=0
SRPM_ONLY=0
RUN_RPMLINT=0
INSTALL_BUILDDEPS=0
ALLOW_CLEAN_OUTSIDE_REPO=0
# The spec turns dlib on by default. Track the effective setting here, because
# the dlib source tarball is only needed, and only fetched, when dlib is on.
WITH_DLIB=1
RPMBUILD_ARGS=()

usage() {
  cat <<EOF
Build Fedora RPM packages for the StudioCast application.

Usage:
  packaging/rpm/build_rpm.sh [options]

Options:
  --help                    Show this help.
  --dry-run                 Print commands without executing them.
  --clean                   Remove the rpmbuild tree and previous artifacts first.
  --keep-downloads          With --clean, keep the cached downloads directory,
                            so the dlib tarball is not fetched again.
  --clean-outside-repo      Let --clean remove a build or dist directory that
                            is outside the repository. Without it, --clean
                            refuses such a path, so a typo in --build-dir or
                            --dist-dir cannot delete something else. A
                            top-level directory and a system directory such as
                            /usr stay refused with the flag as well.
  --build-dir DIR           Working directory for the private rpmbuild tree.
                            Default: ${BUILD_DIR}
  --dist-dir DIR            Artifact output directory.
                            Default: ${DIST_DIR}
  --fedora-release N        Fedora release for the default container image tag
                            and the expected %{dist} tag. Default: ${FEDORA_RELEASE}
  --container[=IMAGE]       Run rpmbuild inside a podman (or docker) container.
                            Use this on a host that is not Fedora ${FEDORA_RELEASE}.
  --image IMAGE             Container image to use. Implies --container, and
                            overrides the default and \$STUDIOCAST_RPM_IMAGE.
  --install-builddeps       Install the spec BuildRequires with dnf builddep
                            before the build. Needs root, or sudo. Container
                            mode installs them by itself, so this option is
                            unnecessary there.
  --srpm-only               Build the source RPM only.
  --with NAME               Enable a spec build conditional. May be repeated.
  --without NAME            Disable a spec build conditional. May be repeated.
  --rpmlint                 Run rpmlint on the results and print the report.
                            The rpmlint exit status never fails this script.

Build conditionals (see packaging/rpm/studiocast.spec.in):
  open_cuda (on), open_audio (on), dlib (on), libyuv (on), pipewire (on),
  tests (on), installer (off)

Artifacts in ${DIST_DIR}:
  studiocast-${VERSION}-1.fc${FEDORA_RELEASE}.src.rpm
  studiocast-${VERSION}-1.fc${FEDORA_RELEASE}.${ARCH}.rpm
  studiocast-debuginfo-... and studiocast-debugsource-...
  $(basename "${CHECKSUM_FILE}")

Notes:
  The script renders packaging/rpm/studiocast.spec.in into the private rpmbuild
  tree and makes ${SOURCE_ARCHIVE_NAME} with the shared helper
  packaging/_lib/source_archive.sh, so the RPM and the AppImage ship the same
  source archive. \$HOME/rpmbuild is never touched.

  Fedora has no dlib package, so a dlib build compiles dlib from the source
  release pinned in packaging/rpm/dlib.lock. This script downloads that tarball
  and checks its SHA256 before rpmbuild starts. The file is cached in
  ${DOWNLOAD_DIR}
  and copied into the rpmbuild SOURCES directory. --without dlib skips the
  download.

  In container mode the host prepares the rpmbuild tree, which includes the
  dlib download, then the container mounts only that tree. The container
  installs rpm-build and the build dependencies from the rendered spec before
  it builds. It never reaches the network for the sources.
EOF
}

log() {
  printf '[rpm] %s\n' "$*" >&2
}

die() {
  printf '[rpm] ERROR: %s\n' "$*" >&2
  exit 2
}

print_cmd() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
}

run() {
  print_cmd "$@"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    return 0
  fi
  "$@"
}

# The source archive must match the one the AppImage flow ships.
# shellcheck source-path=SCRIPTDIR
# shellcheck source=../_lib/source_archive.sh
source "${REPO_ROOT}/packaging/_lib/source_archive.sh"

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --help|-h)
        usage
        exit 0
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      --clean)
        CLEAN=1
        shift
        ;;
      --keep-downloads)
        KEEP_DOWNLOADS=1
        shift
        ;;
      --clean-outside-repo)
        ALLOW_CLEAN_OUTSIDE_REPO=1
        shift
        ;;
      --build-dir)
        [[ $# -ge 2 ]] || die "--build-dir requires a directory"
        BUILD_DIR="$2"
        shift 2
        ;;
      --dist-dir)
        [[ $# -ge 2 ]] || die "--dist-dir requires a directory"
        DIST_DIR="$2"
        shift 2
        ;;
      --fedora-release)
        [[ $# -ge 2 ]] || die "--fedora-release requires a number"
        [[ "$2" =~ ^[0-9]+$ ]] || die "--fedora-release must be a number: $2"
        FEDORA_RELEASE="$2"
        shift 2
        ;;
      --container)
        USE_CONTAINER=1
        shift
        ;;
      --container=*)
        USE_CONTAINER=1
        IMAGE="${1#--container=}"
        IMAGE_EXPLICIT=1
        [[ -n "${IMAGE}" ]] || die "--container=IMAGE requires an image name"
        shift
        ;;
      --image)
        [[ $# -ge 2 ]] || die "--image requires an image name"
        # An image is only used in container mode, and a caller who names one
        # means to use it, so imply --container as --container=IMAGE does.
        USE_CONTAINER=1
        IMAGE="$2"
        IMAGE_EXPLICIT=1
        shift 2
        ;;
      --install-builddeps)
        INSTALL_BUILDDEPS=1
        shift
        ;;
      --srpm-only)
        SRPM_ONLY=1
        shift
        ;;
      --with)
        [[ $# -ge 2 ]] || die "--with requires a conditional name"
        if [[ "$2" == "dlib" ]]; then
          WITH_DLIB=1
        fi
        RPMBUILD_ARGS+=(--with "$2")
        shift 2
        ;;
      --without)
        [[ $# -ge 2 ]] || die "--without requires a conditional name"
        if [[ "$2" == "dlib" ]]; then
          WITH_DLIB=0
        fi
        RPMBUILD_ARGS+=(--without "$2")
        shift 2
        ;;
      --rpmlint)
        RUN_RPMLINT=1
        shift
        ;;
      *)
        die "Unknown option: $1"
        ;;
    esac
  done
}

refresh_paths() {
  TOPDIR="${BUILD_DIR}/rpmbuild"
  SPEC_PATH="${TOPDIR}/SPECS/studiocast.spec"
  SOURCE_ARCHIVE_NAME="StudioCast-${VERSION}-source.tar.gz"
  SOURCE_ARCHIVE_PATH="${TOPDIR}/SOURCES/${SOURCE_ARCHIVE_NAME}"
  CHECKSUM_FILE="${DIST_DIR}/studiocast-${VERSION}-rpm.sha256"
  CONTAINER_SCRIPT="${BUILD_DIR}/container-build.sh"
  DOWNLOAD_DIR="${BUILD_DIR}/downloads"
  if [[ "${IMAGE_EXPLICIT}" -eq 0 && -z "${IMAGE}" ]]; then
    IMAGE="registry.fedoraproject.org/fedora:${FEDORA_RELEASE}"
  fi
}

# Reads the pinned dlib release. The version, the URL and the SHA256 live in
# packaging/rpm/dlib.lock only, so nothing else repeats them.
load_dlib_lock() {
  [[ -f "${DLIB_LOCK}" ]] || die "dlib lock file is missing: ${DLIB_LOCK}"
  # shellcheck source-path=SCRIPTDIR
  # shellcheck source=dlib.lock
  source "${DLIB_LOCK}"
  [[ -n "${DLIB_VERSION:-}" ]] || die "${DLIB_LOCK} does not set DLIB_VERSION"
  [[ -n "${DLIB_URL:-}" ]] || die "${DLIB_LOCK} does not set DLIB_URL"
  [[ -n "${DLIB_SHA256:-}" ]] || die "${DLIB_LOCK} does not set DLIB_SHA256"
  DLIB_ARCHIVE_NAME="dlib-${DLIB_VERSION}.tar.gz"
}

file_sha256() {
  sha256sum -- "$1" | awk '{ print $1 }'
}

# Puts the pinned dlib tarball in the rpmbuild SOURCES directory. It downloads
# into a cache first, so a second run and a --clean --keep-downloads run reuse
# the file. Every path checks the SHA256 before the file is used.
fetch_dlib_source() {
  local cached="${DOWNLOAD_DIR}/${DLIB_ARCHIVE_NAME}"
  local target="${TOPDIR}/SOURCES/${DLIB_ARCHIVE_NAME}"

  log "Providing dlib ${DLIB_VERSION} source for the build"
  run install -d -m 0755 "${DOWNLOAD_DIR}"

  if [[ "${DRY_RUN}" -eq 1 ]]; then
    print_cmd curl --fail --location --retry 3 --output "${cached}" "${DLIB_URL}"
    print_cmd sha256sum --check "-" "<" "${DLIB_SHA256}  ${cached}"
    print_cmd install -m 0644 "${cached}" "${target}"
    return 0
  fi

  if [[ -f "${cached}" ]] && [[ "$(file_sha256 "${cached}")" == "${DLIB_SHA256}" ]]; then
    log "Reusing the cached download ${cached}"
  else
    if [[ -f "${cached}" ]]; then
      log "The cached download does not match the pin; fetching it again"
      run rm -f -- "${cached}"
    fi
    log "Downloading ${DLIB_URL}"
    curl --fail --location --retry 3 --output "${cached}" "${DLIB_URL}" ||
      die "could not download the dlib source from ${DLIB_URL}"
  fi

  local actual
  actual="$(file_sha256 "${cached}")"
  if [[ "${actual}" != "${DLIB_SHA256}" ]]; then
    die "dlib source checksum mismatch for ${DLIB_ARCHIVE_NAME}: expected ${DLIB_SHA256}, got ${actual}. Fix packaging/rpm/dlib.lock, or remove the stale file."
  fi
  log "dlib source SHA256 matches the pin"

  install -m 0644 "${cached}" "${target}"
}

find_container_runtime() {
  local candidate
  for candidate in podman docker; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  return 1
}

prepare_topdir() {
  log "Preparing rpmbuild tree at ${TOPDIR}"
  run install -d -m 0755 \
    "${TOPDIR}/BUILD" \
    "${TOPDIR}/BUILDROOT" \
    "${TOPDIR}/RPMS" \
    "${TOPDIR}/SOURCES" \
    "${TOPDIR}/SPECS" \
    "${TOPDIR}/SRPMS"

  studiocast_create_source_archive "${REPO_ROOT}" "${VERSION}" \
    "${SOURCE_ARCHIVE_PATH}"

  if [[ "${WITH_DLIB}" -eq 1 ]]; then
    fetch_dlib_source
  else
    log "dlib is off; skipping the dlib source download"
  fi

  log "Rendering ${SPEC_PATH}"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    print_cmd sed -e "s/@VERSION@/${VERSION}/g" \
      -e "s/@DLIB_VERSION@/${DLIB_VERSION}/g" "${SPEC_TEMPLATE}" ">" "${SPEC_PATH}"
  else
    [[ -f "${SPEC_TEMPLATE}" ]] || die "spec template is missing: ${SPEC_TEMPLATE}"
    sed -e "s/@VERSION@/${VERSION}/g" -e "s/@DLIB_VERSION@/${DLIB_VERSION}/g" \
      "${SPEC_TEMPLATE}" > "${SPEC_PATH}"
    chmod 0644 "${SPEC_PATH}"
    # @BINDIR@ is a placeholder for the systemd unit template, not for the
    # spec, so it must stay. Only check the ones this script renders.
    if grep -qE '@VERSION@|@DLIB_VERSION@' "${SPEC_PATH}"; then
      die "spec still holds an unsubstituted placeholder: ${SPEC_PATH}"
    fi
  fi
}

# Installs the BuildRequires of the rendered spec. The package list stays in
# the spec only, so no other file repeats it.
install_builddeps() {
  command -v dnf >/dev/null 2>&1 ||
    die "dnf was not found; --install-builddeps needs a Fedora host."

  local -a privileged=()
  if [[ "$(id -u)" -ne 0 ]]; then
    command -v sudo >/dev/null 2>&1 ||
      die "--install-builddeps changes the system. Run it as root, or install sudo."
    privileged=(sudo)
  fi

  # dnf builddep comes from dnf5-plugins on Fedora 44 and from
  # dnf-plugins-core on older releases.
  if [[ "${DRY_RUN}" -eq 0 ]] && ! dnf builddep --help >/dev/null 2>&1; then
    die "dnf builddep was not found. Install dnf5-plugins, or dnf-plugins-core on an older Fedora."
  fi

  if [[ ${#RPMBUILD_ARGS[@]} -gt 0 ]]; then
    log "Note: dnf builddep reads the spec defaults; --with and --without are not passed on."
  fi

  log "Installing the build dependencies from ${SPEC_PATH}"
  run "${privileged[@]}" dnf builddep -y "${SPEC_PATH}"
}

build_native() {
  command -v rpmbuild >/dev/null 2>&1 ||
    die "rpmbuild was not found. Install rpm-build, or re-run with --container."

  log "Building the source RPM"
  run rpmbuild --define "_topdir ${TOPDIR}" "${RPMBUILD_ARGS[@]}" \
    -bs "${SPEC_PATH}"

  if [[ "${SRPM_ONLY}" -eq 1 ]]; then
    log "Stopping after the source RPM by request"
    return 0
  fi

  log "Building the binary RPMs"
  if ! run rpmbuild --define "_topdir ${TOPDIR}" "${RPMBUILD_ARGS[@]}" \
    -bb "${SPEC_PATH}"; then
    die "rpmbuild failed. Missing build dependencies are the usual cause; re-run with --container to build in a clean Fedora ${FEDORA_RELEASE} image."
  fi

  if [[ "${RUN_RPMLINT}" -eq 1 ]]; then
    if command -v rpmlint >/dev/null 2>&1; then
      log "Running rpmlint"
      run_rpmlint_over "${TOPDIR}"
    else
      log "rpmlint was not found on this host; skipping the report."
    fi
  fi
}

run_rpmlint_over() {
  local topdir="$1"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    print_cmd rpmlint "${topdir}/SRPMS" "${topdir}/RPMS"
    return 0
  fi
  # rpmlint reports style warnings that are not build failures, so its exit
  # status is deliberately ignored. The report itself is the deliverable.
  rpmlint "${topdir}/SRPMS" "${topdir}/RPMS" || true
}

# Puts the checked-in container build script in the build directory, which is
# the only directory the container mounts. The script is checked in rather than
# generated, so shellcheck covers it and no value is interpolated into it.
write_container_script() {
  log "Installing ${CONTAINER_SCRIPT}"
  run install -d -m 0755 "$(dirname "${CONTAINER_SCRIPT}")"
  run install -m 0755 "${CONTAINER_BUILD_SOURCE}" "${CONTAINER_SCRIPT}"
}

build_in_container() {
  local runtime
  runtime="$(find_container_runtime)" ||
    die "neither podman nor docker was found; cannot use --container"

  write_container_script

  local -a runtime_args=(
    run --rm
    --volume "${BUILD_DIR}:/work:Z"
    --workdir /work
  )
  if [[ "${runtime}" == "docker" ]]; then
    # Docker keeps container root, so the artifacts need the host owner back.
    runtime_args+=(
      --env STUDIOCAST_FIX_OWNERSHIP=1
      --env "STUDIOCAST_HOST_UID=$(id -u)"
      --env "STUDIOCAST_HOST_GID=$(id -g)"
    )
  fi
  runtime_args+=(
    "${IMAGE}" bash /work/container-build.sh "${SRPM_ONLY}" "${RUN_RPMLINT}"
  )
  if [[ ${#RPMBUILD_ARGS[@]} -gt 0 ]]; then
    runtime_args+=("${RPMBUILD_ARGS[@]}")
  fi

  log "Building in ${runtime} image ${IMAGE}"
  run "${runtime}" "${runtime_args[@]}"
}

collect_artifacts() {
  log "Collecting artifacts into ${DIST_DIR}"
  run install -d -m 0755 "${DIST_DIR}"

  if [[ "${DRY_RUN}" -eq 1 ]]; then
    print_cmd cp -f "${TOPDIR}/SRPMS"/*.src.rpm "${TOPDIR}/RPMS"/*/*.rpm "${DIST_DIR}/"
    print_cmd sha256sum "${DIST_DIR}"/*.rpm ">" "${CHECKSUM_FILE}"
    return 0
  fi

  # Only this version. A tree that kept an older build would otherwise put
  # those packages in dist/ and in the checksum file, and the verify script
  # then fails on a package that this run never made. The pattern also covers
  # studiocast-debuginfo and studiocast-debugsource.
  local -a artifacts=()
  local path
  while IFS= read -r path; do
    artifacts+=("${path}")
  done < <(find "${TOPDIR}/SRPMS" "${TOPDIR}/RPMS" -type f \
    -name "studiocast-*${VERSION}-*.rpm" -print | sort)

  [[ ${#artifacts[@]} -gt 0 ]] ||
    die "rpmbuild produced no packages under ${TOPDIR}"

  for path in "${artifacts[@]}"; do
    install -m 0644 "${path}" "${DIST_DIR}/$(basename "${path}")"
  done

  : > "${CHECKSUM_FILE}"
  for path in "${artifacts[@]}"; do
    (cd "${DIST_DIR}" && sha256sum "$(basename "${path}")") >> "${CHECKSUM_FILE}"
  done
}

report_artifacts() {
  log "Artifacts:"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    log "  ${DIST_DIR}/studiocast-${VERSION}-1.fc${FEDORA_RELEASE}.src.rpm"
    log "  ${DIST_DIR}/studiocast-${VERSION}-1.fc${FEDORA_RELEASE}.${ARCH}.rpm"
    log "  ${CHECKSUM_FILE}"
    return 0
  fi

  local path
  while IFS= read -r path; do
    log "  ${path}"
    case "$(basename "${path}")" in
      *".fc${FEDORA_RELEASE}."*) ;;
      *)
        log "  WARNING: unexpected dist tag; expected .fc${FEDORA_RELEASE}"
        ;;
    esac
  done < <(find "${DIST_DIR}" -maxdepth 1 -type f \
    -name "studiocast-*${VERSION}-*.rpm" -print | sort)
  log "  ${CHECKSUM_FILE}"
}

# System directories that never hold a build tree. --clean-outside-repo says
# the caller knows the path is outside the repository; it does not say the
# path is safe to remove, so these stay refused whatever the flag says.
CLEAN_PROTECTED_PREFIXES=(/usr /etc /boot /bin /sbin /lib /lib64 /opt /srv
                          /proc /sys /dev /run /var)

# The one place inside that list that holds throwaway trees. rpmbuild itself
# works there, so a build directory under it is a normal choice.
CLEAN_ALLOWED_PREFIXES=(/var/tmp)

# --clean removes the build directory and the artifact directory, so check
# first that the path is one this script made. A path outside the repository
# needs --clean-outside-repo, which keeps a typo in --build-dir or --dist-dir
# from deleting the home directory or the root file system.
require_safe_clean_target() {
  local label="$1"
  local path="$2"
  local resolved

  [[ -n "${path}" ]] || die "${label} is empty; --clean needs a directory"
  resolved="$(readlink -m -- "${path}")" ||
    die "could not resolve the ${label} path: ${path}"

  [[ "${resolved}" != "/" ]] ||
    die "--clean refuses to remove the root directory (${label})"
  [[ -z "${HOME:-}" || "${resolved}" != "${HOME}" ]] ||
    die "--clean refuses to remove the home directory (${label})"

  # A path with one component is a top-level directory such as /usr or /home,
  # and no build tree lives there.
  local parts
  IFS='/' read -r -a parts <<<"${resolved#/}"
  [[ "${#parts[@]}" -ge 2 ]] ||
    die "--clean refuses the top-level ${label} path ${resolved}"

  local prefix allowed=0
  for prefix in "${CLEAN_ALLOWED_PREFIXES[@]}"; do
    if [[ "${resolved}" == "${prefix}/"* ]]; then
      allowed=1
      break
    fi
  done
  if [[ "${allowed}" -eq 0 ]]; then
    for prefix in "${CLEAN_PROTECTED_PREFIXES[@]}"; do
      [[ "${resolved}" != "${prefix}/"* ]] ||
        die "--clean refuses the ${label} path ${resolved}, which is inside the system directory ${prefix}"
    done
  fi

  if [[ "${ALLOW_CLEAN_OUTSIDE_REPO}" -eq 0 && "${resolved}" != "${REPO_ROOT}/"* ]]; then
    die "--clean refuses the ${label} path ${resolved}, which is outside ${REPO_ROOT}. Add --clean-outside-repo to allow it."
  fi
}

clean_build_dir() {
  if [[ "${KEEP_DOWNLOADS}" -eq 1 ]]; then
    log "Cleaning the rpmbuild tree, keeping ${DOWNLOAD_DIR}"
    if [[ "${DRY_RUN}" -eq 1 ]]; then
      print_cmd find "${BUILD_DIR}" -mindepth 1 -maxdepth 1 \
        '!' -name downloads -exec rm -rf '{}' '+'
      return 0
    fi
    if [[ -d "${BUILD_DIR}" ]]; then
      find "${BUILD_DIR}" -mindepth 1 -maxdepth 1 \
        '!' -name downloads -exec rm -rf '{}' '+'
    fi
    return 0
  fi

  log "Cleaning the rpmbuild tree and the cached downloads"
  run rm -rf -- "${BUILD_DIR}"
}

main() {
  parse_args "$@"
  refresh_paths
  load_dlib_lock

  if [[ "${CLEAN}" -eq 1 ]]; then
    require_safe_clean_target "--build-dir" "${BUILD_DIR}"
    require_safe_clean_target "--dist-dir" "${DIST_DIR}"
    clean_build_dir
    log "Removing the previous artifacts"
    run rm -rf -- "${DIST_DIR}"
  fi

  prepare_topdir

  if [[ "${USE_CONTAINER}" -eq 1 ]]; then
    if [[ "${INSTALL_BUILDDEPS}" -eq 1 ]]; then
      log "Container mode installs the build dependencies itself; ignoring --install-builddeps"
    fi
    build_in_container
  else
    if [[ "${INSTALL_BUILDDEPS}" -eq 1 ]]; then
      install_builddeps
    fi
    build_native
  fi

  collect_artifacts
  report_artifacts
}

main "$@"
