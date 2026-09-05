#!/usr/bin/env bash

# StudioCast Fedora setup helper.
#
# This script is invoked via ./scripts/setup.sh on a Fedora-family
# distribution, and is tested on Fedora 44. It installs build/runtime
# prerequisites and configures v4l2loopback.
#
# Everything down to the source guard is definitions and plain defaults, plus
# scripts/_lib/onnxruntime.sh, which this file sources. The tests read those
# defaults. The shell options, the system probes and the option parsing come
# after the guard, so that a shell which sources this file keeps its own
# settings.
#
# Differences from the Ubuntu helper:
# - The cpu ONNX Runtime flavor comes from the distro package onnxruntime-devel.
#   The gpu flavor comes from the upstream CUDA tarball, like on Ubuntu.
# - v4l2loopback is not in Fedora. It is in RPM Fusion Free, which this script
#   never enables for you.
# - The CUDA runtime rpms come from the NVIDIA repository, which this script
#   never enables for you either.

usage() {
  cat <<'EOF'
Usage:
  ./scripts/setup.sh [options]          # on Fedora this runs scripts/setup/fedora.sh
  ./scripts/setup/fedora.sh [options]

Options:
  --deps                  Install build/runtime deps with dnf (Qt6/CMake/Ninja/etc + Pulse utils).
  --v4l2loopback          Ensure the v4l2loopback module is available (needs RPM Fusion Free).
  --load-loopback         Load v4l2loopback now (creates /dev/videoN).
  --persist-loopback      Persist module load/options across reboot.

  --video-nr N            v4l2loopback device number (default: 10).
  --label TEXT            v4l2loopback card label (default: "StudioCast Camera").
  --exclusive-caps 0|1    v4l2loopback exclusive_caps (default: 1).

  --onnxruntime-version V ONNX Runtime version for the gpu flavor (default: 1.29.0).
  --onnxruntime-flavor F  cpu|gpu (default: auto; gpu if nvidia-smi works, else cpu).
  --onnxruntime-arch A    x64|aarch64 (default: auto from uname -m).
  --cuda-major 12|13      CUDA major version for the gpu tarball (default: auto, else 13).
  --cudnn-version V       cuDNN redistributable version (default: 9.25.1.1).
  --check-cuda            Report the CUDA runtime state and exit. Installs nothing.

  --build                 Configure + build StudioCast (dev convenience).
  --build-dir DIR         Build directory (default: ./cmake-build-debug).
  --build-type TYPE       CMake build type (default: Debug).

  --rpm                   Run packaging/rpm/build_rpm.sh. Arguments after -- go to it.
  --maxine                Run scripts/setup/maxine.sh. Arguments after -- go to it.
  -y, --yes               Assume yes for dnf installs.
  -h, --help              Show help.

Fedora notes:
  - ONNX Runtime, cpu flavor: the Fedora package onnxruntime-devel. CMake finds
    it through its CMake config file. That package has the CPU execution
    provider only, so the Open CUDA backend cannot use it.
  - ONNX Runtime, gpu flavor: the upstream CUDA tarball, installed under
    /opt/studiocast/onnxruntime/<version>/ with a pkg-config file, the same as
    on Ubuntu. This flavor does not install onnxruntime-devel, because the
    distro CMake config file would otherwise hide the GPU build. --build then
    passes -DONNXRUNTIME_ROOT so CMake uses the tarball.
  - The gpu flavor needs the CUDA 13 runtime rpms and cuDNN 9. The CUDA rpms
    come from the NVIDIA repository, which this script never enables for you:
      sudo dnf config-manager addrepo --from-repofile=https://developer.download.nvidia.com/compute/cuda/repos/fedora44/x86_64/cuda-fedora44.repo
    Fedora and NVIDIA have no cuDNN rpm for Fedora 44, so this script installs
    the NVIDIA cuDNN redistributable tarball under /opt/studiocast/cudnn/.
  - ONNX Runtime 1.29 with the CUDA execution provider needs CUDA 13.x, cuDNN
    9.x and an NVIDIA driver 580.65.06 or newer.
  - Fedora has no dlib package. CMake reports this and disables the Open Video
    Eye Contact effect.
  - v4l2loopback is not in Fedora. It is in RPM Fusion Free as
    akmod-v4l2loopback. Enable that repository first:
      sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
  - The GUI installer wizard stays Ubuntu-only. Use this script on Fedora.

Examples:
  ./scripts/setup.sh --deps -y
  ./scripts/setup.sh --deps --onnxruntime-flavor gpu -y
  ./scripts/setup.sh --check-cuda
  ./scripts/setup.sh --deps --v4l2loopback --load-loopback --persist-loopback
  ./scripts/setup.sh --build --build-type Release
  ./scripts/setup.sh --rpm -- --clean
EOF
}

# v4l2loopback defaults. The functions below read them, so they stay here.
VIDEO_NR=10
LABEL="StudioCast Camera"
EXCLUSIVE_CAPS=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { echo "[setup] $*"; }
warn() { echo "[setup] WARNING: $*" >&2; }

# ONNX Runtime install defaults.
# - 1.29.0 is the newest stable upstream release with a CUDA 13 Linux tarball.
#   Its CUDA execution provider needs CUDA 13.x, cuDNN 9.x and an NVIDIA driver
#   580.65.06 or newer. Its Linux x64 CUDA 13 build covers compute capability
#   75, 80, 86, 89, 90a and 120a.
# - Prefer the gpu flavor when an NVIDIA driver is present, like the Ubuntu
#   helper does.
ORT_VERSION="${ORT_VERSION:-1.29.0}"
CUDNN_VERSION="${CUDNN_VERSION:-9.25.1.1}"

