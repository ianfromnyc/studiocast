#!/usr/bin/env bash
set -euo pipefail

# StudioCast Maxine setup helper.
#
# This script does NOT redistribute NVIDIA Maxine SDK assets.
# The SDK comes from NVIDIA and you must comply with NVIDIA's licensing terms.
#
# What this script CAN do:
# - Create the expected StudioCast Maxine directory layout under XDG_DATA_HOME
# - Download the core SDK archives from NGC with your NGC API key (--download)
# - Extract user-provided Maxine SDK tarballs into the correct locations
# - Install Maxine "features" (models and feature libraries). It prefers the
#   SDK's own install_feature.sh / download_features.sh scripts and falls back
#   to the NGC REST API when the core SDK is not there (--download-features).
#
# One key drives every step. Export NGC_API_KEY (NGC_CLI_API_KEY also works);
# this script gives both names to the SDK scripts that it calls.
#
# Everything runs as the normal user. No step needs sudo, because the whole
# layout lives under your own XDG directories.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

log() {
  echo "[maxine] $*"
}

err() {
  echo "[maxine] ERROR: $*" >&2
}

# scripts/_lib/ngc.sh keeps these names, so its messages get the same prefix.
sc_ngc_log() {
  log "$@"
}

sc_ngc_err() {
  err "$@"
}

# shellcheck source=scripts/_lib/ngc.sh
source "${REPO_ROOT}/scripts/_lib/ngc.sh"

# Component tables. The key is the short name used on the command line.
declare -A COMPONENT_LABEL=(
  [vfx]="VFX"
  [ar]="AR"
  [afx]="AFX"
)

# Directory each component must end up in, under the Maxine base directory.
declare -A COMPONENT_ROOT_NAME=(
  [vfx]="VideoFX"
  [ar]="ARSDK"
  [afx]="Audio_Effects_SDK"
)

# NGC resource holding the core SDK archive. Override with --<comp>-resource.
# These three are the NVIDIA Developer Program packaging, which every NGC
# account can read.
declare -A COMPONENT_RESOURCE=(
  [vfx]="vfx_sdk_core"
  [ar]="ar_sdk_core"
  [afx]="maxine_linux_audio_effects_sdk"
)

# Other resource names for the same component. They are printed when NGC says
# the account has no entitlement, because a different tier may be available.
# The maxine_linux_*_ga names are the NVIDIA AI Enterprise packaging of the
# same SDKs, and they need that subscription.
declare -A COMPONENT_ALT_RESOURCES=(
  [vfx]="vfx_sdk_core maxine_linux_vfx_sdk_ga maxine_linux_vfx_sdk_ea maxine_linux_vfx_sdk"
  [ar]="ar_sdk_core maxine_linux_ar_sdk_ga maxine_linux_ar_sdk_ea maxine_linux_ar_sdk"
  [afx]="maxine_linux_audio_effects_sdk"
)

# NGC models holding the VFX/AR feature packs. Each one has a "<version>_lib_linux"
# version with the feature library and a "<version>_models_linux_sm<CC>" version
# with the engine files.
#
# These are the features that NVIDIA builds for Linux in SDK 1.x. The word "all"
# is allowed as well; it lets the SDK script ask NGC for the whole list, which
# also names Windows only features such as nvarlipsync.
declare -A COMPONENT_FEATURE_MODELS=(
  [vfx]="nvvfxaigsrelighting nvvfxbackgroundblur nvvfxdenoising nvvfxgreenscreen nvvfxrelighting nvvfxtransfer nvvfxupscale nvvfxvideosuperres"
  [ar]="nvaractivespeakerdetection nvarbodydetection nvarbodyposeestimation nvarfaceboxdetection nvarfaceexpressions nvargazeredirection nvarlandmarkdetection"
)

# The same list, kept as it is, for the REST fallback when the user asks for
# "all" and there is no SDK script to do the discovery.
declare -A COMPONENT_DEFAULT_FEATURES=(
  [vfx]="${COMPONENT_FEATURE_MODELS[vfx]}"
  [ar]="${COMPONENT_FEATURE_MODELS[ar]}"
)

# Set when the user names the features, so a failure of one of them is fatal.
declare -A COMPONENT_FEATURES_EXPLICIT=()

# Version pins, filled by --sdk-version / --<comp>-version.
declare -A COMPONENT_VERSION=()
# Feature pack version pins, filled by --feature-version.
declare -A COMPONENT_FEATURE_VERSION=()

