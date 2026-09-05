#!/usr/bin/env bash
# Installs the StudioCast binary RPM, runs the programs, then removes it.
#
# packaging/rpm/verify_rpm.sh feeds this file to bash as root, either in a
# container it starts or in a container the caller is already inside. It is a
# checked-in file rather than a heredoc, so the shell-lint job in
# .github/workflows/ci.yml reads it like every other script here. It changes the system, so do not run it on a machine
# you want to keep.
#
# Usage: install_test.sh VERSION ARCH DIST_DIR HAS_DLIB
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

# The dist tag is not known here, so match it with a glob. A loop keeps the
# glob out of an assignment, where it would stay literal.
package=""
for candidate in studiocast-"${version}"-1.fc*."${arch}".rpm; do
  [ -f "${candidate}" ] || continue
  package="${candidate}"
  break
done
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
#
# Both checks read the compiled program, so they pin the implementation, not
# the behaviour. A comment in src/core/open_video/dlib_face_landmarks.cpp says
# the message text is load-bearing here. The dlib type names would also
# disappear under -fno-rtti or an aggressive --gc-sections, so if this check
# ever gives a false alarm, expose the flag through studiocast-probe --json and
# read that instead.
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