# Where the cuDNN redistributable installs live. Tests can point this at a
# sandbox; everything else uses the real directory.
CUDNN_INSTALL_DIR="${STUDIOCAST_CUDNN_INSTALL_DIR:-/opt/studiocast/cudnn}"

# CUDA major version of the installed toolkit, or 13 when there is none.
detect_cuda_major() {
  local version_json="/usr/local/cuda/version.json"
  if [[ -f "${version_json}" ]]; then
    local v
    v="$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([0-9]\+\)\..*/\1/p' "${version_json}" | head -n 1)"
    if [[ -n "${v}" ]]; then
      printf '%s\n' "${v}"
      return 0
    fi
  fi

  if command -v nvcc >/dev/null 2>&1; then
    local v
    v="$(nvcc --version 2>/dev/null | sed -n 's/.*release \([0-9]\+\)\..*/\1/p' | head -n 1)"
    if [[ -n "${v}" ]]; then
      printf '%s\n' "${v}"
      return 0
    fi
  fi

  printf '13\n'
}

require_cmd() {
  command -v "$1" >/dev/null 2>&1 || { echo "[setup] Missing required command: $1"; exit 1; }
}

# Stop with a message when an option that takes a value is the last argument.
# Call it with the arguments that are left: "$1" is the option itself. Without
# this, "shift 2" fails and set -e ends the script with no output at all.
need_value() {
  [[ "$#" -ge 2 ]] || { echo "[setup] ERROR: $1 needs a value" >&2; exit 2; }
}

# Run a command as root. Root shells (containers, rescue systems) have no sudo.
run_priv() {
  if [[ "$(id -u)" -eq 0 ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

# Hooks for scripts/_lib/onnxruntime.sh.
sc_ort_log() { log "$@"; }
sc_ort_priv() { run_priv "$@"; }

# shellcheck source-path=SCRIPTDIR
# shellcheck source=../_lib/onnxruntime.sh
source "${SCRIPT_DIR}/../_lib/onnxruntime.sh"

DNF_ARGS=()

dnf_install() {
  require_cmd dnf
  run_priv dnf install "${DNF_ARGS[@]}" "$@"
}

# ---------------------------------------------------------------------------
# ONNX Runtime, gpu flavor
# ---------------------------------------------------------------------------

# True when version "$1" is the same as or newer than version "$2".
version_at_least() {
  [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" == "$2" ]]
}

# Upstream publishes gpu tarballs for CUDA 12 and CUDA 13 only, so no other
# major can give a working gpu install.
cuda_major_supported() {
  [[ "${CUDA_MAJOR}" == "12" || "${CUDA_MAJOR}" == "13" ]]
}

# Upstream asset base name for the gpu flavor.
#
# The upstream naming changed twice:
#   before 1.25.0  onnxruntime-linux-<arch>-gpu-<version>            (CUDA 11/12)
#   1.25 and 1.26  ...-gpu-<version> is CUDA 12, ...-gpu_cuda13-... exists
#   1.27.0 onward  ...-gpu_cuda12-<version> and ...-gpu_cuda13-<version>
onnxruntime_gpu_asset_name() {
  local arch="$1"
  local cuda_major="$2"
  local version="$3"

  local split_from="1.27.0"
  if [[ "${cuda_major}" == "13" ]]; then
    split_from="1.25.0"
  fi

  if version_at_least "${version}" "${split_from}"; then
    printf 'onnxruntime-linux-%s-gpu_cuda%s-%s\n' "${arch}" "${cuda_major}" "${version}"
  else
    warn "ONNX Runtime ${version} has no gpu_cuda${cuda_major} asset; using the old"
    warn "'gpu' asset name. That build is for CUDA 11 or CUDA 12, not CUDA ${cuda_major},"
    warn "so it does not match the CUDA ${cuda_major} runtime this setup installs."
    sc_ort_legacy_asset_name "${arch}" "gpu" "${version}"
  fi
}

# SHA-256 of the assets this script installs by default. ONNX Runtime publishes
# no checksum asset; these are the asset digests from the GitHub release API.
# Other versions install without a checksum, and the script says so.
onnxruntime_known_sha256() {
  case "$1" in
    onnxruntime-linux-x64-gpu_cuda13-1.29.0.tgz)
      printf '844c64acfc43ab9423215c26493055ea229268e28283146cc644ecef0bdae048\n' ;;
    onnxruntime-linux-x64-gpu_cuda12-1.29.0.tgz)
      printf '4ca594a0da83927befbd73fe020d7f569be151d70bb4fe9741ad405f4882e2ad\n' ;;
    *)
      printf '\n' ;;
  esac
}

# The bootstrap root this run asks for.
#
# The default version, arch and CUDA major name a root as much as the matching
# options do: the install path uses them, so the build path must read the same
# root. A run that installs nothing still gets the root it would install.
#
# Only the gpu flavor installs a bootstrap root, so a cpu run asks for none.
requested_onnxruntime_root() {
  [[ "${ORT_FLAVOR}" == "gpu" ]] || return 0

  local asset
  # The warning about a version with no CUDA asset belongs to the install path,
  # which prints it once. This is only a name lookup.
  asset="$(onnxruntime_gpu_asset_name "${ORT_ARCH}" "${CUDA_MAJOR}" \
    "${ORT_VERSION}" 2>/dev/null)"
  sc_ort_root "${ORT_VERSION}" "${asset}"
}

# The bootstrap root to read: the one this run asks for when it is installed,
# else the newest one. Everything that looks at the installed bootstrap goes
# through here, so a run that asks for one version never reads another one.
installed_onnxruntime_root() {
  sc_ort_installed_root "$(requested_onnxruntime_root)"
}

warn_if_distro_onnxruntime_installed() {
  rpm -q onnxruntime-devel >/dev/null 2>&1 || return 0

  warn "onnxruntime-devel is installed. Its CMake config file has the CPU"
  warn "execution provider only, and CMake finds it before this GPU build."
  warn "--build works around this with -DONNXRUNTIME_ROOT, but any other"
  warn "configure command picks the CPU build. Remove it with:"
  warn "  sudo dnf remove onnxruntime-devel"
}

ensure_onnxruntime_gpu_available() {
  require_cmd curl
  require_cmd tar

  local asset_name
  asset_name="$(onnxruntime_gpu_asset_name "${ORT_ARCH}" "${CUDA_MAJOR}" "${ORT_VERSION}")"
  local root
  root="$(sc_ort_root "${ORT_VERSION}" "${asset_name}")"

  if [[ -f "${root}/include/onnxruntime_cxx_api.h" ]]; then
    log "ONNX Runtime ${ORT_VERSION} is already installed at ${root}; skipping the download."
    sc_ort_link_pkgconfig
    return 0
  fi

  local url="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${asset_name}.tgz"
  local sha256
  sha256="$(onnxruntime_known_sha256 "${asset_name}.tgz")"

  # Name the asset, not the CUDA major of the options: a version from before
  # the CUDA split has one gpu asset for every major.
  log "Installing ONNX Runtime ${ORT_VERSION} (${asset_name}) from: ${url}"
  log "  -> ${root}"
  if [[ -z "${sha256}" ]]; then
    log "No published SHA-256 is known for this asset; the download is not checksummed."
  fi

  sc_ort_install_tarball "${ORT_VERSION}" "${asset_name}" "${url}" "${sha256}"

  require_cmd pkg-config
  log "ONNX Runtime installed; pkg-config reports: $(pkg-config --modversion onnxruntime)"
}

# ---------------------------------------------------------------------------
# cuDNN 9
# ---------------------------------------------------------------------------

# NVIDIA names the redistributable directory after the machine, not after the
# ONNX Runtime arch token.
cudnn_arch() {
  case "${ORT_ARCH}" in
    aarch64) printf 'sbsa\n' ;;
    *) printf 'x86_64\n' ;;
  esac
}