usage() {
  cat <<'EOF'
Usage:
  scripts/setup/maxine.sh [options]

Options:
  --base DIR            Base directory (default: $XDG_DATA_HOME/studiocast/maxine or ~/.local/share/studiocast/maxine)
  --cache-dir DIR       Download cache (default: $XDG_CACHE_HOME/studiocast/maxine or ~/.cache/studiocast/maxine)

  --download LIST       Download and extract core SDKs from NGC. LIST is afx, vfx, ar or all,
                        as a comma separated list. The option can be repeated.
                        Needs NGC_API_KEY (or NGC_CLI_API_KEY).
  --list-versions COMP  Print the NGC versions of one component (afx|vfx|ar) and exit.
  --sdk-version C=V     Pin one component to version V, for example --sdk-version afx=2.1.0.
  --afx-version V       Same as --sdk-version afx=V.
  --vfx-version V       Same as --sdk-version vfx=V.
  --ar-version V        Same as --sdk-version ar=V.
  --afx-resource NAME   Override the NGC resource name for AFX (default: maxine_linux_audio_effects_sdk).
  --vfx-resource NAME   Override the NGC resource name for VFX (default: vfx_sdk_core).
  --ar-resource NAME    Override the NGC resource name for AR  (default: ar_sdk_core).
  --platform NAME       Platform of the SDK version to take: linux or windows (default: linux).

  --vfx-tar PATH        Path to a local VFXSDK_linux_<version>.tgz
  --ar-tar PATH         Path to a local ARSDK_linux_<version>.tgz
  --afx-tar PATH        Path to a local NVIDIA_AFX_SDK_Linux_<version>.tar.gz
  --extract             Extract provided tarballs (default if any tarball arg is given)

  --download-features LIST
                        Install the feature packs for afx, vfx, ar or all. It runs the
                        SDK's own feature script when the core SDK is extracted, and
                        otherwise downloads the same packs from NGC over REST.
  --install-features    Install the VFX/AR feature packs (needs NGC_API_KEY). Same as
                        --download-features vfx,ar, but both SDK roots must exist.
  --install-afx-features  Download AFX features needed for the MVP (needs NGC_API_KEY)
  --afx-effects CSV     AFX effect list (default: the four the microphone effects use,
                        denoiser-48k,dereverb-48k,dereverb_denoiser-48k,studio_voice-48k).
                        AEC and Superres are optional; name them here to add them.
  --afx-gpu NAME        GPU name for download_features.sh -g (for example a40, t4, l4).
                        Without it the AFX script detects the GPU itself.
  --vfx-features CSV    VFX features to install, or "all" (default: the Linux features
                        of SDK 1.x). A feature you name here must install, or the run fails.
  --ar-features CSV     AR features to install, or "all" (default: the Linux features
                        of SDK 1.x, which leaves out the Windows only nvarlipsync).
  --feature-version C=V Pin the feature pack version of one component,
                        for example --feature-version ar=1.1.1.0.
  --sm NN               GPU compute capability for the REST feature download,
                        for example 86. Default: detected from the local GPU.
  --gpu ARG             GPU name for install_feature.sh -g. Can be repeated. A name that
                        the SDK script does not know is dropped, and the script then reads
                        the local GPU itself.
  --build-dir DIR       Build dir containing studiocast-maxine (default: ./cmake-build-debug). Used to auto-detect --gpu args.
  --ngc-org ORG         NGC org (default: nvidia)
  --ngc-team TEAM       NGC team (default: maxine)
  --dry-run             Ask NGC what it would fetch, then print the download and
                        extract steps without running them.
  -h, --help            Show help.

NGC entitlements:
  The VFX, AR and AFX cores and their feature packs are all available to an
  NVIDIA Developer Program account, under the default resource names
  vfx_sdk_core, ar_sdk_core and maxine_linux_audio_effects_sdk.
  The maxine_linux_vfx_sdk_ga and maxine_linux_ar_sdk_ga resources are the
  NVIDIA AI Enterprise packaging of the same SDKs, and they need that
  subscription. The *_ea names are Maxine Early Access, by request.
  Use --vfx-resource / --ar-resource to pick another one.
  Catalog page: https://catalog.ngc.nvidia.com/orgs/nvidia/teams/maxine/resources/<name>

  A core SDK version holds more than one archive. Only the core is extracted;
  the Triton Inference Server build stays in the cache.

Examples:
  # Full install with one key. This is the normal path:
  export NGC_API_KEY="..."
  ./scripts/setup/maxine.sh --download all --install-features --install-afx-features

  # Audio only, pinned to one SDK version:
  export NGC_API_KEY="..."
  ./scripts/setup/maxine.sh --download afx --sdk-version afx=2.1.0 --install-afx-features

  # See what is available before downloading:
  export NGC_API_KEY="..."
  ./scripts/setup/maxine.sh --list-versions afx

  # Offline fallback: extract archives you already downloaded:
  ./scripts/setup/maxine.sh --vfx-tar ~/Downloads/VFXSDK_linux_1.2.0.0.tgz \
                            --ar-tar  ~/Downloads/ARSDK_linux_1.1.1.0.tgz

  # Install features with explicit GPU arg(s):
  export NGC_API_KEY="..."
  ./scripts/setup/maxine.sh --install-features --gpu turing

  # Fetch one AR feature pack without the core SDK:
  export NGC_API_KEY="..."
  ./scripts/setup/maxine.sh --download-features ar --ar-features nvarlandmarkdetection
EOF
}

default_base() {
  local xdg="${XDG_DATA_HOME:-$HOME/.local/share}"
  echo "${xdg}/studiocast/maxine"
}

default_cache_dir() {
  local xdg="${XDG_CACHE_HOME:-$HOME/.cache}"
  echo "${xdg}/studiocast/maxine"
}

BASE="$(default_base)"
CACHE_DIR="$(default_cache_dir)"
VFX_TAR=""
AR_TAR=""
AFX_TAR=""
DO_EXTRACT=0
DO_INSTALL_FEATURES=0
DO_INSTALL_AFX_FEATURES=0
AFX_EFFECTS_CSV=""
AFX_GPU=""
SM_OVERRIDE=""
declare -a GPU_ARGS=()
declare -a DOWNLOAD_COMPONENTS=()
declare -a FEATURE_COMPONENTS=()
LIST_VERSIONS_COMPONENT=""
BUILD_DIR="./cmake-build-debug"
PLATFORM="linux"
NGC_ORG="nvidia"
NGC_TEAM="maxine"
DRY_RUN=0

# The AFX features that the StudioCast microphone effects select, at the rate
# the audio pipeline runs. PlanBroadcastMicrophoneEffect in
# src/core/maxine/afx/afx_effect.cpp picks one of these four, and `doctor`
# reports the same four. A default that installed anything else would leave
# every broadcast microphone effect missing.
#
# AEC and Superres are not in this list, because nothing in StudioCast selects
# them yet. Add them with --afx-effects when you want them:
#   --afx-effects "denoiser-48k,dereverb-48k,dereverb_denoiser-48k,studio_voice-48k,aec-48k,superres-16k_to_48k"
AFX_EFFECTS_DEFAULT="denoiser-48k,dereverb-48k,dereverb_denoiser-48k,studio_voice-48k"

# Check one component short name.
require_component() {
  local comp="$1"
  local option="$2"
  if [[ -z "${COMPONENT_LABEL[$comp]:-}" ]]; then
    err "${option} takes one of: afx, vfx, ar (got: '${comp}')"
    exit 2
  fi
}

# Add every component named in a comma separated list to one array.
# Arguments: <array name> <option name> <list>
add_components() {
  local -n target="$1"
  local option="$2"
  local list="$3"
  local comp existing
  local -a wanted=()

  IFS=',' read -r -a wanted <<< "${list}"
  for comp in "${wanted[@]}"; do
    comp="${comp//[[:space:]]/}"
    [[ -n "${comp}" ]] || continue
    if [[ "${comp}" == "all" ]]; then
      add_components "$1" "${option}" "vfx,ar,afx"
      continue
    fi
    require_component "${comp}" "${option}"
    for existing in ${target[@]+"${target[@]}"}; do
      [[ "${existing}" != "${comp}" ]] || continue 2
    done
    target+=("${comp}")
  done
}

