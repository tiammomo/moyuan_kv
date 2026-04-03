cmake_minimum_required(VERSION 3.16)
project(mokv
    VERSION 0.1.0
    DESCRIPTION "An LLM-ready distributed key-value store based on LSM Tree and Raft consensus"
    LANGUAGES CXX
)

# C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 查找 gRPC 和 Protobuf
find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)
find_path(LZ4_INCLUDE_DIR NAMES lz4frame.h REQUIRED)
find_library(LZ4_LIBRARY NAMES lz4 REQUIRED)

if(NOT TARGET LZ4::LZ4)
    add_library(LZ4::LZ4 UNKNOWN IMPORTED)
    set_target_properties(LZ4::LZ4 PROPERTIES
        IMPORTED_LOCATION "${LZ4_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LZ4_INCLUDE_DIR}"
    )
endif()

# 打印配置信息
message(STATUS "gRPC version: ${gRPC_VERSION}")
message(STATUS "Protobuf version: ${Protobuf_VERSION}")
message(STATUS "LZ4 include: ${LZ4_INCLUDE_DIR}")
message(STATUS "LZ4 library: ${LZ4_LIBRARY}")

# 添加子目录
add_subdirectory(mokv)

# 测试
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# 安装配置
include(GNUInstallDirs)

install(TARGETS mokv_server mokv_client
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES raft.cfg
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/mokv
)

message(STATUS "Build configuration complete")
message(STATUS "Use: cmake --build <build_dir> --target mokv_server")
