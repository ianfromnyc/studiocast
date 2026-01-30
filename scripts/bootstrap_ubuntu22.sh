#!/usr/bin/env bash
set -euo pipefail

sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-dev-tools \
  qtbase5-dev \
  clang clang-format clang-tidy

echo "Done. Build with:"
echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug"
echo "  cmake --build build"