# Pin one version from a "component=version" argument.
# Arguments: <array name> <option name> <pair>
set_component_version() {
  local -n pins="$1"
  local option="$2"
  local pair="$3"
  local comp="${pair%%=*}"
  local version="${pair#*=}"

  if [[ "${pair}" != *=* || -z "${comp}" || -z "${version}" ]]; then
    err "${option} takes COMPONENT=VERSION (got: '${pair}')"
    exit 2
  fi
  require_component "${comp}" "${option}"
  # shellcheck disable=SC2034  # pins is a nameref to the caller's array.
  pins["${comp}"]="${version}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base) BASE="${2:-}"; shift 2 ;;
    --cache-dir) CACHE_DIR="${2:-}"; shift 2 ;;
    --download) add_components DOWNLOAD_COMPONENTS --download "${2:-}"; shift 2 ;;
    --download-features) add_components FEATURE_COMPONENTS --download-features "${2:-}"; shift 2 ;;
    --list-versions) LIST_VERSIONS_COMPONENT="${2:-}"; shift 2 ;;
    --sdk-version) set_component_version COMPONENT_VERSION --sdk-version "${2:-}"; shift 2 ;;
    --afx-version) set_component_version COMPONENT_VERSION --sdk-version "afx=${2:-}"; shift 2 ;;
    --vfx-version) set_component_version COMPONENT_VERSION --sdk-version "vfx=${2:-}"; shift 2 ;;
    --ar-version) set_component_version COMPONENT_VERSION --sdk-version "ar=${2:-}"; shift 2 ;;
    --feature-version) set_component_version COMPONENT_FEATURE_VERSION --feature-version "${2:-}"; shift 2 ;;
    --afx-resource) COMPONENT_RESOURCE[afx]="${2:-}"; shift 2 ;;
    --vfx-resource) COMPONENT_RESOURCE[vfx]="${2:-}"; shift 2 ;;
    --ar-resource) COMPONENT_RESOURCE[ar]="${2:-}"; shift 2 ;;
    --vfx-features)
      COMPONENT_FEATURE_MODELS[vfx]="${2//,/ }"
      COMPONENT_FEATURES_EXPLICIT[vfx]=1
      shift 2
      ;;
    --ar-features)
      COMPONENT_FEATURE_MODELS[ar]="${2//,/ }"
      COMPONENT_FEATURES_EXPLICIT[ar]=1
      shift 2
      ;;
    --vfx-tar) VFX_TAR="${2:-}"; DO_EXTRACT=1; shift 2 ;;
    --ar-tar) AR_TAR="${2:-}"; DO_EXTRACT=1; shift 2 ;;
    --afx-tar) AFX_TAR="${2:-}"; DO_EXTRACT=1; shift 2 ;;
    --extract) DO_EXTRACT=1; shift ;;
    --install-features) DO_INSTALL_FEATURES=1; shift ;;
    --install-afx-features) DO_INSTALL_AFX_FEATURES=1; shift ;;
    --afx-effects) AFX_EFFECTS_CSV="${2:-}"; shift 2 ;;
    --afx-gpu) AFX_GPU="${2:-}"; shift 2 ;;
    --sm) SM_OVERRIDE="${2:-}"; shift 2 ;;
    --gpu) GPU_ARGS+=("${2:-}"); shift 2 ;;
    --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
    --platform) PLATFORM="${2:-}"; shift 2 ;;
    --ngc-org) NGC_ORG="${2:-}"; shift 2 ;;
    --ngc-team) NGC_TEAM="${2:-}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1"; usage; exit 2 ;;
  esac
done

if [[ "${PLATFORM}" != "linux" && "${PLATFORM}" != "windows" ]]; then
  err "--platform takes linux or windows (got: '${PLATFORM}')"
  exit 2
fi

SC_NGC_ORG="${NGC_ORG}"
SC_NGC_TEAM="${NGC_TEAM}"
SC_NGC_DRY_RUN="${DRY_RUN}"

# One key drives every step:
#   install_feature.sh  (VFX/AR) reads NGC_CLI_API_KEY
#   download_features.sh (AFX)   reads NGC_API_KEY
#   scripts/_lib/ngc.sh          reads either one
# Take whichever the user set, prefer NGC_API_KEY, and export both names.
NGC_KEY="${NGC_API_KEY:-${NGC_CLI_API_KEY:-}}"
if [[ -n "${NGC_KEY}" ]]; then
  export NGC_API_KEY="${NGC_KEY}"
  export NGC_CLI_API_KEY="${NGC_KEY}"
fi

require_key() {
  local what="$1"
  if [[ -n "${NGC_KEY}" ]]; then
    return 0
  fi
  err "${what} needs an NGC API key, and none is set."
  err "Export your key (do not commit it):"
  err '  export NGC_API_KEY="..."'
  err "NGC_CLI_API_KEY works too. Get a key at https://ngc.nvidia.com -> Setup -> API key."
  exit 2
}

resource_for() {
  printf '%s' "${COMPONENT_RESOURCE[$1]}"
}

# Fill SC_NGC_ALT_RESOURCES so a 402 answer names the other tiers, without
# repeating the name that just failed.
set_alt_resources_for() {
  local comp="$1"
  local current name
  local -a known=()

  current="$(resource_for "${comp}")"
  read -r -a known <<< "${COMPONENT_ALT_RESOURCES[$comp]}"

  SC_NGC_ALT_RESOURCES=()
  for name in "${known[@]}"; do
    [[ "${name}" != "${current}" ]] || continue
    SC_NGC_ALT_RESOURCES+=("${name}")
  done
}

# Print the platform of one NGC version id, or "-" when it names none.
version_platform() {
  case "$1" in
    *_linux) printf 'linux' ;;
    *_windows) printf 'windows' ;;
    *) printf '-' ;;
  esac
}

