# MoKV Docker 构建文件
# 使用方式:
#   构建: docker build -t mokv .
#   运行: docker run -p 9001:9001 -v data:/data mokv

# ============ 构建阶段 ============
FROM ubuntu:24.04 AS builder

# 安装构建工具
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    && rm -rf /var/lib/apt/lists/*

# 安装运行时依赖（用于构建时链接）
RUN apt-get update && apt-get install -y \
    libprotobuf-dev \
    protobuf-compiler-grpc \
    libgrpc++-dev \
    liburing-dev \
    libboost-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# 复制源码
COPY . .

# 构建项目
RUN chmod +x build.sh
RUN ./build.sh

# ============ 运行阶段 ============
FROM ubuntu:24.04 AS runtime

# 安装运行时依赖
RUN apt-get update && apt-get install -y \
    libgrpc++-dev \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

# 创建非 root 用户
RUN useradd -m -s /bin/bash appuser && \
    mkdir -p /data /app && \
    chown -R appuser:appuser /data /app

WORKDIR /app

# 从构建阶段复制可执行文件
COPY --from=builder /app/build/bin/mokv_server ./server
COPY --from=builder /app/build/bin/mokv_client ./client
COPY raft.cfg ./

# 切换到非 root 用户
USER appuser

# 暴露端口
EXPOSE 9001

# 健康检查
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD nc -zv localhost 9001 || exit 1

# 默认命令
CMD ["./server"]
