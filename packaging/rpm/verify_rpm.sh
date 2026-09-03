#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VERSION="$(tr -d '[:space:]' < "${REPO_ROOT}/VERSION")"
ARCH="$(uname -m)"
FEDORA_RELEASE=44
DIST_DIR="${REPO_ROOT}/dist/rpm"
IMAGE="${STUDIOCAST_RPM_IMAGE:-}"
IMAGE_EXPLICIT=0
INSTALL_TEST=0
USE_CONTAINER=0

usage() {
  cat <<EOF
Verify StudioCast RPM packaging artifacts.

Usage:
  packaging/rpm/verify_rpm.sh [options]

Options:
  --dist-dir DIR          Artifact output directory.
                          Default: ${DIST_DIR}
  --fedora-release N      Expected %{dist} tag and default container image tag.
                          Default: ${FEDORA_RELEASE}
  --install-test          Install the binary RPM, run the programs, then remove
                          it. This needs root, so run it inside a container or
                          add --container.
  --container[=IMAGE]     Run the install test inside a podman (or docker)
                          container. Implies --install-test.
  --image IMAGE           Container image for the install test. Overrides the
                          default and \$STUDIOCAST_RPM_IMAGE.
  --help                  Show this help.

Checks:
  The source RPM and the binary RPM exist for VERSION, their metadata matches,
  the file list holds the programs, the user unit, the desktop entry, the icon
  and the license files, the run-time dependencies are declared, and the
  checksum file matches every package.

  The install test also checks that the package installs with dnf while the
  weak dependency v4l2loopback is unavailable, that the programs run, that the
  desktop entry is valid, and that removal leaves no files behind.
EOF
}

die() {
  printf '[verify-rpm] ERROR: %s\n' "$*" >&2
  exit 2
}

log() {
  printf '[verify-rpm] %s\n' "$*" >&2
}

print_cmd() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
}

require_file() {
  local path="$1"
  [[ -f "${path}" ]] || die "missing file: ${path}"
}

require_equal() {
  local actual="$1"
  local expected="$2"
  local context="$3"
  [[ "${actual}" == "${expected}" ]] ||
    die "${context}: expected '${expected}', got '${actual}'"
}

checksum_contains() {
  local checksum_file="$1"
  local artifact="$2"
  grep -F "  $(basename "${artifact}")" "${checksum_file}" >/dev/null ||
    die "checksum file does not reference $(basename "${artifact}")"
}

rpm_query() {
  local package="$1"
  local format="$2"
  rpm -qp --qf "${format}" "${package}" 2>/dev/null ||
    die "rpm could not read ${package}"
}

package_lists_path() {
  local package="$1"
  local path="$2"
  rpm -qpl "${package}" 2>/dev/null | grep -Fx "${path}" >/dev/null ||
    die "$(basename "${package}") does not contain ${path}"
}

package_requires() {
  local package="$1"
  local requirement="$2"
  rpm -qp --requires "${package}" 2>/dev/null |
    awk '{ print $1 }' | grep -Fx "${requirement}" >/dev/null ||
    die "$(basename "${package}") does not require ${requirement}"
}

package_recommends() {
  local package="$1"
  local requirement="$2"
  rpm -qp --recommends "${package}" 2>/dev/null |
    awk '{ print $1 }' | grep -Fx "${requirement}" >/dev/null ||
    die "$(basename "${package}") does not recommend ${requirement}"
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

inside_container() {
  [[ -f /run/.containerenv || -f /.dockerenv || -n "${container:-}" ]]
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
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
      --install-test)
        INSTALL_TEST=1
        shift
        ;;
      --container)
        USE_CONTAINER=1
        INSTALL_TEST=1
        shift
        ;;
      --container=*)
        USE_CONTAINER=1
        INSTALL_TEST=1
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
      --help|-h)
        usage
        exit 0
        ;;
      *)
        die "Unknown option: $1"
        ;;
    esac
  done

  if [[ "${IMAGE_EXPLICIT}" -eq 0 && -z "${IMAGE}" ]]; then
    IMAGE="registry.fedoraproject.org/fedora:${FEDORA_RELEASE}"
  fi
}