if [[ -n "${LIST_VERSIONS_COMPONENT}" ]]; then
  require_component "${LIST_VERSIONS_COMPONENT}" "--list-versions"
  require_key "--list-versions"
  sc_ngc_require_tools || exit 2

  RESOURCE="$(resource_for "${LIST_VERSIONS_COMPONENT}")"
  set_alt_resources_for "${LIST_VERSIONS_COMPONENT}"

  log "Versions of ${RESOURCE} (NGC org ${NGC_ORG}, team ${NGC_TEAM}):"
  printf '  %-20s %-9s %-18s %s\n' "VERSION" "PLATFORM" "STATUS" "SIZE"
  VERSION_LIST="$(sc_ngc_list_versions "${RESOURCE}")" || exit 2
  while IFS=$'\t' read -r version status size; do
    [[ -n "${version}" ]] || continue
    printf '  %-20s %-9s %-18s %s\n' \
      "${version}" "$(version_platform "${version}")" "${status}" \
      "$(sc_ngc_human_bytes "${size:-0}")"
  done <<< "${VERSION_LIST}"
  exit 0
fi

mkdir -p "$BASE"

VFX_ROOT="${BASE}/VideoFX"
AR_ROOT="${BASE}/ARSDK"
AFX_ROOT="${BASE}/Audio_Effects_SDK"

root_for() {
  printf '%s/%s' "${BASE}" "${COMPONENT_ROOT_NAME[$1]}"
}

# Print the top level names inside an archive. Only the first entries are read,
# which is enough to tell the layout and keeps a multi GB archive cheap to scan.
archive_top_names() {
  local archive="$1"
  (
    set +o pipefail
    tar -tf "${archive}" 2>/dev/null | head -n 200 \
      | sed -e 's#^\./##' -e 's#/.*##' | grep -v '^$' | sort -u
  )
}

# True when a directory holds what an SDK root should hold.
looks_like_sdk_root() {
  local dir="$1"
  local comp="$2"

  case "${comp}" in
    afx) [[ -d "${dir}/nvafx" || -d "${dir}/features" ]] ;;
    *) [[ -d "${dir}/lib" || -d "${dir}/lib64" || -d "${dir}/include" \
          || -d "${dir}/features" || -d "${dir}/bin" ]] ;;
  esac
}

# True when a directory name is the SDK root name, or that name with a version
# after it, such as Audio_Effects_SDK_2.1.0.
#
# The name must match exactly otherwise. NVIDIA ships a Triton Inference Server
# build whose directory is ARSDK-triton-server or VideoFX-triton-server, and
# that one holds no SDK.
is_sdk_root_name() {
  local name="$1"
  local root_name="$2"

  [[ "${name}" == "${root_name}" ]] && return 0
  [[ "${name}" =~ ^${root_name}[-_.]?[0-9][0-9._-]*$ ]]
}

# Print the SDK root inside an extracted tree, or nothing when there is none.
find_sdk_root() {
  local staging="$1"
  local root_name="$2"
  local comp="$3"
  local hit entry
  local -a entries=()

  hit="$(find "${staging}" -maxdepth 4 -type d -name "${root_name}" -print -quit 2>/dev/null || true)"
  if [[ -n "${hit}" ]]; then
    printf '%s' "${hit}"
    return 0
  fi

  if looks_like_sdk_root "${staging}" "${comp}"; then
    printf '%s' "${staging}"
    return 0
  fi

  # A single directory whose name is the root name with a version after it.
  mapfile -t entries < <(find "${staging}" -mindepth 1 -maxdepth 1 -print)
  if [[ "${#entries[@]}" -eq 1 && -d "${entries[0]}" ]]; then
    entry="$(basename "${entries[0]}")"
    if is_sdk_root_name "${entry}" "${root_name}" \
       && looks_like_sdk_root "${entries[0]}" "${comp}"; then
      printf '%s' "${entries[0]}"
      return 0
    fi
  fi

  return 1
}

# Extract one core SDK archive so that <base>/<root name> holds the SDK.
#
# Arguments: <component> <archive path>
#
# NVIDIA does not use one layout for every SDK, so the archive is first checked.
# When its only top level directory is already the expected one, it is unpacked
# straight into the base directory. Any other layout is unpacked into a staging
# directory, and the SDK root inside it is moved into place.
extract_archive() {
  local comp="$1"
  local archive="$2"
  local label="${COMPONENT_LABEL[$comp]}"
  local root_name="${COMPONENT_ROOT_NAME[$comp]}"
  local dest="${BASE}/${root_name}"
  local staging src
  local -a tops=()

  if [[ "${DRY_RUN}" -eq 1 ]]; then
    log "Would extract ${label}: ${archive} -> ${dest}"
    return 0
  fi

  if [[ ! -f "${archive}" ]]; then
    err "${label} archive not found: ${archive}"
    return 2
  fi

  # The Triton build sits beside the core in the same NGC version. It holds a
  # model repository for NVIDIA Triton Inference Server, not an SDK.
  case "$(basename "${archive}")" in
    *triton*|*Triton*)
      err "${archive} is the Triton Inference Server build, not the core SDK."
      err "StudioCast does not use it. Give the *SDK_${PLATFORM}_*.tgz archive instead."
      return 2
      ;;
  esac

  log "Extracting ${label}: ${archive}"
  mapfile -t tops < <(archive_top_names "${archive}")

  if [[ "${#tops[@]}" -eq 1 && "${tops[0]}" == "${root_name}" ]]; then
    tar -xf "${archive}" -C "${BASE}"
    if [[ ! -d "${dest}" ]]; then
      err "expected ${label} root at ${dest} after extraction."
      return 2
    fi
    log "${label} root: ${dest}"
    return 0
  fi

  staging="${BASE}/.studiocast-extract.$$"
  rm -rf "${staging}"
  mkdir -p "${staging}"
  tar -xf "${archive}" -C "${staging}"

  if ! src="$(find_sdk_root "${staging}" "${root_name}" "${comp}")" || [[ -z "${src}" ]]; then
    err "could not find the ${label} SDK root inside ${archive}."
    err "Expected a '${root_name}' directory. The archive contains:"
    find "${staging}" -maxdepth 2 -printf '  %P\n' 2>/dev/null | head -n 40 >&2
    err "Extract it by hand so that ${dest} holds the SDK, then re-run with --extract."
    rm -rf "${staging}"
    return 2
  fi

  if [[ -d "${dest}" ]]; then
    log "${dest} exists; merging the new files into it."
    cp -a "${src}/." "${dest}/"
  else
    mv "${src}" "${dest}"
  fi
  rm -rf "${staging}"

  log "${label} root: ${dest}"
}

