#!/usr/bin/env bash
set -euo pipefail

sc_die() {
  echo "[scripts] ERROR: $*" >&2
  exit 2
}

sc_script_dir() {
  cd -- "$(dirname -- "${BASH_SOURCE[1]}")" && pwd
}

sc_repo_root() {
  local caller_dir
  caller_dir="$(sc_script_dir)"

  # scripts/*/* -> repo root is two levels up
  # scripts/*   -> repo root is one level up
  if [[ -d "${caller_dir}/../.." && -f "${caller_dir}/../../CMakeLists.txt" ]]; then
    cd -- "${caller_dir}/../.." && pwd
    return 0
  fi
  if [[ -d "${caller_dir}/.." && -f "${caller_dir}/../CMakeLists.txt" ]]; then
    cd -- "${caller_dir}/.." && pwd
    return 0
  fi

  # Best-effort fallback: walk upward a few levels.
  local p="$caller_dir"
  for _ in 1 2 3 4 5; do
    if [[ -f "${p}/CMakeLists.txt" ]]; then
      cd -- "$p" && pwd
      return 0
    fi
    p="$(cd -- "${p}/.." && pwd)"
  done

  sc_die "Could not determine repo root from: ${caller_dir}"
}
