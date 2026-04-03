#!/usr/bin/env bash

set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Please run as root or with sudo:"
  echo "  sudo bash scripts/install_cpp_env_ubuntu.sh"
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  pkg-config \
  ccache \
  gdb \
  clang \
  clangd \
  lldb \
  git \
  curl \
  ca-certificates \
  libgrpc++-dev \
  libprotobuf-dev \
  protobuf-compiler \
  protobuf-compiler-grpc \
  liblz4-dev \
  liburing-dev \
  libboost-all-dev \
  googletest

echo
echo "C++ development environment for mokv is installed."
echo "Next:"
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build --target mokv_server mokv_client -j\$(nproc)"