# Print the version of a core SDK to install.
#
# A pinned version wins. Otherwise the newest finished version for the wanted
# platform wins. NGC reports a "latest version" of its own, but for the SDK core
# resources that is the Windows build, so it cannot be used here.
resolve_sdk_version() {
  local comp="$1"
  local resource="$2"
  local label="${COMPONENT_LABEL[$comp]}"
  local pinned list ids matching version

  pinned="${COMPONENT_VERSION[$comp]:-}"
  if [[ -n "${pinned}" ]]; then
    log "${label}: using the pinned version ${pinned} of ${resource}." >&2
    printf '%s' "${pinned}"
    return 0
  fi

  log "${label}: asking NGC for the newest ${PLATFORM} version of ${resource}..." >&2
  list="$(sc_ngc_list_versions "${resource}")" || return 2

  ids="$(printf '%s\n' "${list}" | awk -F'\t' '$2 == "UPLOAD_COMPLETE" { print $1 }')"
  if [[ -z "${ids}" ]]; then
    err "${label}: NGC has no finished version of ${resource}."
    return 2
  fi

  # Keep the versions of the wanted platform. A resource whose versions name no
  # platform, such as the audio SDK, keeps all of them.
  matching="$(printf '%s\n' "${ids}" | grep -E "_${PLATFORM}\$" || true)"
  if [[ -n "${matching}" ]]; then
    ids="${matching}"
  fi

  version="$(printf '%s\n' "${ids}" | sort -V | tail -n 1)"
  if [[ -z "${version}" ]]; then
    err "${label}: no ${PLATFORM} version of ${resource} found."
    return 2
  fi

  log "${label}: newest ${PLATFORM} version is ${version}." >&2
  printf '%s' "${version}"
}

# Print the core SDK archive out of a "<size><TAB><path>" list.
#
# A version can also hold the Triton Inference Server build of the same SDK.
# That one is never the core, so it is dropped. Of what is left, an archive that
# names the wanted platform wins, and the biggest one wins after that.
select_main_archive() {
  printf '%s' "$1" | awk -F'\t' -v plat="_${PLATFORM}" '
    {
      if ($2 == "") next;
      name = tolower($2);
      sub(/.*\//, "", name);
      if (index(name, "triton") > 0) next;
      score = (index(name, plat) > 0) ? 1 : 0;
      size = $1 + 0;
      if (score > best_score || (score == best_score && size > best_size)) {
        best_score = score;
        best_size = size;
        best = $2;
      }
    }
    END { if (best != "") print best }
  '
}

# Download one core SDK from NGC into the cache, then extract it.
download_component() {
  local comp="$1"
  local label="${COMPONENT_LABEL[$comp]}"
  local resource version listing dir dest path size sha main
  local candidates=""

  resource="$(resource_for "${comp}")"
  set_alt_resources_for "${comp}"

  version="$(resolve_sdk_version "${comp}" "${resource}")" || return 2

  listing="$(sc_ngc_list_files "${resource}" "${version}")" || return 2
  if [[ -z "${listing}" ]]; then
    err "${label}: NGC lists no files for ${resource} version ${version}."
    return 2
  fi

  dir="${CACHE_DIR}/${resource}/${version}"
  while IFS=$'\t' read -r path size sha; do
    [[ -n "${path}" ]] || continue
    dest="$(sc_ngc_safe_dest "${dir}" "${path}")" || return 2
    sc_ngc_download_file "${resource}" "${version}" "${path}" "${dest}" "${sha}" "${size}" || return 2
    case "${path}" in
      *.tar.gz|*.tgz|*.tar|*.tar.xz|*.tar.bz2|*.tar.zst)
        candidates+="${size:-0}"$'\t'"${dest}"$'\n'
        ;;
    esac
  done <<< "${listing}"

  if [[ -z "${candidates}" ]]; then
    err "${label}: ${resource} version ${version} holds no archive to extract."
    err "Files are in ${dir}. Extract them by hand into ${BASE}."
    return 2
  fi

  main="$(select_main_archive "${candidates}")"
  if [[ -z "${main}" ]]; then
    err "${label}: ${resource} version ${version} holds no core SDK archive."
    err "Files are in ${dir}."
    return 2
  fi

  # A version can hold more than one archive. Only the core SDK goes into the
  # base directory; the Triton server variant and the README stay in the cache.
  local other
  while IFS=$'\t' read -r size other; do
    [[ -n "${other}" && "${other}" != "${main}" ]] || continue
    log "${label}: keeping $(basename "${other}") in the cache; it is not the core SDK."
  done <<< "${candidates}"

  extract_archive "${comp}" "${main}" || return 2
}

# Print the CUDA compute capability of the local GPU as NGC writes it (86, 89).
detect_sm() {
  local bin="${AFX_ROOT}/samples/utils/compute_capability/compute_capability"
  local cc=""

  if [[ -n "${SM_OVERRIDE}" ]]; then
    printf '%s' "${SM_OVERRIDE}"
    return 0
  fi

  if [[ -x "${bin}" ]]; then
    cc="$("${bin}" 2>/dev/null || true)"
  fi

  if [[ -z "${cc}" ]] && command -v nvidia-smi >/dev/null 2>&1; then
    cc="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -n 1 || true)"
  fi

  cc="${cc//[^0-9]/}"
  if [[ -z "${cc}" ]]; then
    err "could not read the GPU compute capability."
    err "Pass it yourself, for example --sm 86 for an Ampere GeForce card."
    return 2
  fi

  printf '%s' "${cc}"
}

# Print the highest version of one NGC model that matches a regular expression.
# No match prints nothing and is not an error; only an API failure returns 2.
pick_model_version() {
  local model="$1"
  local pattern="$2"
  local list hit

  list="$(sc_ngc_list_model_versions "${model}")" || return 2
  hit="$(printf '%s\n' "${list}" | cut -f1 | grep -E "${pattern}" | sort -V | tail -n 1 || true)"
  printf '%s' "${hit}"
}

# Turn a version pin into a regular expression part. Without a pin any version
# prefix matches.
feature_version_pattern() {
  local pin="${COMPONENT_FEATURE_VERSION[$1]:-}"
  if [[ -z "${pin}" ]]; then
    printf '.+'
    return 0
  fi
  printf '%s' "${pin//./\\.}"
}