verify_metadata() {
  local srpm="$1"
  local rpm_file="$2"
  local release="1.fc${FEDORA_RELEASE}"

  require_equal "$(rpm_query "${rpm_file}" '%{name}')" "studiocast" \
    "binary RPM name"
  require_equal "$(rpm_query "${rpm_file}" '%{version}')" "${VERSION}" \
    "binary RPM version"
  require_equal "$(rpm_query "${rpm_file}" '%{release}')" "${release}" \
    "binary RPM release"
  require_equal "$(rpm_query "${rpm_file}" '%{license}')" "MPL-2.0" \
    "binary RPM license"
  require_equal "$(rpm_query "${rpm_file}" '%{arch}')" "${ARCH}" \
    "binary RPM architecture"

  require_equal "$(rpm_query "${srpm}" '%{name}')" "studiocast" "source RPM name"
  require_equal "$(rpm_query "${srpm}" '%{version}')" "${VERSION}" \
    "source RPM version"
  require_equal "$(rpm_query "${srpm}" '%{release}')" "${release}" \
    "source RPM release"
  require_equal "$(rpm_query "${srpm}" '%{license}')" "MPL-2.0" \
    "source RPM license"
}

verify_file_list() {
  local rpm_file="$1"
  local path
  for path in \
    /usr/bin/studiocast \
    /usr/bin/studiocastd \
    /usr/bin/studiocastctl \
    /usr/bin/studiocast-probe \
    /usr/bin/studiocast-maxine \
    /usr/bin/studiocast-open \
    /usr/bin/studiocast-audio \
    /usr/bin/studiocast-video \
    /usr/lib/systemd/user/studiocastd.service \
    /usr/share/applications/studiocast.desktop \
    /usr/share/icons/hicolor/scalable/apps/studiocast.svg \
    /usr/share/licenses/studiocast/LICENSE \
    /usr/share/licenses/studiocast/NOTICE \
    /usr/share/doc/studiocast/README.md \
    /usr/share/doc/studiocast/CHANGELOG.md; do
    package_lists_path "${rpm_file}" "${path}"
  done

  # The GUI installer and its Ubuntu-only backend are not part of the package.
  if rpm -qpl "${rpm_file}" 2>/dev/null |
      grep -Fx /usr/bin/studiocast-installer >/dev/null; then
    die "$(basename "${rpm_file}") unexpectedly contains the GUI installer"
  fi
}

verify_dependencies() {
  local rpm_file="$1"
  package_requires "${rpm_file}" "pulseaudio-utils"
  package_requires "${rpm_file}" "v4l-utils"
  package_requires "${rpm_file}" "hicolor-icon-theme"
  package_recommends "${rpm_file}" "v4l2loopback"
}

verify_checksums() {
  local checksum_file="$1"
  local path
  while IFS= read -r path; do
    checksum_contains "${checksum_file}" "${path}"
  done < <(find "${DIST_DIR}" -maxdepth 1 -type f -name '*.rpm' -print | sort)
  (cd "${DIST_DIR}" && sha256sum --check "$(basename "${checksum_file}")")
}

