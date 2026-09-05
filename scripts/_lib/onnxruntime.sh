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
# On a distribution whose pkg-config does not search /usr/local/lib/pkgconfig,
# it also links the .pc file into a directory that pkg-config does search.
#
# scripts/uninstall/uninstall.sh removes exactly those paths.
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

# Directory of the shared libraries inside one bootstrap root.
#
# The upstream tarballs use lib, but some of them use lib64, so prefer lib64
# when it is there. Everything that looks inside a bootstrap root must ask this
# function, or it looks in the wrong place on a lib64 tarball.
sc_ort_libdir() {
  local root="$1"

  if [[ -d "${root}/lib64" ]]; then
    printf '%s/lib64\n' "${root}"
  else
    printf '%s/lib\n' "${root}"
  fi
}

# Download and install one upstream ONNX Runtime tarball.
#
# Arguments: <version> <asset name> <url> [sha256]
#
# The SHA-256 is optional because ONNX Runtime publishes no checksum asset. The
# GitHub release API does report a digest per asset, so the callers pass it when
# they know it. Without it the download is still fetched over HTTPS with
# --fail --location --retry 3.
#
# Returns non-zero when the download or the layout check fails, which ends the
# caller under set -e. The temporary directory goes either way.
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

  # Clean up with RETURN and ERR, never with EXIT. An EXIT trap here would
  # replace the trap of the caller, and a second call would drop the trap of
  # the first and leak its directory.
  #
  # RETURN covers a normal return, ERR covers the paths where set -e ends the
  # script. Both run while the frame of this function is still live, so they
  # can read the local tmpdir. The handler takes itself off again and puts both
  # traps of the caller back, so nothing of this call outlives it. The RETURN
  # trap matters under set -T, which passes the RETURN trap of the caller in.
  local prev_err_trap prev_return_trap
  prev_err_trap="$(trap -p ERR)"
  prev_return_trap="$(trap -p RETURN)"
  trap 'rm -rf "${tmpdir}"; trap - RETURN ERR; eval "${prev_err_trap}"; eval "${prev_return_trap}"' RETURN ERR

  curl --fail --silent --show-error --location --retry 3 "${url}" -o "${tmpdir}/${tgz}"

  if [[ -n "${sha256}" ]]; then
    sc_ort_log "Checking the SHA-256 of ${tgz}..."
    echo "${sha256}  ${tmpdir}/${tgz}" | sha256sum --check --status - \
      || { echo "[setup] ERROR: SHA-256 mismatch for ${tgz}" >&2; return 1; }
  fi

  sc_ort_priv mkdir -p "${prefix}"
  sc_ort_priv tar -xzf "${tmpdir}/${tgz}" -C "${prefix}"

  if [[ ! -f "${root}/include/onnxruntime_cxx_api.h" ]]; then
    echo "[setup] ERROR: ONNX Runtime headers not found at ${root}/include/onnxruntime_cxx_api.h" >&2
    return 1
  fi

  local libdir
  libdir="$(sc_ort_libdir "${root}")"

  if [[ ! -e "${libdir}/libonnxruntime.so" ]]; then
    local sofile
    # shellcheck disable=SC2012  # The upstream names have no spaces.
    sofile="$(ls -1 "${libdir}"/libonnxruntime.so.* 2>/dev/null | head -n 1 || true)"
    if [[ -z "${sofile}" ]]; then
      echo "[setup] ERROR: libonnxruntime.so not found under ${libdir}" >&2
      return 1
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

  sc_ort_link_pkgconfig
}

# Fedora's pkgconf searches only /usr/lib64/pkgconfig and /usr/share/pkgconfig,
# so the file written above is invisible there. Add a link in the first
# directory pkg-config does search. Debian and Ubuntu already search
# /usr/local/lib/pkgconfig, so this is a no-op for them.
#
# Fedora's onnxruntime-devel owns /usr/lib64/pkgconfig/libonnxruntime.pc, a
# different name, so the link never collides with a package file.
# Arguments: [bootstrap .pc file]  (default: the file written above)
#
# The link is a convenience, so nothing here may end the caller. A pkg-config
# that fails, or that names no search path, falls back to the two directories
# its list almost always starts with, the same fallback that
# scripts/uninstall/uninstall.sh uses.
sc_ort_link_pkgconfig() {
  local pc_file="${1:-/usr/local/lib/pkgconfig/onnxruntime.pc}"

  command -v pkg-config >/dev/null 2>&1 || return 0
  [[ -f "${pc_file}" ]] || return 0

  if pkg-config --exists onnxruntime; then
    return 0
  fi

  local pc_path=""
  pc_path="$(pkg-config --variable pc_path pkg-config 2>/dev/null || true)"

  local -a dirs=()
  if [[ -n "${pc_path}" ]]; then
    mapfile -t dirs < <(printf '%s\n' "${pc_path}" | tr ':' '\n' | grep -v '^$' || true)
  fi
  if [[ "${#dirs[@]}" -eq 0 ]]; then
    dirs=(/usr/lib64/pkgconfig /usr/lib/pkgconfig)
  fi

  local dir="${dirs[0]}"
  sc_ort_log "pkg-config does not search $(dirname "${pc_file}"); linking into ${dir}."
  if ! sc_ort_priv mkdir -p "${dir}" ||
    ! sc_ort_priv ln -sfn "${pc_file}" "${dir}/onnxruntime.pc"; then
    sc_ort_log "Could not link onnxruntime.pc into ${dir}; going on without it."
  fi

  return 0
}
