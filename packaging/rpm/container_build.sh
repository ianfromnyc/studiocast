#!/usr/bin/env bash
# Builds the StudioCast RPM packages inside a Fedora container.
#
# packaging/rpm/build_rpm.sh --container copies this file into the build
# directory as container-build.sh, mounts that directory at /work, and runs it
# as root in the container. It is a checked-in file rather than a heredoc, so
# the shell linter reads it like every other script here.
#
# Usage: container_build.sh SRPM_ONLY RUN_RPMLINT [rpmbuild argument ...]
#   SRPM_ONLY     1 stops after the source RPM.
#   RUN_RPMLINT   1 installs rpmlint and prints its report.
#   The rest are passed to rpmbuild, such as --with dlib.
#
# Environment:
#   STUDIOCAST_FIX_OWNERSHIP   1 gives /work back to the host user at the end.
#                              build_rpm.sh sets it for docker, which runs as
#                              root, and not for podman.
#   STUDIOCAST_HOST_UID/GID    The host owner, needed for the above.
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "[rpm-container] ERROR: expected SRPM_ONLY and RUN_RPMLINT" >&2
  exit 2
fi

TOPDIR="/work/rpmbuild"
SPEC="${TOPDIR}/SPECS/studiocast.spec"
SRPM_ONLY="$1"
RUN_RPMLINT="$2"
shift 2
RPMBUILD_ARGS=("$@")
FIX_OWNERSHIP="${STUDIOCAST_FIX_OWNERSHIP:-0}"

echo "[rpm-container] Installing rpmbuild and the dependency resolver"
dnf install -y --setopt=install_weak_deps=false rpm-build rpmdevtools
dnf install -y --setopt=install_weak_deps=false dnf5-plugins ||
  dnf install -y --setopt=install_weak_deps=false dnf-plugins-core
if [[ "${RUN_RPMLINT}" -eq 1 ]]; then
  dnf install -y --setopt=install_weak_deps=false rpmlint
fi

echo "[rpm-container] Building the source RPM"
rpmbuild --define "_topdir ${TOPDIR}" "${RPMBUILD_ARGS[@]}" -bs "${SPEC}"

if [[ "${SRPM_ONLY}" -ne 1 ]]; then
  echo "[rpm-container] Installing the build dependencies"
  dnf builddep -y "${SPEC}"

  echo "[rpm-container] Building the binary RPMs"
  rpmbuild --define "_topdir ${TOPDIR}" "${RPMBUILD_ARGS[@]}" -bb "${SPEC}"
fi

if [[ "${RUN_RPMLINT}" -eq 1 ]]; then
  echo "[rpm-container] rpmlint report"
  rpmlint "${TOPDIR}/SRPMS" "${TOPDIR}/RPMS" || true
fi

if [[ "${FIX_OWNERSHIP}" -eq 1 ]]; then
  chown -R "${STUDIOCAST_HOST_UID}:${STUDIOCAST_HOST_GID}" /work
fi
