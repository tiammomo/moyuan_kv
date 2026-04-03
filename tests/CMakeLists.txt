# MoKV 测试用例

# 优先使用系统提供的 googletest 源码，避免测试配置依赖外网拉取
find_package(GTest CONFIG QUIET)

if(NOT TARGET GTest::gtest_main)
    if(EXISTS /usr/src/googletest/CMakeLists.txt)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
        set(gtest_build_samples OFF CACHE BOOL "" FORCE)
        set(gtest_build_tests OFF CACHE BOOL "" FORCE)
        add_subdirectory(/usr/src/googletest
            ${CMAKE_CURRENT_BINARY_DIR}/third_party/googletest
            EXCLUDE_FROM_ALL
        )
    else()
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_build_samples OFF CACHE BOOL "" FORCE)
        set(gtest_build_tests OFF CACHE BOOL "" FORCE)
        set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

# 启用测试
enable_testing()

# 测试源文件
set(TEST_SOURCES
    skip_list_test.cpp
    db_test.cpp
    lock_test.cpp
    thread_pool_test.cpp
    list_test.cpp
    bloom_filter_test.cpp
    cache_test.cpp
    compaction_test.cpp
    cm_sketch_test.cpp
    compression_test.cpp
    llm_store_test.cpp
)

# 创建测试可执行文件
foreach(source ${TEST_SOURCES})
    get_filename_component(test_name ${source} NAME_WE)
    add_executable(${test_name} ${source})
    # 注意：静态库链接顺序，mokv 放在最后以确保正确解析符号
    target_link_libraries(${test_name}
        PRIVATE
            GTest::gtest_main
            mokv_grpc
            gRPC::grpc++
            grpc
            protobuf::libprotobuf
            mokv
    )
    target_include_directories(${test_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..
            ${CMAKE_CURRENT_BINARY_DIR}/mokv
    )
    set(test_workdir "${CMAKE_CURRENT_BINARY_DIR}/${test_name}_workdir")
    file(MAKE_DIRECTORY "${test_workdir}")
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY "${test_workdir}")
endforeach()

message(STATUS "Tests configured: ${TEST_SOURCES}")
message(STATUS "Benchmark: benchmark.cpp")
