# MoKV 核心库

# Proto 文件
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/raft/protos/raft.proto)
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/raft/protos)

# 检查 proto 生成的文件是否存在，不存在则生成
if(NOT EXISTS ${PROTO_DIR}/raft.pb.cc)
    message(STATUS "Proto files not found, generating...")
    execute_process(
        COMMAND bash -c "cd ${PROTO_DIR} && protoc --cpp_out=. --grpc_out=. --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) raft.proto"
        RESULT_VARIABLE proto_result
    )
    if(NOT proto_result EQUAL 0)
        message(FATAL_ERROR "Failed to generate proto files")
    endif()
endif()

# 使用源目录中的 proto 生成文件
add_library(mokv_grpc STATIC
    ${PROTO_DIR}/raft.pb.cc
    ${PROTO_DIR}/raft.grpc.pb.cc
)

# 添加 proto 文件目录到 include 路径
target_include_directories(mokv_grpc
    PUBLIC
        ${PROTO_DIR}
)

# 源文件列表
set(MOKV_SOURCES
    kvstore.cpp
    utils/global_random.cpp
    lsm/async_sst_writer.cpp
)

set(MOKV_HEADERS
    # 顶层头文件
    config.hpp
    db.hpp
    db_client.hpp
    kvstore.hpp
    resource_manager.hpp
    llm/store.hpp

    # Cache 模块
    cache/cm_sketch.hpp
    cache/concurrent_cache.hpp
    cache/list.hpp

    # Pool 模块
    pool/thread_pool.hpp

    # Utils 模块
    utils/lock.hpp
    utils/global_random.h
    utils/global_random.cpp
    utils/ring_buffer_queue.hpp
    utils/compression.hpp
    utils/bloom_filter.hpp
    utils/codec.hpp

    # LSM 模块
    lsm/async_io.hpp
    lsm/manifest.hpp
    lsm/block_cache.hpp
    lsm/async_sst_writer.hpp
    lsm/sst.hpp
    lsm/memtable.hpp
    lsm/skiplist.hpp
    lsm/batch_commit.hpp
    lsm/read_quorum.hpp

    # Raft 模块
    raft/raft_log.hpp
    raft/config.hpp
    raft/client.hpp
    raft/service.hpp
    raft/pod.hpp
)

# 创建静态库
add_library(mokv STATIC ${MOKV_SOURCES} ${MOKV_HEADERS})

target_include_directories(mokv
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(mokv
    PUBLIC
        mokv_grpc
        LZ4::LZ4
        gRPC::grpc++
        grpc
        protobuf::libprotobuf
)

# Server 可执行文件
add_executable(mokv_server
    server.cpp
)

target_include_directories(mokv_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(mokv_server
    PRIVATE
        mokv
)

# Client 可执行文件
add_executable(mokv_client
    client.cpp
)

target_include_directories(mokv_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(mokv_client
    PRIVATE
        mokv
)

# 单元测试（在父 CMakeLists.txt 中处理）