cudnn_archive_name() {
  printf 'cudnn-linux-%s-%s_cuda%s-archive\n' "$(cudnn_arch)" "${CUDNN_VERSION}" "${CUDA_MAJOR}"
}

# The redistributable index file drops the last version component:
# cuDNN 9.25.1.1 is described by redistrib_9.25.1.json.
cudnn_release_label() {
  printf '%s\n' "${CUDNN_VERSION}" | cut -d. -f1-3
}

# Read the SHA-256 of one archive out of the NVIDIA redistributable index.
# Prints nothing when the index or the entry is missing.
cudnn_published_sha256() {
  local archive="$1"
  local label
  label="$(cudnn_release_label)"
  local index_url="https://developer.download.nvidia.com/compute/cudnn/redist/redistrib_${label}.json"

  local index
  index="$(curl --fail --silent --show-error --location --retry 3 "${index_url}" 2>/dev/null || true)"
  [[ -n "${index}" ]] || return 0

  # An index without the entry means "no published SHA-256", not an error, so
  # keep the lookup best-effort. Without the || true, grep finding nothing
  # would make the pipeline fail under pipefail and set -e would end the setup.
  printf '%s\n' "${index}" \
    | grep -A2 -F "\"relative_path\": \"cudnn/linux-$(cudnn_arch)/${archive}.tar.xz\"" \
    | sed -n 's/.*"sha256"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{64\}\)".*/\1/p' \
    | head -n 1 \
    || true
}

# Point ldconfig at the cuDNN libraries of one install.
write_cudnn_ld_conf() {
  run_priv tee /etc/ld.so.conf.d/studiocast-cudnn.conf >/dev/null <<EOF
$1
EOF
  run_priv ldconfig
}

