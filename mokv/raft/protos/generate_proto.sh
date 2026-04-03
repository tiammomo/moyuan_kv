#!/bin/bash
# 生成 gRPC 和 Protocol Buffers 代码

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# 检查 protoc 和 grpc_cpp_plugin 是否可用
if ! command -v protoc &> /dev/null; then
    echo "Error: protoc not found. Please install protobuf-compiler-grpc."
    exit 1
fi

if ! command -v grpc_cpp_plugin &> /dev/null; then
    echo "Error: grpc_cpp_plugin not found. Please install libgrpc++-dev."
    exit 1
fi

echo "Generating gRPC and Protocol Buffers code..."

# 生成 .pb.h 和 .pb.cc
protoc --cpp_out=. \
    --grpc_out=. \
    --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) \
    raft.proto

# 检查生成的文件
if [ -f "raft.pb.h" ] && [ -f "raft.grpc.pb.h" ]; then
    echo "Successfully generated:"
    echo "  - raft.pb.h"
    echo "  - raft.pb.cc"
    echo "  - raft.grpc.pb.h"
    echo "  - raft.grpc.pb.cc"
else
    echo "Error: Failed to generate proto files"
    exit 1
fi

echo "Done."
