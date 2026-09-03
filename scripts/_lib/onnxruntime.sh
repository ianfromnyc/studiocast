# shellcheck shell=bash
#
# Shared ONNX Runtime bootstrap for the distro setup helpers.
#
# Source this file, then call sc_ort_install_tarball. The Ubuntu helper and the
# Fedora helper both use it, so both write the same layout:
#
#   /opt/studiocast/onnxruntime/<version>/<asset name>/{include,lib}
#   /etc/ld.so.conf.d/studiocast-onnxruntime.conf
#   /usr/local/lib/pkgconfig/onnxruntime.pc
#
# scripts/uninstall/uninstall.sh removes exactly those three paths.
#
# The caller must define two helpers before it sources this file:
#   sc_ort_log <message>      Print one log line.
#   sc_ort_priv <command...>  Run one command as root.

if [[ -n "${STUDIOCAST_ORT_LIB_SOURCED:-}" ]]; then
  return 0
fi
STUDIOCAST_ORT_LIB_SOURCED=1

# Upstream asset base name for the releases from before the CUDA split:
#   cpu -> onnxruntime-linux-<arch>-<version>
#   gpu -> onnxruntime-linux-<arch>-gpu-<version>
sc_ort_legacy_asset_name() {
  local arch="$1"
  local flavor="$2"
  local version="$3"

  if [[ "${flavor}" == "cpu" ]]; then
    printf 'onnxruntime-linux-%s-%s\n' "${arch}" "${version}"
  else
    printf 'onnxruntime-linux-%s-%s-%s\n' "${arch}" "${flavor}" "${version}"
  fi
}

# Directory that holds the extracted tarball for one version.
sc_ort_prefix() {
  printf '/opt/studiocast/onnxruntime/%s\n' "$1"
}

# Directory of the extracted tarball itself.
sc_ort_root() {
  printf '/opt/studiocast/onnxruntime/%s/%s\n' "$1" "$2"
}

# Print the newest installed bootstrap root, or nothing when there is none.
sc_ort_installed_root() {
  local root
  local -a roots=()

  for root in /opt/studiocast/onnxruntime/*/*/; do
    [[ -f "${root}include/onnxruntime_cxx_api.h" ]] || continue
    roots+=("${root%/}")
  done

  [[ "${#roots[@]}" -gt 0 ]] || return 0
  printf '%s\n' "${roots[@]}" | sort -V | tail -n 1
}

# Download and install one upstream ONNX Runtime tarball.
#
# Arguments: <version> <asset name> <url> [sha256]
#
# The SHA-256 is optional because ONNX Runtime publishes no checksum asset. The
# GitHub release API does report a digest per asset, so the callers pass it when
# they know it. Without it the download is still fetched over HTTPS with
# --fail --location --retry 3.
sc_ort_install_tarball() {
  local version="$1"
  local asset_name="$2"
  local url="$3"
  local sha256="${4:-}"

  local tgz="${asset_name}.tgz"
  local prefix
  prefix="$(sc_ort_prefix "${version}")"
  local root="${prefix}/${asset_name}"

  local tmpdir
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "${tmpdir}"' EXIT

  curl --fail --silent --show-error --location --retry 3 "${url}" -o "${tmpdir}/${tgz}"

  if [[ -n "${sha256}" ]]; then
    sc_ort_log "Checking the SHA-256 of ${tgz}..."
    echo "${sha256}  ${tmpdir}/${tgz}" | sha256sum --check --status - \
      || { echo "[setup] ERROR: SHA-256 mismatch for ${tgz}" >&2; exit 1; }
  fi

  sc_ort_priv mkdir -p "${prefix}"
  sc_ort_priv tar -xzf "${tmpdir}/${tgz}" -C "${prefix}"

  if [[ ! -f "${root}/include/onnxruntime_cxx_api.h" ]]; then
    echo "[setup] ERROR: ONNX Runtime headers not found at ${root}/include/onnxruntime_cxx_api.h"
    exit 1
  fi

  local libdir="${root}/lib"
  if [[ -d "${root}/lib64" ]]; then
    libdir="${root}/lib64"
  fi

  if [[ ! -e "${libdir}/libonnxruntime.so" ]]; then
    local sofile
    # shellcheck disable=SC2012  # The upstream names have no spaces.
    sofile="$(ls -1 "${libdir}"/libonnxruntime.so.* 2>/dev/null | head -n 1 || true)"
    if [[ -z "${sofile}" ]]; then
      echo "[setup] ERROR: libonnxruntime.so not found under ${libdir}"
      exit 1
    fi
    sc_ort_priv ln -sf "$(basename "${sofile}")" "${libdir}/libonnxruntime.so"
  fi

  # Make the shared library discoverable for runtime linking.
  sc_ort_priv tee /etc/ld.so.conf.d/studiocast-onnxruntime.conf >/dev/null <<EOF
${libdir}
EOF
  sc_ort_priv ldconfig

  # Provide a pkg-config file so our CMake can pick it up via
  # pkg_check_modules(onnxruntime).
  sc_ort_priv mkdir -p /usr/local/lib/pkgconfig
  sc_ort_priv tee /usr/local/lib/pkgconfig/onnxruntime.pc >/dev/null <<EOF
prefix=${root}
exec_prefix=\${prefix}
libdir=${libdir}
includedir=\${prefix}/include

Name: onnxruntime
Description: ONNX Runtime
Version: ${version}
Libs: -L\${libdir} -lonnxruntime
Cflags: -I\${includedir}
EOF
}