ensure_cudnn_available() {
  if lib_resolves libcudnn.so.9; then
    log "libcudnn.so.9 already resolves through ldconfig; skipping the cuDNN install."
    return 0
  fi

  local archive arch
  archive="$(cudnn_archive_name)"
  arch="$(cudnn_arch)"
  local prefix="${CUDNN_INSTALL_DIR}/${CUDNN_VERSION}"
  local root="${prefix}/${archive}"
  local libdir="${root}/lib"

  # The tree can be there while the ld.so.conf.d entry is gone, which is what
  # ./scripts/uninstall.sh --greedy leaves. The path names the version, the
  # architecture and the CUDA major, so a tree here is the archive this call
  # would fetch. Write the entry again instead of downloading 850 MB again.
  if [[ -e "${libdir}/libcudnn.so.9" ]]; then
    log "cuDNN ${CUDNN_VERSION} is already installed at ${root}; skipping the download."
    write_cudnn_ld_conf "${libdir}"
    return 0
  fi

  require_cmd curl
  require_cmd tar

  local url="https://developer.download.nvidia.com/compute/cudnn/redist/cudnn/linux-${arch}/${archive}.tar.xz"

  log "Installing cuDNN ${CUDNN_VERSION} (CUDA ${CUDA_MAJOR}) from: ${url}"
  log "  -> ${root}"

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
  # This is the same shape as sc_ort_install_tarball in
  # scripts/_lib/onnxruntime.sh.
  local prev_err_trap prev_return_trap
  prev_err_trap="$(trap -p ERR)"
  prev_return_trap="$(trap -p RETURN)"
  trap 'rm -rf "${tmpdir}"; trap - RETURN ERR; eval "${prev_err_trap}"; eval "${prev_return_trap}"' RETURN ERR

  curl --fail --silent --show-error --location --retry 3 "${url}" -o "${tmpdir}/${archive}.tar.xz"

  local sha256
  sha256="$(cudnn_published_sha256 "${archive}")"
  if [[ -n "${sha256}" ]]; then
    log "Checking the SHA-256 of ${archive}.tar.xz..."
    echo "${sha256}  ${tmpdir}/${archive}.tar.xz" | sha256sum --check --status - \
      || { echo "[setup] ERROR: SHA-256 mismatch for ${archive}.tar.xz" >&2; return 1; }
  else
    log "The NVIDIA redistributable index has no SHA-256 for this archive; skipping the check."
  fi

  run_priv mkdir -p "${prefix}"
  run_priv tar -xJf "${tmpdir}/${archive}.tar.xz" -C "${prefix}"

  if [[ ! -e "${libdir}/libcudnn.so.9" ]]; then
    echo "[setup] ERROR: libcudnn.so.9 not found under ${libdir}" >&2
    return 1
  fi

  write_cudnn_ld_conf "${libdir}"

  log "cuDNN installed at ${root}."
}

# ---------------------------------------------------------------------------
# CUDA runtime preflight
# ---------------------------------------------------------------------------

ldconfig_cmd() {
  if command -v ldconfig >/dev/null 2>&1; then
    printf 'ldconfig\n'
  elif [[ -x /sbin/ldconfig ]]; then
    printf '/sbin/ldconfig\n'
  else
    printf '\n'
  fi
}

# Directories the CUDA rpms use when they do not install into /usr/lib64.
cuda_search_dirs() {
  local d
  for d in "/usr/local/cuda-${CUDA_MAJOR}"*/targets/*/lib /usr/local/cuda/targets/*/lib; do
    [[ -d "${d}" ]] && printf '%s\n' "${d}"
  done
  return 0
}

# Print where a soname resolves, or nothing when it does not resolve.
lib_location() {
  local soname="$1"

  local ldc
  ldc="$(ldconfig_cmd)"
  if [[ -n "${ldc}" ]]; then
    local hit
    hit="$("${ldc}" -p 2>/dev/null | sed -n "s|^[[:space:]]*${soname} .*=> ||p" | head -n 1)"
    if [[ -n "${hit}" ]]; then
      printf '%s\n' "${hit}"
      return 0
    fi
  fi

  local d
  while read -r d; do
    if [[ -e "${d}/${soname}" ]]; then
      printf '%s\n' "${d}/${soname}"
      return 0
    fi
  done < <(cuda_search_dirs)

  return 1
}

lib_resolves() {
  lib_location "$1" >/dev/null 2>&1
}

# The CUDA libraries the ONNX Runtime CUDA execution provider needs.
#
# libcuda.so.1 comes from the NVIDIA driver, never from a CUDA toolkit package.
# libcudnn and libnvrtc are opened with dlopen, so they are not in the ELF
# NEEDED list of the provider. Anything else the installed provider links is
# added to the list, so a different ONNX Runtime version stays covered.
cuda_required_libs() {
  {
    printf 'libcuda.so.1\n'
    printf 'libcudart.so.%s\n' "${CUDA_MAJOR}"
    printf 'libcublas.so.%s\n' "${CUDA_MAJOR}"
    printf 'libcublasLt.so.%s\n' "${CUDA_MAJOR}"
    printf 'libcurand.so.10\n'
    printf 'libnvrtc.so.%s\n' "${CUDA_MAJOR}"
    printf 'libcudnn.so.9\n'

    local root provider=""
    root="$(installed_onnxruntime_root)"
    # The bootstrap root holds its libraries in lib or in lib64, so ask the
    # shared helper instead of assuming lib.
    if [[ -n "${root}" ]]; then
      provider="$(sc_ort_libdir "${root}")/libonnxruntime_providers_cuda.so"
    fi
    # The probe only adds to the fixed list, so it must never end the caller.
    # Without the || true, an objdump that cannot read the provider makes the
    # pipeline fail under pipefail, and set -e then ends --deps or --check-cuda.
    if [[ -n "${provider}" && -f "${provider}" ]] \
        && command -v objdump >/dev/null 2>&1; then
      objdump -p "${provider}" 2>/dev/null \
        | sed -n 's/^[[:space:]]*NEEDED[[:space:]]*\(lib\(cu\|nv\)[^[:space:]]*\)$/\1/p' \
        || true
    fi
  } | sort -u
}

