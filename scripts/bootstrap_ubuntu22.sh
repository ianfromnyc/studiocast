#!/usr/bin/env bash
set -euo pipefail

ORT_VERSION="${ORT_VERSION:-1.24.1}"
ORT_FLAVOR="${ORT_FLAVOR:-gpu}"
ORT_ARCH="${ORT_ARCH:-x64}"

ORT_TGZ="onnxruntime-linux-${ORT_ARCH}-${ORT_FLAVOR}-${ORT_VERSION}.tgz"
ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_TGZ}"

# Install location for the extracted upstream tarball.
ORT_PREFIX="/opt/studiocast/onnxruntime/${ORT_VERSION}"
ORT_ROOT="${ORT_PREFIX}/onnxruntime-linux-${ORT_ARCH}-${ORT_FLAVOR}-${ORT_VERSION}"

sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config \
  qt6-base-dev qt6-base-dev-tools \
  qtbase5-dev \
  libpng-dev \
  libpulse-dev \
  clang clang-format clang-tidy \
  curl ca-certificates tar

if pkg-config --exists onnxruntime; then
  echo "onnxruntime already available via pkg-config; skipping ONNX Runtime install."
else
  tmpdir="$(mktemp -d)"
  trap 'rm -rf "${tmpdir}"' EXIT

  echo "Installing ONNX Runtime ${ORT_VERSION} (${ORT_FLAVOR}) from: ${ORT_URL}"
  echo "  -> ${ORT_ROOT}"

  curl -fsSL "${ORT_URL}" -o "${tmpdir}/${ORT_TGZ}"

  sudo mkdir -p "${ORT_PREFIX}"
  sudo tar -xzf "${tmpdir}/${ORT_TGZ}" -C "${ORT_PREFIX}"

  if [[ ! -f "${ORT_ROOT}/include/onnxruntime_cxx_api.h" ]]; then
    echo "ERROR: ONNX Runtime headers not found at ${ORT_ROOT}/include/onnxruntime_cxx_api.h"
    exit 1
  fi

  ort_libdir="${ORT_ROOT}/lib"
  if [[ -d "${ORT_ROOT}/lib64" ]]; then
    ort_libdir="${ORT_ROOT}/lib64"
  fi

  if [[ ! -e "${ort_libdir}/libonnxruntime.so" ]]; then
    sofile="$(ls -1 "${ort_libdir}"/libonnxruntime.so.* 2>/dev/null | head -n 1 || true)"
    if [[ -z "${sofile}" ]]; then
      echo "ERROR: libonnxruntime.so not found under ${ort_libdir}"
      exit 1
    fi
    sudo ln -sf "$(basename "${sofile}")" "${ort_libdir}/libonnxruntime.so"
  fi

  # Make the shared library discoverable for runtime linking.
  sudo tee /etc/ld.so.conf.d/studiocast-onnxruntime.conf >/dev/null <<EOF
${ort_libdir}
EOF
  sudo ldconfig

  # Provide a pkg-config file so our CMake can pick it up via pkg_check_modules(onnxruntime).
  sudo mkdir -p /usr/local/lib/pkgconfig
  sudo tee /usr/local/lib/pkgconfig/onnxruntime.pc >/dev/null <<EOF
prefix=${ORT_ROOT}
exec_prefix=\${prefix}
libdir=${ort_libdir}
includedir=\${prefix}/include

Name: onnxruntime
Description: ONNX Runtime
Version: ${ORT_VERSION}
Libs: -L\${libdir} -lonnxruntime
Cflags: -I\${includedir}
EOF

  echo "ONNX Runtime installed; pkg-config reports: $(pkg-config --modversion onnxruntime)"
fi

echo "Done. Build with:"
echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug"
echo "  cmake --build build"
