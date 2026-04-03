#include "mokv/pool/thread_pool.hpp"
#include <functional>
#include <gtest/gtest.h>
#include <vector>

TEST(ThreadPool, function) {
    cpputil::pool::ThreadPool pool(4);
    std::vector<std::function<void()>> funcs;
    const int n = 10, m = 10;
    for (int i = 0; i < n; i++) {
        funcs.emplace_back([i, m]() {
            for (int j = 0; j < m; j++) {
                std::cout << i << std::endl;
            }
        });
    }
    pool.ConcurrentRun(funcs);
}