# One preflight line for the pkg-config state. Returns non-zero on a failure.
#
# Arguments: [bootstrap .pc file]
#
# CMake reads the bootstrap through pkg-config, so the file alone says nothing:
# on Fedora it lies outside the pkg-config search path, and the link that fixes
# that is only made when pkg-config was there at install time.
report_pkgconfig_check() {
  local pc_file="${1:-/usr/local/lib/pkgconfig/onnxruntime.pc}"

  if [[ ! -f "${pc_file}" ]]; then
    log "  FAIL  ${pc_file} (missing)"
    return 1
  fi

  if ! command -v pkg-config >/dev/null 2>&1; then
    log "  FAIL  pkg-config (not installed, so nothing reads ${pc_file})"
    return 1
  fi

  if ! pkg-config --exists onnxruntime; then
    log "  FAIL  pkg-config does not find onnxruntime, though ${pc_file} is"
    log "        there (the link into a directory pkg-config searches is gone)"
    return 1
  fi

  log "  PASS  pkg-config finds onnxruntime $(pkg-config --modversion onnxruntime 2>/dev/null)"
  return 0
}

# Print a pass/fail line per check. Returns non-zero when any check failed.
report_cuda_preflight() {
  local failures=0

  log "CUDA preflight (CUDA major ${CUDA_MAJOR}, arch ${ORT_ARCH}):"

  if cuda_major_supported; then
    log "  PASS  CUDA major ${CUDA_MAJOR}"
  else
    log "  FAIL  CUDA major ${CUDA_MAJOR} (the gpu flavor needs 12 or 13; pass"
    log "        --cuda-major 12|13 to install for another toolkit)"
    failures=$((failures + 1))
  fi

  if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi >/dev/null 2>&1; then
    local driver
    driver="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -n 1)"
    log "  PASS  nvidia-smi (driver ${driver:-unknown})"
  else
    log "  FAIL  nvidia-smi (no working NVIDIA driver)"
    failures=$((failures + 1))
  fi

  local soname location
  while read -r soname; do
    if location="$(lib_location "${soname}")"; then
      log "  PASS  ${soname} -> ${location}"
    else
      log "  FAIL  ${soname} (not found by ldconfig or under /usr/local/cuda*/targets/*/lib)"
      failures=$((failures + 1))
    fi
  done < <(cuda_required_libs)

  local ort_root
  ort_root="$(installed_onnxruntime_root)"
  if [[ -n "${ort_root}" ]]; then
    log "  PASS  ONNX Runtime bootstrap -> ${ort_root}"
  else
    log "  FAIL  ONNX Runtime bootstrap (nothing under /opt/studiocast/onnxruntime)"
    failures=$((failures + 1))
  fi

  report_pkgconfig_check || failures=$((failures + 1))

  if [[ "${failures}" -eq 0 ]]; then
    log "CUDA preflight: PASS"
    return 0
  fi

  log "CUDA preflight: FAIL (${failures} check(s) failed)"
  return 1
}

print_cuda_repo_hint() {
  cat >&2 <<EOF
[setup] ERROR: the CUDA ${CUDA_MAJOR} runtime libraries are missing and the NVIDIA
[setup] repository is not enabled. This script does not add third-party
[setup] repositories for you. Enable it, then run the setup again:
[setup]
[setup]   sudo dnf config-manager addrepo --from-repofile=https://developer.download.nvidia.com/compute/cuda/repos/fedora44/x86_64/cuda-fedora44.repo
[setup]   ./scripts/setup.sh --deps --onnxruntime-flavor gpu -y
[setup]
[setup] The setup then installs these packages:
[setup]   cuda-cudart-${CUDA_MAJOR}-N libcublas-${CUDA_MAJOR}-N libcufft-${CUDA_MAJOR}-N
[setup]   libcurand-${CUDA_MAJOR}-N cuda-nvrtc-${CUDA_MAJOR}-N libnvjitlink-${CUDA_MAJOR}-N
[setup]
[setup] libcuda.so.1 comes from the NVIDIA driver package. This script never
[setup] installs a driver.
EOF
}

# Newest <major>-<minor> package suffix the enabled repositories offer.
#
# A system without dnf offers none. Report that as an empty suffix, so that the
# caller prints the repository hint instead of ending the script on a command
# that is not there.
#
# A dnf that is there but fails offers none either: repository metadata that
# does not download, no network, a locked rpmdb. Report that as an empty suffix
# too, so that it reaches the same hint. rpmfusion_free_enabled below degrades
# the same way.
#
# dnf writes its progress and its errors to stderr. Keep the output of a good
# query, which the pattern below reads, and show what dnf said only when the
# query fails, where it tells the user why there is no suffix.
cuda_package_suffix() {
  command -v dnf >/dev/null 2>&1 || return 0

  local output status=0
  output="$(dnf repoquery --qf '%{name}\n' \
    "cuda-cudart-${CUDA_MAJOR}-*" 2>&1)" || status=$?

  if [[ "${status}" -ne 0 ]]; then
    printf '%s\n' "${output}" >&2
    return 0
  fi

  printf '%s\n' "${output}" \
    | sed -n "s/^cuda-cudart-\(${CUDA_MAJOR}-[0-9]\+\)$/\1/p" \
    | sort -t- -k2 -V \
    | tail -n 1
}