# Emits the install test. It runs as root, either in a container started by
# this script or in a container the caller is already inside.
install_test_script() {
  cat <<'EOF'
set -euo pipefail

version="$1"
arch="$2"
dist="$3"

fail() {
  printf '[verify-rpm] ERROR: %s\n' "$*" >&2
  exit 2
}

cd "${dist}"
package="studiocast-${version}-1.fc"*".${arch}.rpm"
# shellcheck disable=SC2086
package="$(ls -1 ${package} 2>/dev/null | head -n 1 || true)"
[ -n "${package}" ] || fail "no binary RPM for ${version} in ${dist}"

echo "[verify-rpm] Installing ${package}"
dnf install -y --setopt=install_weak_deps=True "./${package}"
dnf install -y desktop-file-utils

rpm -q studiocast

# v4l2loopback is only in RPM Fusion. The install must still succeed with the
# weak dependency unresolved.
if rpm -q v4l2loopback >/dev/null 2>&1; then
  echo "[verify-rpm] v4l2loopback is present in this image"
else
  echo "[verify-rpm] v4l2loopback is absent; the weak dependency was skipped"
fi

studiocast --version | grep -Fq "studiocast ${version}" ||
  fail "studiocast --version did not report ${version}"
studiocastctl --version | grep -Fq "studiocastctl ${version}" ||
  fail "studiocastctl --version did not report ${version}"
studiocastd --help | grep -Fq 'studiocastd' ||
  fail "studiocastd --help did not print its usage"

# studiocast-probe --self-test needs the repository layout, so the spec runs it
# in %check instead. An installed probe reports the machine as JSON.
studiocast-probe --json | grep -Fq "\"app_version\":\"${version}\"" ||
  fail "studiocast-probe --json did not report ${version}"

desktop-file-validate /usr/share/applications/studiocast.desktop
grep -Fxq 'ExecStart=/usr/bin/studiocastd' \
  /usr/lib/systemd/user/studiocastd.service ||
  fail "the packaged user unit does not start /usr/bin/studiocastd"

echo "[verify-rpm] Removing studiocast"
dnf remove -y studiocast

for path in \
  /usr/bin/studiocast \
  /usr/bin/studiocastd \
  /usr/lib/systemd/user/studiocastd.service \
  /usr/share/applications/studiocast.desktop \
  /usr/share/icons/hicolor/scalable/apps/studiocast.svg \
  /usr/share/licenses/studiocast \
  /usr/share/doc/studiocast; do
  [ ! -e "${path}" ] || fail "removal left ${path} behind"
done

echo "[verify-rpm] Install test passed"
EOF
}

run_install_test() {
  if [[ "${USE_CONTAINER}" -eq 1 ]]; then
    local runtime
    runtime="$(find_container_runtime)" ||
      die "neither podman nor docker was found; cannot use --container"

    log "Running the install test in ${runtime} image ${IMAGE}"
    print_cmd "${runtime}" run --rm -i \
      --volume "${DIST_DIR}:/work/dist:Z" "${IMAGE}" \
      bash -s -- "${VERSION}" "${ARCH}" /work/dist
    install_test_script | "${runtime}" run --rm -i \
      --volume "${DIST_DIR}:/work/dist:Z" "${IMAGE}" \
      bash -s -- "${VERSION}" "${ARCH}" /work/dist
    return 0
  fi

  inside_container ||
    die "--install-test changes the system. Run it inside a container, or add --container."
  [[ "$(id -u)" -eq 0 ]] || die "--install-test needs root"

  log "Running the install test on this system"
  install_test_script | bash -s -- "${VERSION}" "${ARCH}" "${DIST_DIR}"
}

main() {
  parse_args "$@"

  command -v rpm >/dev/null 2>&1 ||
    die "the rpm command was not found; the package checks need it"

  local srpm="${DIST_DIR}/studiocast-${VERSION}-1.fc${FEDORA_RELEASE}.src.rpm"
  local rpm_file="${DIST_DIR}/studiocast-${VERSION}-1.fc${FEDORA_RELEASE}.${ARCH}.rpm"
  local checksum_file="${DIST_DIR}/studiocast-${VERSION}-rpm.sha256"

  require_file "${srpm}"
  require_file "${rpm_file}"
  require_file "${checksum_file}"

  verify_metadata "${srpm}" "${rpm_file}"
  verify_file_list "${rpm_file}"
  verify_dependencies "${rpm_file}"
  verify_checksums "${checksum_file}"

  if [[ "${INSTALL_TEST}" -eq 1 ]]; then
    run_install_test
  else
    log "Install test skipped; pass --install-test or --container to run it."
  fi

  log "Packaging artifact checks passed for studiocast-${VERSION}-1.fc${FEDORA_RELEASE}.${ARCH}."
}

main "$@"
