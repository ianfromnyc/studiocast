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
SPEC_PATH="${TOPDIR}/SPECS/studiocast.spec"
SOURCE_ARCHIVE_NAME="StudioCast-${VERSION}-source.tar.gz"
SOURCE_ARCHIVE_PATH="${TOPDIR}/SOURCES/${SOURCE_ARCHIVE_NAME}"
CHECKSUM_FILE="${DIST_DIR}/studiocast-${VERSION}-rpm.sha256"
CONTAINER_SCRIPT="${BUILD_DIR}/container-build.sh"
IMAGE="${STUDIOCAST_RPM_IMAGE:-}"
IMAGE_EXPLICIT=0
DRY_RUN=0
CLEAN=0
USE_CONTAINER=0
SRPM_ONLY=0
RUN_RPMLINT=0
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
  --build-dir DIR           Working directory for the private rpmbuild tree.
                            Default: ${BUILD_DIR}
  --dist-dir DIR            Artifact output directory.
                            Default: ${DIST_DIR}
  --fedora-release N        Fedora release for the default container image tag
                            and the expected %{dist} tag. Default: ${FEDORA_RELEASE}
  --container[=IMAGE]       Run rpmbuild inside a podman (or docker) container.
                            Use this on a host that is not Fedora ${FEDORA_RELEASE}.
  --image IMAGE             Container image to use. Overrides the default and
                            \$STUDIOCAST_RPM_IMAGE.
  --srpm-only               Build the source RPM only.
  --with NAME               Enable a spec build conditional. May be repeated.
  --without NAME            Disable a spec build conditional. May be repeated.
  --rpmlint                 Run rpmlint on the results and print the report.
                            The rpmlint exit status never fails this script.

Build conditionals (see packaging/rpm/studiocast.spec.in):
  open_cuda (on), open_audio (on), dlib (off), libyuv (on), tests (on),
  installer (off)

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

  In container mode the host prepares the rpmbuild tree, then the container
  mounts only that tree. The container installs rpm-build and the build
  dependencies from the rendered spec before it builds.
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
        IMAGE="$2"
        IMAGE_EXPLICIT=1
        shift 2
        ;;
      --srpm-only)
        SRPM_ONLY=1
        shift
        ;;
      --with)
        [[ $# -ge 2 ]] || die "--with requires a conditional name"
        RPMBUILD_ARGS+=(--with "$2")
        shift 2
        ;;
      --without)
        [[ $# -ge 2 ]] || die "--without requires a conditional name"
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
  if [[ "${IMAGE_EXPLICIT}" -eq 0 && -z "${IMAGE}" ]]; then
    IMAGE="registry.fedoraproject.org/fedora:${FEDORA_RELEASE}"
  fi
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

  log "Rendering ${SPEC_PATH}"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    print_cmd sed "s/@VERSION@/${VERSION}/g" "${SPEC_TEMPLATE}" ">" "${SPEC_PATH}"
  else
    [[ -f "${SPEC_TEMPLATE}" ]] || die "spec template is missing: ${SPEC_TEMPLATE}"
    sed "s/@VERSION@/${VERSION}/g" "${SPEC_TEMPLATE}" > "${SPEC_PATH}"
    chmod 0644 "${SPEC_PATH}"
    if grep -q '@VERSION@' "${SPEC_PATH}"; then
      die "spec still holds an unsubstituted placeholder: ${SPEC_PATH}"
    fi
  fi
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

write_container_script() {
  local rpmbuild_args=""
  if [[ ${#RPMBUILD_ARGS[@]} -gt 0 ]]; then
    rpmbuild_args="$(printf ' %q' "${RPMBUILD_ARGS[@]}")"
  fi

  log "Writing ${CONTAINER_SCRIPT}"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    print_cmd install -m 0755 /dev/stdin "${CONTAINER_SCRIPT}"
    return 0
  fi

  install -d -m 0755 "$(dirname "${CONTAINER_SCRIPT}")"
  cat > "${CONTAINER_SCRIPT}" <<EOF
#!/usr/bin/env bash
# Generated by packaging/rpm/build_rpm.sh. Do not edit.
set -euo pipefail

TOPDIR="/work/rpmbuild"
SPEC="\${TOPDIR}/SPECS/studiocast.spec"
SRPM_ONLY=${SRPM_ONLY}
RUN_RPMLINT=${RUN_RPMLINT}
RPMBUILD_ARGS=(${rpmbuild_args})
FIX_OWNERSHIP="\${STUDIOCAST_FIX_OWNERSHIP:-0}"

echo "[rpm-container] Installing rpmbuild and the dependency resolver"
dnf install -y --setopt=install_weak_deps=False rpm-build rpmdevtools
dnf install -y --setopt=install_weak_deps=False dnf5-plugins ||
  dnf install -y --setopt=install_weak_deps=False dnf-plugins-core
if [[ "\${RUN_RPMLINT}" -eq 1 ]]; then
  dnf install -y --setopt=install_weak_deps=False rpmlint
fi

echo "[rpm-container] Building the source RPM"
rpmbuild --define "_topdir \${TOPDIR}" "\${RPMBUILD_ARGS[@]}" \\
  -bs "\${SPEC}"

if [[ "\${SRPM_ONLY}" -ne 1 ]]; then
  echo "[rpm-container] Installing the build dependencies"
  dnf builddep -y "\${SPEC}"

  echo "[rpm-container] Building the binary RPMs"
  rpmbuild --define "_topdir \${TOPDIR}" "\${RPMBUILD_ARGS[@]}" \\
    -bb "\${SPEC}"
fi

if [[ "\${RUN_RPMLINT}" -eq 1 ]]; then
  echo "[rpm-container] rpmlint report"
  rpmlint "\${TOPDIR}/SRPMS" "\${TOPDIR}/RPMS" || true
fi

if [[ "\${FIX_OWNERSHIP}" -eq 1 ]]; then
  chown -R "\${STUDIOCAST_HOST_UID}:\${STUDIOCAST_HOST_GID}" /work
fi
EOF
  chmod 0755 "${CONTAINER_SCRIPT}"
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
  runtime_args+=("${IMAGE}" bash /work/container-build.sh)

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

  local -a artifacts=()
  local path
  while IFS= read -r path; do
    artifacts+=("${path}")
  done < <(find "${TOPDIR}/SRPMS" "${TOPDIR}/RPMS" -type f -name '*.rpm' -print |
    sort)

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
  done < <(find "${DIST_DIR}" -maxdepth 1 -type f -name '*.rpm' -print | sort)
  log "  ${CHECKSUM_FILE}"
}

main() {
  parse_args "$@"
  refresh_paths

  if [[ "${CLEAN}" -eq 1 ]]; then
    log "Cleaning the rpmbuild tree and previous artifacts"
    run rm -rf -- "${BUILD_DIR}"
    run rm -rf -- "${DIST_DIR}"
  fi

  prepare_topdir

  if [[ "${USE_CONTAINER}" -eq 1 ]]; then
    build_in_container
  else
    build_native
  fi

  collect_artifacts
  report_artifacts
}

main "$@"