ensure_cuda_runtime() {
  # libcuda.so.1 belongs to the driver, so it is not part of this check.
  local -a missing=()
  local soname
  while read -r soname; do
    case "${soname}" in
      libcuda.so.1|libcudnn.so.9) continue ;;
    esac
    lib_resolves "${soname}" || missing+=("${soname}")
  done < <(cuda_required_libs)

  if [[ "${#missing[@]}" -eq 0 ]]; then
    log "The CUDA ${CUDA_MAJOR} runtime libraries already resolve; skipping the rpm install."
    return 0
  fi

  log "Missing CUDA runtime libraries: ${missing[*]}"

  # Ask for the packages, not for a repository id: any enabled repository that
  # offers them will do, and one that offers none cannot help whatever it is
  # called.
  local suffix
  suffix="$(cuda_package_suffix)"
  if [[ -z "${suffix}" ]]; then
    print_cuda_repo_hint
    exit 2
  fi

  log "Installing the CUDA ${suffix//-/.} runtime rpms..."
  dnf_install \
    "cuda-cudart-${suffix}" \
    "libcublas-${suffix}" \
    "libcufft-${suffix}" \
    "libcurand-${suffix}" \
    "cuda-nvrtc-${suffix}" \
    "libnvjitlink-${suffix}"

  # The rpms install into /usr/local/cuda-<ver>/targets/<arch>/lib and normally
  # ship their own ld.so.conf.d entry. Add one when they do not.
  local still_missing=0
  for soname in "${missing[@]}"; do
    lib_resolves "${soname}" || still_missing=1
  done

  if [[ "${still_missing}" -eq 1 ]]; then
    local dirs
    dirs="$(cuda_search_dirs)"
    if [[ -n "${dirs}" ]]; then
      log "Adding /etc/ld.so.conf.d/studiocast-cuda.conf for the CUDA library directories."
      printf '%s\n' "${dirs}" | run_priv tee /etc/ld.so.conf.d/studiocast-cuda.conf >/dev/null
      run_priv ldconfig
    fi
  fi
}

# ---------------------------------------------------------------------------
# v4l2loopback
# ---------------------------------------------------------------------------

have_module() {
  # Does the module exist for this running kernel?
  modinfo v4l2loopback >/dev/null 2>&1
}

rpmfusion_free_enabled() {
  command -v dnf >/dev/null 2>&1 || return 1
  dnf repolist --enabled 2>/dev/null | grep -qi 'rpmfusion-free'
}

print_rpmfusion_hint() {
  cat >&2 <<'EOF'
[setup] ERROR: v4l2loopback is not available on this system.
[setup] Fedora does not ship v4l2loopback. RPM Fusion Free ships it as
[setup] akmod-v4l2loopback. This script does not add third-party repositories
[setup] for you. Enable RPM Fusion Free, then run the setup again:
[setup]
[setup]   sudo dnf install https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm
[setup]   ./scripts/setup.sh --v4l2loopback --load-loopback --persist-loopback
[setup]
EOF
}

ensure_v4l2loopback_available() {
  log "Ensuring v4l2loopback availability..."

  if have_module; then
    log "v4l2loopback module is available for this kernel."
    dnf_install v4l-utils || true
    return 0
  fi

  if ! rpmfusion_free_enabled; then
    print_rpmfusion_hint
    exit 2
  fi

  log "RPM Fusion Free is enabled; installing akmod-v4l2loopback..."
  # akmods needs the headers of the running kernel. Older kernels can be gone
  # from the repositories, so fall back to the newest kernel-devel package.
  if ! dnf_install akmod-v4l2loopback "kernel-devel-$(uname -r)" v4l-utils; then
    log "kernel-devel-$(uname -r) is not available; using the newest kernel-devel."
    dnf_install akmod-v4l2loopback kernel-devel v4l-utils
  fi

  # akmods builds the kernel module for the running kernel. The package trigger
  # normally does this, but it is skipped in some images, so run it directly.
  if command -v akmods >/dev/null 2>&1; then
    log "Building the module with akmods (this can take a few minutes)..."
    run_priv akmods --kernels "$(uname -r)" || true
    run_priv depmod -a || true
  fi

  if have_module; then
    log "v4l2loopback is now available (akmod)."
    return 0
  fi

  echo "[setup] ERROR: v4l2loopback still not available after the akmod install." >&2
  echo "[setup] Check the build log under /var/cache/akmods/v4l2loopback/ and that" >&2
  echo "[setup] kernel-devel matches the running kernel: $(uname -r)" >&2
  exit 1
}

load_v4l2loopback_now() {
  require_cmd modprobe
  log "Loading v4l2loopback now (video_nr=${VIDEO_NR}, label=${LABEL}, exclusive_caps=${EXCLUSIVE_CAPS})..."
  run_priv modprobe -r v4l2loopback 2>/dev/null || true
  run_priv modprobe v4l2loopback "video_nr=${VIDEO_NR}" "card_label=${LABEL}" "exclusive_caps=${EXCLUSIVE_CAPS}"
  log "Loaded. Devices:"
  if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl --list-devices || true
  fi
  ls -l "/dev/video${VIDEO_NR}" 2>/dev/null || true
}

