#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include <vector>

#include "mokv/lsm/skiplist.hpp"
#include "mokv/pool/thread_pool.hpp"

TEST(SkipList, Function) {
    mokv::lsm::ConcurrentSkipList skip_list;
    const int n = 100;
    std::cout << "finish init" << std::endl;
    for (int i = 0; i < n; i++) {
        skip_list.Put(std::to_string(i), std::to_string(i));
    }
    std::cout << "finish put" << std::endl;
    for (int i = 0; i < n; i++) {
        std::string value;
        ASSERT_EQ(skip_list.Get(std::to_string(i), value), true);
        ASSERT_EQ(value, std::to_string(i));
    }
    for (int i = n; i < n + n; i++) {
        std::string value;
        ASSERT_EQ(skip_list.Get(std::to_string(i), value), false);
    }
    for (int i = 0; i < n / 2; i++) {
        skip_list.Delete(std::to_string(i));
    }
    for (int i = 0; i < n / 2; i++) {
        std::string value;
        ASSERT_EQ(skip_list.Get(std::to_string(i), value), false);
    }
    for (int i = n / 2 + 1; i < n; i++) {
        std::string value;
        ASSERT_EQ(skip_list.Get(std::to_string(i), value), true);
        ASSERT_EQ(value, std::to_string(i));
    }
    ASSERT_EQ(skip_list.size(), n - (n / 2));
}

TEST(SkipList, Concurrent) {
    mokv::lsm::ConcurrentSkipList skip_list;
    std::vector<std::function<void()>> functions;
    const int n = 200;   // 减少测试数据量
    const int m = 2;     // 减少并发数
    cpputil::pool::ThreadPool pool(m + 2);
    functions.emplace_back([n, &skip_list]() {
        for (int i = 0; i < n; i++) {
            skip_list.Put(std::to_string(i), std::to_string(i));
        }
    });
    for (int t = 0; t < m; t++) {
        functions.emplace_back([n, &skip_list]() {
            for (int i = 0; i < n; i++) {
                std::string value;
                skip_list.Get(std::to_string(i), value);
            }
        });
    }
    functions.emplace_back([n, &skip_list]() {
        for (int i = 0; i < n; i++) {
            skip_list.Delete(std::to_string(i));
        }
    });
    pool.ConcurrentRun(functions);
}