# Unpack every archive of a feature library pack into a directory.
extract_feature_libs() {
  local srcdir="$1"
  local destdir="$2"
  local archive
  local found=0

  shopt -s nullglob
  for archive in "${srcdir}"/*.tar.gz "${srcdir}"/*.tgz "${srcdir}"/*.tar; do
    tar -xf "${archive}" -C "${destdir}"
    found=1
  done
  shopt -u nullglob

  [[ "${found}" -eq 1 ]] || return 1
}

# Download the AFX feature packs over REST, in the layout that the SDK's own
# download_features.sh writes:
#   <AFX root>/features/<effect>/{include,lib}
#   <AFX root>/features/<effect>/models/sm_<CC>/<model>.trtpkg
rest_download_afx_features() {
  local features_dir="${AFX_ROOT}/features"
  local effects sm entry effect rate model dir libver modelver mdir tmp base link
  local pattern

  sm="$(detect_sm)" || return 2
  effects="${AFX_EFFECTS_CSV:-$AFX_EFFECTS_DEFAULT}"
  pattern="$(feature_version_pattern afx)"

  log "AFX: downloading feature packs for sm${sm}: ${effects}"
  mkdir -p "${features_dir}"

  local -a wanted=()
  IFS=',' read -r -a wanted <<< "${effects}"
  for entry in "${wanted[@]}"; do
    entry="${entry//[[:space:]]/}"
    [[ -n "${entry}" ]] || continue

    effect="${entry%-*}"
    rate="${entry##*-}"
    if [[ "${entry}" == *voice_font* ]]; then
      rate=""
    fi

    model="afx_${effect}"
    dir="${features_dir}/${effect}"

    libver="$(pick_model_version "${model}" "^${pattern}-lib$")" || return 2
    if [[ -z "${libver}" ]]; then
      err "AFX: NGC has no feature library version for ${model}."
      return 2
    fi

    if [[ -n "${rate}" ]]; then
      modelver="$(pick_model_version "${model}" "^${pattern}-${rate}-sm${sm}$")" || return 2
    else
      modelver="$(pick_model_version "${model}" "^${pattern}-sm${sm}$")" || return 2
    fi
    if [[ -z "${modelver}" ]]; then
      err "AFX: NGC has no ${entry} models for sm${sm} (model ${model})."
      err "Check --sm, or pick another effect."
      return 2
    fi

    log "AFX ${entry}: library ${libver}, models ${modelver}"
    mkdir -p "${dir}"

    tmp="$(mktemp -d)"
    if ! sc_ngc_download_model_version "${model}" "${libver}" "${tmp}"; then
      rm -rf "${tmp}"
      return 2
    fi
    if [[ "${DRY_RUN}" -ne 1 ]] && ! extract_feature_libs "${tmp}" "${dir}"; then
      err "AFX: the feature library pack of ${model} holds no archive."
      rm -rf "${tmp}"
      return 2
    fi
    rm -rf "${tmp}"

    mdir="${dir}/models/sm_${sm}"
    mkdir -p "${mdir}"
    sc_ngc_download_model_version "${model}" "${modelver}" "${mdir}" || return 2

    # The SDK links every model to a name without the trailing size, because
    # that is the name the runtime opens.
    [[ "${DRY_RUN}" -ne 1 ]] || continue
    shopt -s nullglob
    for base in "${mdir}"/*.trtpkg; do
      base="$(basename "${base}")"
      link="$(printf '%s' "${base}" | sed 's/_[0-9]\+\.trtpkg$/.trtpkg/')"
      if [[ "${link}" != "${base}" ]]; then
        ln -sfn "${base}" "${mdir}/${link}"
      fi
    done
    shopt -u nullglob
  done

  log "AFX feature packs are in ${features_dir}"
}

# Add the features that one feature pack needs to a work list.
#
# Arguments: <array name> <feature directory>
#
# A pack ships <Name>_dependencies.txt holding names such as
# "nvARFaceBoxDetection;". The NGC model name is the same word in lower case.
add_feature_dependencies() {
  local -n queue="$1"
  local dir="$2"
  local file dep known
  local -a deps=()

  shopt -s nullglob
  for file in "${dir}"/*_dependencies.txt; do
    mapfile -t deps < <(tr ';' '\n' < "${file}" | tr -d '[:space:]' | grep -v '^$' || true)
    for dep in ${deps[@]+"${deps[@]}"}; do
      dep="${dep,,}"
      for known in "${queue[@]}"; do
        [[ "${known}" != "${dep}" ]] || continue 2
      done
      log "Feature ${dep} is needed by another feature; adding it."
      queue+=("${dep}")
    done
  done
  shopt -u nullglob
}

# Download the VFX or AR feature packs over REST, in the layout the SDK's
# install_feature.sh writes:
#   <SDK root>/features/<model>/{include,lib}
#   <SDK root>/features/<model>/models/sm_<CC>/<engine>.trtpkg
rest_download_sdk_features() {
  local comp="$1"
  local label="${COMPONENT_LABEL[$comp]}"
  local root features_dir models_dir sm name libver modelver tmp pattern
  local -a models=()
  local index=0

  root="$(root_for "${comp}")"
  features_dir="${root}/features"
  # install_feature.sh puts every engine file in one flat directory beside the
  # SDK libraries, so this fallback writes them in the same place.
  models_dir="${root}/lib/models"
  sm="$(detect_sm)" || return 2
  pattern="$(feature_version_pattern "${comp}")"

  # "all" means whatever the SDK script would discover. This fallback has no
  # discovery of its own, so it takes the built in list of Linux features.
  if [[ "${COMPONENT_FEATURE_MODELS[$comp]}" == "all" ]]; then
    read -r -a models <<< "${COMPONENT_DEFAULT_FEATURES[$comp]}"
  else
    read -r -a models <<< "${COMPONENT_FEATURE_MODELS[$comp]}"
  fi
  if [[ "${#models[@]}" -eq 0 ]]; then
    err "${label}: no feature models named. Use --${comp}-features."
    return 2
  fi

  log "${label}: downloading feature packs for sm${sm}: ${models[*]}"
  mkdir -p "${features_dir}"

  # A feature can need another feature. The list grows while it is walked, so
  # that a narrowed list still gets everything it needs.
  while [[ "${index}" -lt "${#models[@]}" ]]; do
    name="${models[$index]}"
    index=$((index + 1))
    libver="$(pick_model_version "${name}" "^${pattern}_lib_linux$")" || return 2
    modelver="$(pick_model_version "${name}" "^${pattern}_models_linux_sm${sm}$")" || return 2

    if [[ -z "${libver}" && -z "${modelver}" ]]; then
      err "${label}: NGC has no Linux feature pack for ${name} on sm${sm}."
      return 2
    fi

    log "${label} ${name}: library ${libver:-none}, models ${modelver:-none}"

    if [[ -n "${libver}" ]]; then
      tmp="$(mktemp -d)"
      if ! sc_ngc_download_model_version "${name}" "${libver}" "${tmp}"; then
        rm -rf "${tmp}"
        return 2
      fi
      if [[ "${DRY_RUN}" -ne 1 ]] && ! extract_feature_libs "${tmp}" "${features_dir}"; then
        err "${label}: the feature library pack of ${name} holds no archive."
        rm -rf "${tmp}"
        return 2
      fi
      rm -rf "${tmp}"
      add_feature_dependencies models "${features_dir}/${name}"
    fi

    if [[ -n "${modelver}" ]]; then
      mkdir -p "${models_dir}"
      sc_ngc_download_model_version "${name}" "${modelver}" "${models_dir}" || return 2
    fi
  done

  log "${label} feature libraries are in ${features_dir}"
  log "${label} feature models are in ${models_dir}"
  log "${label}: these packs need the core SDK libraries. Run --download ${comp} as well."
}

# Work out the --gpu values for install_feature.sh from studiocast-maxine.
resolve_gpu_args() {
  local maxine_bin=""

  [[ "${#GPU_ARGS[@]}" -eq 0 ]] || return 0

  if [[ -x "${BUILD_DIR}/studiocast-maxine" ]]; then
    maxine_bin="${BUILD_DIR}/studiocast-maxine"
  elif command -v studiocast-maxine >/dev/null 2>&1; then
    maxine_bin="$(command -v studiocast-maxine)"
  fi

  [[ -n "${maxine_bin}" ]] || return 0

  log "Auto-detecting Maxine --gpu args via: ${maxine_bin} gpu list"
  # Example output includes: "(maxine --gpu turing)"
  mapfile -t GPU_ARGS < <("${maxine_bin}" gpu list | sed -n 's/.*(maxine --gpu \([^)]\+\)).*/\1/p' | sort -u)
}