persist_v4l2loopback() {
  log "Persisting v4l2loopback across reboot..."
  echo "v4l2loopback" | run_priv tee /etc/modules-load.d/v4l2loopback.conf >/dev/null

  cat <<EOF | run_priv tee /etc/modprobe.d/studiocast-v4l2loopback.conf >/dev/null
# StudioCast v4l2loopback options
options v4l2loopback video_nr=${VIDEO_NR} card_label="${LABEL}" exclusive_caps=${EXCLUSIVE_CAPS}
EOF

  log "Wrote:"
  log "  /etc/modules-load.d/v4l2loopback.conf"
  log "  /etc/modprobe.d/studiocast-v4l2loopback.conf"
  log "You can verify after reboot with:"
  log "  modinfo v4l2loopback | head"
  log "  ls -l /dev/video${VIDEO_NR}"
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

# Everything above is definitions and plain defaults. Stop here when the file
# is sourced, so that tests/fedora_setup_tests.sh can call single functions
# without running a setup and without a change to the shell of the caller.
if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
  return 0
fi

set -euo pipefail

REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Tests can point this at a fake os-release file. Everything else uses the real one.
OS_RELEASE_FILE="${STUDIOCAST_OS_RELEASE:-/etc/os-release}"

YES=0
DO_DEPS=0
DO_V4L2=0
DO_LOAD_LOOP=0
DO_PERSIST_LOOP=0
DO_CHECK_CUDA=0
DO_BUILD=0
BUILD_DIR="./cmake-build-debug"
BUILD_TYPE="Debug"
DO_MAXINE=0
DO_RPM=0
PASSTHRU_ARGS=()
PARSE_PASSTHRU_ARGS=0

# Did the user pass --cuda-major? Only that value is an option to check.
CUDA_MAJOR_EXPLICIT=0

if [[ -z "${ORT_ARCH:-}" ]]; then
  case "$(uname -m)" in
    x86_64|amd64) ORT_ARCH="x64" ;;
    aarch64|arm64) ORT_ARCH="aarch64" ;;
    *) ORT_ARCH="x64" ;;
  esac
fi

if [[ -z "${ORT_FLAVOR:-}" ]]; then
  if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi >/dev/null 2>&1; then
    ORT_FLAVOR="gpu"
  else
    ORT_FLAVOR="cpu"
  fi
fi

CUDA_MAJOR="${CUDA_MAJOR:-$(detect_cuda_major)}"

# The Ubuntu helper reads the sudo password from stdin for the GUI wizard. This
# helper does not: the GUI wizard is Ubuntu-only, and every privileged write
# here gives the file content to "tee" on stdin, which is where "sudo -S" reads
# the password from. Use this helper from a terminal, or from a root shell.

while [[ $# -gt 0 ]]; do
  if [[ "$PARSE_PASSTHRU_ARGS" -eq 1 ]]; then
    PASSTHRU_ARGS+=("$1"); shift; continue
  fi

  case "$1" in
    --deps) DO_DEPS=1; shift ;;
    --v4l2loopback) DO_V4L2=1; shift ;;
    --load-loopback) DO_LOAD_LOOP=1; shift ;;
    --persist-loopback) DO_PERSIST_LOOP=1; shift ;;
    --video-nr) need_value "$@"; VIDEO_NR="$2"; shift 2 ;;
    --label) need_value "$@"; LABEL="$2"; shift 2 ;;
    --exclusive-caps) need_value "$@"; EXCLUSIVE_CAPS="$2"; shift 2 ;;
    --onnxruntime-version) need_value "$@"; ORT_VERSION="$2"; shift 2 ;;
    --onnxruntime-flavor) need_value "$@"; ORT_FLAVOR="$2"; shift 2 ;;
    --onnxruntime-arch) need_value "$@"; ORT_ARCH="$2"; shift 2 ;;
    --cuda-major)
      need_value "$@"; CUDA_MAJOR="$2"; CUDA_MAJOR_EXPLICIT=1
      shift 2 ;;
    --cudnn-version) need_value "$@"; CUDNN_VERSION="$2"; shift 2 ;;
    --check-cuda) DO_CHECK_CUDA=1; shift ;;
    --build) DO_BUILD=1; shift ;;
    --build-dir) need_value "$@"; BUILD_DIR="$2"; shift 2 ;;
    --build-type) need_value "$@"; BUILD_TYPE="$2"; shift 2 ;;
    --rpm) DO_RPM=1; shift ;;
    --maxine) DO_MAXINE=1; shift ;;
    -y|--yes) YES=1; shift ;;
    --) PARSE_PASSTHRU_ARGS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1"; usage; exit 2 ;;
  esac
done

if [[ "${ORT_FLAVOR}" != "cpu" && "${ORT_FLAVOR}" != "gpu" ]]; then
  echo "[setup] ERROR: --onnxruntime-flavor must be one of: cpu|gpu (got: '${ORT_FLAVOR}')" >&2
  exit 2
fi

# Only a value the user passed can be a bad option value. A value that comes
# from the toolkit on the machine is a fact about the machine: the cpu flavor
# never reads it, --check-cuda reports it, and only the gpu flavor stops for it.
if [[ "${CUDA_MAJOR_EXPLICIT}" -eq 1 ]] && ! cuda_major_supported; then
  echo "[setup] ERROR: --cuda-major must be one of: 12|13 (got: '${CUDA_MAJOR}')" >&2
  exit 2
fi

if [[ "$DO_RPM" -eq 1 && "$DO_MAXINE" -eq 1 ]]; then
  echo "[setup] ERROR: --rpm and --maxine both take the arguments after --." >&2
  echo "[setup] Run them one at a time." >&2
  exit 2
fi

