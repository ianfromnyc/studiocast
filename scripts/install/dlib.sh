#!/usr/bin/env bash
set -euo pipefail

# StudioCast dlib source installer.
#
# Fedora ships no dlib package, so a source build of StudioCast on Fedora has
# no face landmark library and Open Video Eye Contact stays unavailable. This
# script builds the same pinned dlib release that the RPM uses, from
# packaging/rpm/dlib.lock, and installs it into a private prefix. It then
# prints the -Ddlib_DIR value to pass to the StudioCast CMake configure step.
#
# The build flags match packaging/rpm/studiocast.spec.in: a static library, no
# GUI, no image codecs, no CUDA, and BLAS and LAPACK through FlexiBLAS.
#
# Example:
#   sudo dnf install cmake ninja-build gcc-c++ flexiblas-devel
#   ./scripts/install/dlib.sh
#   cmake -S . -B build -Ddlib_DIR=/opt/studiocast/dlib/<ver>/lib64/cmake/dlib

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DLIB_LOCK="${REPO_ROOT}/packaging/rpm/dlib.lock"

PREFIX_ROOT="/opt/studiocast/dlib"
PREFIX=""
JOBS="$(nproc 2>/dev/null || echo 4)"
DRY_RUN=0
YES=0

usage() {
  cat <<EOF
StudioCast dlib source installer

Usage:
  ./scripts/install/dlib.sh [options]

Options:
  --prefix DIR   Install directory. Default: ${PREFIX_ROOT}/<version>
  --jobs N       Parallel compile jobs. Default: ${JOBS}
  --dry-run      Print the commands without running them.
  -y, --yes      Do not ask before the install step.
  -h, --help     Show this help.

Notes:
  The dlib release, its URL and its SHA256 come from
  packaging/rpm/dlib.lock. Bump the pin there, never here.

  The build needs cmake, ninja-build, gcc-c++, curl, tar and flexiblas-devel.
  Writing to ${PREFIX_ROOT} needs root, so the script calls sudo for the
  install step. Everything else runs as the calling user in a temporary
  directory.

  The Fedora RPM does not need this script. packaging/rpm/build_rpm.sh builds
  the same dlib inside the RPM build tree.
EOF
}

log() { printf '[dlib] %s\n' "$*" >&2; }

die() {
  printf '[dlib] ERROR: %s\n' "$*" >&2
  exit 1
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

# Mirrors scripts/setup/ubuntu.sh: the GUI installer feeds the password on
# standard input. A root shell needs no sudo at all.
if [[ "${STUDIOCAST_GUI_SUDO_STDIN:-0}" == "1" ]]; then
  sudo() {
    command sudo -S -p "${STUDIOCAST_GUI_SUDO_PROMPT:-[sudo] password for %u: }" "$@"
  }
elif [[ "$(id -u)" -eq 0 ]]; then
  sudo() {
    "$@"
  }
fi

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --prefix)
        [[ $# -ge 2 ]] || die "--prefix requires a directory"
        PREFIX="$2"
        shift 2
        ;;
      --jobs)
        [[ $# -ge 2 ]] || die "--jobs requires a number"
        [[ "$2" =~ ^[0-9]+$ ]] || die "--jobs must be a number: $2"
        JOBS="$2"
        shift 2
        ;;
      --dry-run)
        DRY_RUN=1
        shift
        ;;
      -y|--yes)
        YES=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        die "Unknown option: $1"
        ;;
    esac
  done
}

load_lock() {
  [[ -f "${DLIB_LOCK}" ]] || die "dlib lock file is missing: ${DLIB_LOCK}"
  # shellcheck source-path=SCRIPTDIR
  # shellcheck source=../../packaging/rpm/dlib.lock
  source "${DLIB_LOCK}"
  [[ -n "${DLIB_VERSION:-}" ]] || die "${DLIB_LOCK} does not set DLIB_VERSION"
  [[ -n "${DLIB_URL:-}" ]] || die "${DLIB_LOCK} does not set DLIB_URL"
  [[ -n "${DLIB_SHA256:-}" ]] || die "${DLIB_LOCK} does not set DLIB_SHA256"
  if [[ -z "${PREFIX}" ]]; then
    PREFIX="${PREFIX_ROOT}/${DLIB_VERSION}"
  fi
}

