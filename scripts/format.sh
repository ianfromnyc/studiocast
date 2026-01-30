#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-}"

mapfile -t FILES < <(git ls-files \
  '*.h' '*.hpp' '*.hh' \
  '*.c' '*.cc' '*.cpp' '*.cxx')

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No source files found to format."
  exit 0
fi

clang-format -i "${FILES[@]}"

if [[ "$MODE" == "--check" ]]; then
  git diff --exit-code
fi