if [[ -f "$OS_RELEASE_FILE" ]]; then
  # shellcheck disable=SC1090
  source "$OS_RELEASE_FILE"
  if [[ "${ID:-}" == "fedora" ]]; then
    log "Detected Fedora ${VERSION_ID:-unknown}."
  elif [[ "${ID_LIKE:-}" == *fedora* ]]; then
    log "Warning: tuned for Fedora 44. Detected ID=${ID:-unknown} (ID_LIKE=${ID_LIKE})."
  else
    echo "[setup] ERROR: This helper is for Fedora. Detected ID=${ID:-unknown}." >&2
    echo "[setup] Run ./scripts/setup.sh to pick the right helper for this distro." >&2
    exit 2
  fi
fi

if [[ "$YES" -eq 1 ]]; then
  DNF_ARGS+=("-y")
fi

# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------

if [[ "$DO_CHECK_CUDA" -eq 1 ]]; then
  report_cuda_preflight || exit 1
  exit 0
fi

if [[ "$DO_DEPS" -eq 1 ]]; then
  log "ONNX Runtime flavor: ${ORT_FLAVOR}"

  # Fail before any install when the gpu flavor cannot work on this system.
  if [[ "${ORT_FLAVOR}" == "gpu" ]]; then
    if ! cuda_major_supported; then
      echo "[setup] ERROR: the CUDA toolkit on this machine is major ${CUDA_MAJOR}," >&2
      echo "[setup] and the gpu flavor needs 12 or 13. Pass --cuda-major 12|13 to" >&2
      echo "[setup] choose the tarball, or use --onnxruntime-flavor cpu." >&2
      exit 2
    fi
    ensure_cuda_runtime
  fi

  log "Installing build/runtime dependencies..."
  DEPS_PACKAGES=(
    cmake ninja-build gcc-c++ pkgconf-pkg-config
    git curl tar xz
    qt6-qtbase-devel
    pulseaudio-libs-devel pulseaudio-utils
    libjpeg-turbo-devel libpng-devel
    sqlite-devel
    libyuv-devel
    clang clang-tools-extra
    v4l-utils
  )

  # The distro package ships a CMake config file that CMake finds before any
  # hand-installed build, so it must not be installed next to the GPU tarball.
  if [[ "${ORT_FLAVOR}" == "cpu" ]]; then
    DEPS_PACKAGES+=(onnxruntime-devel)
  fi

  dnf_install "${DEPS_PACKAGES[@]}"

  if [[ "${ORT_FLAVOR}" == "gpu" ]]; then
    warn_if_distro_onnxruntime_installed
    ensure_cudnn_available
    ensure_onnxruntime_gpu_available
    report_cuda_preflight || true
  else
    log "ONNX Runtime comes from onnxruntime-devel; CMake finds it through its CMake config."
    log "That package has the CPU execution provider only. Open CUDA GPU inference"
    log "needs the gpu flavor: ./scripts/setup.sh --deps --onnxruntime-flavor gpu"
  fi

  log "Fedora has no dlib package, so CMake disables the Open Video Eye Contact effect."
fi

if [[ "$DO_V4L2" -eq 1 ]]; then
  ensure_v4l2loopback_available
fi

if [[ "$DO_LOAD_LOOP" -eq 1 ]]; then
  if ! have_module; then
    ensure_v4l2loopback_available
  fi
  load_v4l2loopback_now
fi

if [[ "$DO_PERSIST_LOOP" -eq 1 ]]; then
  if ! have_module; then
    ensure_v4l2loopback_available
  fi
  persist_v4l2loopback
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
  log "Configuring + building into: $BUILD_DIR (type: $BUILD_TYPE)"
  CMAKE_ARGS=(
    -S . -B "$BUILD_DIR" -G Ninja
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON
    -DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON
  )

  # Without this, the distro CMake config file for onnxruntime-devel would win
  # and the build would silently use the CPU-only ONNX Runtime.
  REQUESTED_ORT_ROOT="$(requested_onnxruntime_root)"
  ORT_BOOTSTRAP_ROOT="$(installed_onnxruntime_root)"
  if [[ -n "${ORT_BOOTSTRAP_ROOT}" ]]; then
    if [[ -n "${REQUESTED_ORT_ROOT}" &&
      "${REQUESTED_ORT_ROOT}" != "${ORT_BOOTSTRAP_ROOT}" ]]; then
      warn "This run asks for ${REQUESTED_ORT_ROOT}, which is not installed."
      warn "Building against the newest installed bootstrap instead."
    fi
    log "Using the ONNX Runtime bootstrap at ${ORT_BOOTSTRAP_ROOT}"
    CMAKE_ARGS+=(-DONNXRUNTIME_ROOT="${ORT_BOOTSTRAP_ROOT}")
  fi

  cmake "${CMAKE_ARGS[@]}"
  cmake --build "$BUILD_DIR"
  log "Built. Useful commands:"
  echo "  $BUILD_DIR/studiocast --version"
  echo "  $BUILD_DIR/studiocastd"
  echo "  $BUILD_DIR/studiocastctl status"
  echo "  $BUILD_DIR/studiocast-maxine install-hints"
fi

if [[ "$DO_MAXINE" -eq 1 ]]; then
  log "Running Maxine setup helper..."
  "${REPO_ROOT}/scripts/setup/maxine.sh" --build-dir "$BUILD_DIR" "${PASSTHRU_ARGS[@]}"
fi

if [[ "$DO_RPM" -eq 1 ]]; then
  log "Running the RPM build helper..."
  exec "${REPO_ROOT}/packaging/rpm/build_rpm.sh" "${PASSTHRU_ARGS[@]}"
fi

log "Done."