confirm() {
  if [[ "${YES}" -eq 1 || "${DRY_RUN}" -eq 1 ]]; then
    return 0
  fi
  local answer=""
  printf '[dlib] Install dlib %s into %s? [y/N] ' "${DLIB_VERSION}" "${PREFIX}" >&2
  read -r answer || true
  case "${answer}" in
    y|Y|yes|YES) return 0 ;;
    *) die "cancelled" ;;
  esac
}

# Downloads the pinned tarball and checks it before anything unpacks it.
fetch_source() {
  local archive="$1"
  log "Downloading ${DLIB_URL}"
  run curl --fail --location --retry 3 --output "${archive}" "${DLIB_URL}"

  if [[ "${DRY_RUN}" -eq 1 ]]; then
    print_cmd sha256sum "${archive}"
    return 0
  fi

  local actual
  actual="$(sha256sum -- "${archive}" | awk '{ print $1 }')"
  [[ "${actual}" == "${DLIB_SHA256}" ]] ||
    die "dlib source checksum mismatch: expected ${DLIB_SHA256}, got ${actual}"
  log "dlib source SHA256 matches the pin"
}

build_and_install() {
  local workdir="$1"
  local src="${workdir}/dlib-${DLIB_VERSION}"
  local build="${workdir}/dlib-build"

  # These flags must stay the same as the ones in
  # packaging/rpm/studiocast.spec.in, so a source build and an RPM build get
  # the same library. StudioCast uses the shape predictor and the plain image
  # processing headers only, so every codec, the GUI and CUDA are off.
  run cmake -S "${src}" -B "${build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_INSTALL_LIBDIR=lib64 \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBLA_VENDOR=FlexiBLAS \
    -DDLIB_NO_GUI_SUPPORT=ON \
    -DDLIB_USE_CUDA=OFF \
    -DDLIB_USE_BLAS=ON \
    -DDLIB_USE_LAPACK=ON \
    -DDLIB_USE_MKL_FFT=OFF \
    -DDLIB_USE_FFMPEG=OFF \
    -DDLIB_JPEG_SUPPORT=OFF \
    -DDLIB_PNG_SUPPORT=OFF \
    -DDLIB_GIF_SUPPORT=OFF \
    -DDLIB_WEBP_SUPPORT=OFF \
    -DDLIB_JXL_SUPPORT=OFF \
    -DDLIB_LINK_WITH_SQLITE3=OFF

  run cmake --build "${build}" --parallel "${JOBS}"

  confirm
  log "Installing into ${PREFIX}"
  run sudo cmake --install "${build}"
}

main() {
  parse_args "$@"
  load_lock

  require_cmd curl
  require_cmd tar
  require_cmd sha256sum
  if [[ "${DRY_RUN}" -eq 0 ]]; then
    require_cmd cmake
    require_cmd ninja
  fi

  log "dlib ${DLIB_VERSION} -> ${PREFIX}"

  local workdir
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    workdir="/tmp/studiocast-dlib.XXXXXX"
    print_cmd mktemp -d -t studiocast-dlib.XXXXXX
  else
    workdir="$(mktemp -d -t studiocast-dlib.XXXXXX)"
    # shellcheck disable=SC2064
    trap "rm -rf -- '${workdir}'" EXIT
  fi

  local archive="${workdir}/dlib-${DLIB_VERSION}.tar.gz"
  fetch_source "${archive}"
  run tar -xzf "${archive}" -C "${workdir}"
  build_and_install "${workdir}"

  log "Done. Configure StudioCast with:"
  printf '  -Ddlib_DIR=%s/lib64/cmake/dlib\n' "${PREFIX}"
}

main "$@"
