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
NO_CONTAINER_CHECK=0
# Set from the binary RPM metadata in main(); 1 when the package bundles dlib.
HAS_DLIB=0
# Set the same way; 1 when the package was built with the libyuv conditional.
HAS_LIBYUV=0

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
  --no-container-check    Let --install-test run on this system even when it
                          is not a detected container. Use it only where the
                          system is disposable, such as a CI job that already
                          runs inside a container.
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

  A package built with dlib declares Provides: bundled(dlib). For such a
  package the checks also expect the dlib license file, the FlexiBLAS run-time
  dependency, and the "MPL-2.0 AND BSL-1.0" license tag.

  A package built with libyuv declares Provides: studiocast(libyuv). For such a
  package the checks also expect the libyuv run-time dependency, and a package
  without that provide must not depend on libyuv at all.

  The install test also checks that the package installs with dnf while the
  weak dependency v4l2loopback is unavailable, that the programs run, that the
  desktop entry is valid, that the installed programs report dlib as compiled
  in when the package bundles it, and that removal leaves no files behind.
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

# Holds the lines that read_package_list read out of the last package.
PACKAGE_LIST=""

# Read one metadata list out of a package into PACKAGE_LIST. rpm runs on its
# own, so a package that rpm cannot read stops the script with its own message
# instead of one that says the metadata is missing.
read_package_list() {
  local package="$1"
  local query="$2"
  if ! PACKAGE_LIST="$(rpm -qp "${query}" "${package}" 2>&1)"; then
    die "rpm could not read $(basename "${package}"): ${PACKAGE_LIST}"
  fi
}

package_lists_path() {
  local package="$1"
  local path="$2"
  read_package_list "${package}" --list
  grep -Fxq -- "${path}" <<<"${PACKAGE_LIST}" ||
    die "$(basename "${package}") does not contain ${path}"
}

# True when the package declares the given dependency or provide. Only a
# missing entry gives false; an unreadable package stops the script.
package_declares() {
  local package="$1"
  local query="$2"
  local requirement="$3"
  read_package_list "${package}" "${query}"
  local names
  names="$(awk '{ print $1 }' <<<"${PACKAGE_LIST}")"
  grep -Fxq -- "${requirement}" <<<"${names}"
}

package_requires() {
  local package="$1"
  local requirement="$2"
  package_declares "${package}" --requires "${requirement}" ||
    die "$(basename "${package}") does not require ${requirement}"
}

package_recommends() {
  local package="$1"
  local requirement="$2"
  package_declares "${package}" --recommends "${requirement}" ||
    die "$(basename "${package}") does not recommend ${requirement}"
}

# True when the package was built with the dlib build conditional. The spec
# declares the static dlib with Provides: bundled(dlib), so the provide is the
# one piece of metadata that always tells the two builds apart.
package_bundles_dlib() {
  local package="$1"
  package_declares "${package}" --provides 'bundled(dlib)'
}

# True when the package was built with the libyuv build conditional. The spec
# declares Provides: studiocast(libyuv) for it, which is the only metadata that
# names the choice; the soname dependency below is the result to check against.
package_uses_libyuv() {
  local package="$1"
  package_declares "${package}" --provides 'studiocast(libyuv)'
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
      --no-container-check)
        NO_CONTAINER_CHECK=1
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

  # A dlib build bundles dlib, which is under the Boost Software License 1.0,
  # so the license tag must name it as well.
  local license="MPL-2.0"
  if [[ "${HAS_DLIB}" -eq 1 ]]; then
    license="MPL-2.0 AND BSL-1.0"
  fi

  require_equal "$(rpm_query "${rpm_file}" '%{name}')" "studiocast" \
    "binary RPM name"
  require_equal "$(rpm_query "${rpm_file}" '%{version}')" "${VERSION}" \
    "binary RPM version"
  require_equal "$(rpm_query "${rpm_file}" '%{release}')" "${release}" \
    "binary RPM release"
  require_equal "$(rpm_query "${rpm_file}" '%{license}')" "${license}" \
    "binary RPM license"
  require_equal "$(rpm_query "${rpm_file}" '%{arch}')" "${ARCH}" \
    "binary RPM architecture"

  require_equal "$(rpm_query "${srpm}" '%{name}')" "studiocast" "source RPM name"
  require_equal "$(rpm_query "${srpm}" '%{version}')" "${VERSION}" \
    "source RPM version"
  require_equal "$(rpm_query "${srpm}" '%{release}')" "${release}" \
    "source RPM release"
  require_equal "$(rpm_query "${srpm}" '%{license}')" "${license}" \
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

  if [[ "${HAS_DLIB}" -eq 1 ]]; then
    package_lists_path "${rpm_file}" \
      /usr/share/licenses/studiocast/dlib-LICENSE.txt
  fi

  # dlib is linked in statically, so the package must never ship a dlib
  # shared object of its own.
  if rpm -qpl "${rpm_file}" 2>/dev/null | grep -E '/libdlib\.so' >/dev/null; then
    die "$(basename "${rpm_file}") unexpectedly ships a dlib shared object"
  fi

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

  if [[ "${HAS_DLIB}" -eq 1 ]]; then
    # The static dlib calls CBLAS and LAPACK through FlexiBLAS, so rpmbuild
    # must have found the FlexiBLAS soname in the programs.
    package_requires "${rpm_file}" "libflexiblas.so.3()(64bit)"
  fi

  # The libyuv build conditional must decide the binary, both ways. A build
  # with libyuv links it, so the soname has to be in the dependencies; a build
  # without it must not pick a libyuv up from the build machine.
  if [[ "${HAS_LIBYUV}" -eq 1 ]]; then
    package_requires "${rpm_file}" "libyuv.so.0()(64bit)"
  elif package_declares "${rpm_file}" --requires "libyuv.so.0()(64bit)"; then
    die "$(basename "${rpm_file}") requires libyuv although it was built without the libyuv conditional"
  fi
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
has_dlib="$4"

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
dnf install -y --setopt=install_weak_deps=true "./${package}"
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

