#!/bin/bash
# 构建 mokv 服务器 (CMake)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 检查构建目录
if [ ! -d "build" ]; then
    echo "Creating build directory..."
    mkdir -p build
fi

echo "Building mokv server and client..."
cd build
if [ ! -f "CMakeCache.txt" ]; then
    cmake .. -DCMAKE_BUILD_TYPE=Release
fi
cmake --build . --target mokv_server mokv_client -j$(nproc)

echo "Build successful!"
echo "Binary locations:"
echo "  - build/bin/mokv_server"
echo "  - build/bin/mokv_client"