# Print the SDK root that a feature script insists on, or nothing.
#
# install_feature.sh of SDK 1.2 starts with VFXSDK_PATH="/usr/local/VideoFX"
# (ARSDK_PATH for the AR SDK) and stops when that directory is missing. There is
# no flag and no variable to change it.
sdk_script_pinned_root() {
  sed -n 's/^[A-Za-z_]*SDK_PATH="\([^"]*\)".*/\1/p' "$1" | head -n 1
}

# True when the SDK script knows one --gpu name.
sdk_script_knows_gpu() {
  grep -qE "\[\"$2\"\]=" "$1"
}

# Run the SDK's own install_feature.sh for VFX or AR against our own SDK root.
#
# When the script pins another root, a copy with the root replaced is written
# next to the original. It must stay in the features directory, because the
# script finds its compute_capability helper beside itself. The copy is removed
# afterwards.
run_sdk_install_feature() {
  local comp="$1"
  local label="${COMPONENT_LABEL[$comp]}"
  local root script runner pinned gpu status
  local -a args=()
  local -a gpus=()

  root="$(root_for "${comp}")"
  script="${root}/features/install_feature.sh"
  runner="install_feature.sh"

  pinned="$(sdk_script_pinned_root "${script}")"
  if [[ -n "${pinned}" ]] && [[ ! "${pinned}" -ef "${root}" ]]; then
    log "${label}: install_feature.sh installs only into ${pinned}, which is not this install."
    log "${label}: running a copy of it that points at ${root}."
    runner="install_feature_local.sh"
    if [[ "${DRY_RUN}" -ne 1 ]]; then
      sed "s#^\([A-Za-z_]*SDK_PATH\)=\"${pinned}\"#\1=\"${root}\"#" \
        "${script}" > "${root}/features/${runner}"
      chmod +x "${root}/features/${runner}"
    fi
  fi

  # The script maps a GPU name to an architecture with a table of its own. A
  # name that is not in that table would stop it, so those are dropped and the
  # script reads the local GPU itself.
  resolve_gpu_args
  for gpu in ${GPU_ARGS[@]+"${GPU_ARGS[@]}"}; do
    if sdk_script_knows_gpu "${script}" "${gpu}"; then
      gpus+=("${gpu}")
    else
      log "${label}: install_feature.sh does not know the GPU name '${gpu}'; letting it detect the GPU."
    fi
  done

  args=(--feature "$(feature_list_argument "${comp}")" \
        --ngc-org "${NGC_ORG}" --ngc-team "${NGC_TEAM}")
  log "Installing ${label} features. NGC org/team: ${NGC_ORG}/${NGC_TEAM}"

  local out
  out="$(mktemp)"
  status=0
  if [[ "${#gpus[@]}" -eq 0 ]]; then
    log "${label} ${runner} ${args[*]}"
    if [[ "${DRY_RUN}" -ne 1 ]]; then
      ( cd "${root}/features" && "./${runner}" "${args[@]}" ) 2>&1 | tee -a "${out}" || status=$?
    fi
  else
    for gpu in "${gpus[@]}"; do
      log "${label} ${runner} --gpu ${gpu} ${args[*]}"
      if [[ "${DRY_RUN}" -eq 1 ]]; then
        continue
      fi
      ( cd "${root}/features" && "./${runner}" --gpu "${gpu}" "${args[@]}" ) 2>&1 | tee -a "${out}" || status=$?
      [[ "${status}" -eq 0 ]] || break
    done
  fi

  if [[ "${runner}" != "install_feature.sh" ]]; then
    rm -f "${root}/features/${runner}"
  fi

  # The exit code of the SDK script is only one input; what it installed decides.
  if [[ "${DRY_RUN}" -ne 1 ]]; then
    local script_status="${status}"
    status=0
    judge_feature_run "${comp}" "${out}" "${script_status}" || status=$?
  fi
  rm -f "${out}"

  return "${status}"
}