# dlib check. StudioCast reports dlib only through the Open Video Eye Contact
# effect, which needs a running daemon, a camera and an installed model pack,
# so no command line output can show it here. The compiled program is the
# cheapest observable instead. studiocastd is the program that carries the
# landmark code, so look there:
#   - with dlib the static library leaves its mangled type names, such as
#     N4dlib5errorE, in the read-only data;
#   - without dlib those names are gone and the program holds the literal
#     message that dlib_face_landmarks.cpp emits under !STUDIOCAST_HAVE_DLIB.
if [ "${has_dlib}" = "1" ]; then
  echo "[verify-rpm] Checking that dlib is compiled into studiocastd"
  rpm -q --provides studiocast | grep -Fq 'bundled(dlib)' ||
    fail "the installed package does not declare bundled(dlib)"
  if grep -aFq 'STUDIOCAST_HAVE_DLIB=0' /usr/bin/studiocastd; then
    fail "studiocastd still reports STUDIOCAST_HAVE_DLIB=0"
  fi
  grep -aFq 'N4dlib' /usr/bin/studiocastd ||
    fail "studiocastd holds no dlib type names"
  [ -f /usr/share/licenses/studiocast/dlib-LICENSE.txt ] ||
    fail "the dlib license file is missing"
  echo "[verify-rpm] dlib is compiled in"
else
  echo "[verify-rpm] Checking that this build has no dlib"
  if grep -aFq 'N4dlib' /usr/bin/studiocastd; then
    fail "studiocastd holds dlib type names in a build without dlib"
  fi
fi

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
      bash -s -- "${VERSION}" "${ARCH}" /work/dist "${HAS_DLIB}"
    install_test_script | "${runtime}" run --rm -i \
      --volume "${DIST_DIR}:/work/dist:Z" "${IMAGE}" \
      bash -s -- "${VERSION}" "${ARCH}" /work/dist "${HAS_DLIB}"
    return 0
  fi

  if [[ "${NO_CONTAINER_CHECK}" -eq 0 ]]; then
    inside_container ||
      die "--install-test changes the system. Run it inside a container, add --container, or add --no-container-check."
  fi
  [[ "$(id -u)" -eq 0 ]] || die "--install-test needs root"

  log "Running the install test on this system"
  install_test_script | bash -s -- "${VERSION}" "${ARCH}" "${DIST_DIR}" \
    "${HAS_DLIB}"
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

  if package_bundles_dlib "${rpm_file}"; then
    HAS_DLIB=1
    log "The package bundles dlib; running the dlib checks as well."
  else
    HAS_DLIB=0
    log "The package was built without dlib; skipping the dlib checks."
  fi

  if package_uses_libyuv "${rpm_file}"; then
    HAS_LIBYUV=1
    log "The package was built with libyuv; expecting the libyuv dependency."
  else
    HAS_LIBYUV=0
    log "The package was built without libyuv; expecting no libyuv dependency."
  fi

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