# Say whether a feature install worked, from what the SDK script printed.
#
# The script stops with 1 when one feature of the whole list has no Linux pack
# on NGC, even when every other feature installed. NVIDIA builds some features
# for Windows only, so that alone is not a reason to fail the install.
judge_feature_run() {
  local comp="$1"
  local out="$2"
  local status="$3"
  local label="${COMPONENT_LABEL[$comp]}"
  local installed
  local -a failed=()

  installed="$(grep -c '^\[OK\]' "${out}" 2>/dev/null || true)"
  mapfile -t failed < <(
    sed -n 's/^-- ERROR: Could not determine latest version for \(.*\)$/\1/p' "${out}" | sort -u
  )

  if [[ "${#failed[@]}" -gt 0 ]]; then
    log "${label}: NGC has no Linux pack for: ${failed[*]}"
    log "${label}: those features are built for Windows only, or they are not in this SDK."
  fi

  if [[ -n "${COMPONENT_FEATURES_EXPLICIT[$comp]:-}" && "${#failed[@]}" -gt 0 ]]; then
    err "${label}: a feature you asked for could not be installed: ${failed[*]}"
    return 2
  fi

  if [[ "${installed}" -gt 0 ]]; then
    log "${label}: ${installed} feature(s) installed."
    return 0
  fi

  err "${label}: no feature was installed (the SDK script ended with ${status})."
  [[ "${status}" -ne 0 ]] && return "${status}"
  return 2
}

# Print the --feature argument for the SDK script: a comma list, or "all".
feature_list_argument() {
  local list="${COMPONENT_FEATURE_MODELS[$1]}"
  if [[ "${list}" == "all" ]]; then
    printf 'all'
    return 0
  fi
  printf '%s' "${list// /,}"
}

# Run the SDK's own download_features.sh for AFX.
run_sdk_download_features() {
  local effects
  local -a args=()

  effects="${AFX_EFFECTS_CSV:-$AFX_EFFECTS_DEFAULT}"
  if [[ -z "${effects}" ]]; then
    err "--afx-effects was provided but empty."
    return 1
  fi

  # download_features.sh needs only curl or wget and the NGC_API_KEY variable.
  # Without -g it reads the compute capability of the local GPU itself.
  args=(--ngc-org "${NGC_ORG}" --ngc-team "${NGC_TEAM}" --effects "${effects}")
  if [[ -n "${AFX_GPU}" ]]; then
    args+=(--gpu "${AFX_GPU}")
  fi

  log "Downloading AFX features: ${effects}"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    log "Would run: ${AFX_ROOT}/features/download_features.sh ${args[*]}"
    return 0
  fi
  ( cd "${AFX_ROOT}/features" && ./download_features.sh "${args[@]}" )
}

# Install the feature packs of one component. The SDK's own script is the first
# choice, because NVIDIA keeps it in step with the SDK. The REST fallback exists
# for a machine that has no core SDK, for example an audio only install.
download_features_component() {
  local comp="$1"
  local root
  root="$(root_for "${comp}")"

  if [[ "${comp}" == "afx" ]]; then
    if [[ -x "${AFX_ROOT}/features/download_features.sh" ]]; then
      log "AFX: using the SDK script ${AFX_ROOT}/features/download_features.sh"
      run_sdk_download_features
      return $?
    fi
    log "AFX: no SDK feature script under ${AFX_ROOT}; using the NGC REST API."
    rest_download_afx_features
    return $?
  fi

  if [[ -f "${root}/features/install_feature.sh" ]]; then
    log "${COMPONENT_LABEL[$comp]}: using the SDK script ${root}/features/install_feature.sh"
    run_sdk_install_feature "${comp}"
    return $?
  fi

  log "${COMPONENT_LABEL[$comp]}: no SDK feature script under ${root}; using the NGC REST API."
  rest_download_sdk_features "${comp}"
}

if [[ "${#DOWNLOAD_COMPONENTS[@]}" -gt 0 ]]; then
  require_key "--download"
  sc_ngc_require_tools || exit 2

  log "Base: $BASE"
  log "Download cache: ${CACHE_DIR}"
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    log "Dry run: NGC is only asked what it holds. Nothing is written."
  fi

  for COMPONENT in "${DOWNLOAD_COMPONENTS[@]}"; do
    download_component "${COMPONENT}" || exit 2
  done
fi

if [[ "$DO_EXTRACT" -eq 1 ]]; then
  log "Base: $BASE"

  if [[ -n "$VFX_TAR" ]]; then
    extract_archive vfx "$VFX_TAR" || exit 1
  fi

  if [[ -n "$AR_TAR" ]]; then
    extract_archive ar "$AR_TAR" || exit 1
  fi

  if [[ -n "$AFX_TAR" ]]; then
    extract_archive afx "$AFX_TAR" || exit 1
  fi
fi

if [[ "${#FEATURE_COMPONENTS[@]}" -gt 0 ]]; then
  require_key "--download-features"
  sc_ngc_require_tools || exit 2

  for COMPONENT in "${FEATURE_COMPONENTS[@]}"; do
    download_features_component "${COMPONENT}" || exit 2
  done
fi

if [[ "$DO_INSTALL_FEATURES" -eq 1 ]]; then
  require_key "--install-features"

  if [[ ! -d "${VFX_ROOT}/features" ]]; then
    err "VFX features directory not found at ${VFX_ROOT}/features"
    exit 1
  fi
  if [[ ! -d "${AR_ROOT}/features" ]]; then
    err "AR features directory not found at ${AR_ROOT}/features"
    exit 1
  fi

  download_features_component vfx || exit 1
  download_features_component ar || exit 1

  log "Feature install complete."
  log "You can now verify with:"
  echo "  ${BUILD_DIR}/studiocast-maxine doctor"
fi

if [[ "$DO_INSTALL_AFX_FEATURES" -eq 1 ]]; then
  require_key "--install-afx-features"

  if [[ ! -d "${AFX_ROOT}/features" ]]; then
    err "AFX features directory not found at ${AFX_ROOT}/features"
    err "Make sure you extracted the Audio Effects SDK so that ${AFX_ROOT} exists,"
    err "for example with: $0 --download afx"
    exit 1
  fi

  run_sdk_download_features || exit 1

  log "AFX feature download complete."
  log "You can now verify with:"
  echo "  ${BUILD_DIR}/studiocast-maxine doctor"
  echo "  ${BUILD_DIR}/studiocastctl status"
fi

log "Roots:"
echo "  VFX: $VFX_ROOT"
echo "  AR : $AR_ROOT"
echo "  AFX: $AFX_ROOT"
log "Done."
