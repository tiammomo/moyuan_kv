#include <iostream>
#include <string>

#include "mokv/cache/concurrent_cache.hpp"
#include "mokv/utils/global_random.h"

#include <gtest/gtest.h>
#include <string_view>
#include <thread>

using namespace cpputil::cache;

TEST(CacheTest, ConcurrentLRUCacheBasicFunction) {
    ConcurrentLRUCache<std::string_view, std::string> string_cahce(99, 1);
    
    for (int i = 0; i < 100; i++) {
        string_cahce.Put(std::to_string(i));
    }
    std::cout << "finish put" << std::endl;
    ASSERT_EQ(string_cahce.Get("0"), nullptr);

    string_cahce.Put("101");
    ASSERT_EQ(string_cahce.Get("1"), nullptr);
    ASSERT_EQ(*string_cahce.Get("2"), "2");

    ConcurrentLRUCache<int> int_cache(99, 1);
    for (int i = 0; i < 100; i++) {
        int_cache.Put(i);
    }
    
    ASSERT_EQ(string_cahce.Get("0"), nullptr);

    int_cache.Put(101);
    ASSERT_EQ(int_cache.Get(1), nullptr);
    ASSERT_EQ(*int_cache.Get(2), 2);
}

TEST(CacheTest, ConcurrentLRUCache) {

    ConcurrentLRUCache<int> cache(20);
    const int num = 30;
    std::vector<std::thread> threads;
    const int n = 30, m = 1000;
    for (int i = 0; i < n; i++) {
        threads.emplace_back([&, i] {
            const int x = i;
            const int id = i;
            for (int i = 0; i < m; i++) {
                std::cout << id << " write " << x << std::endl;
                cache.Put(x);
            }
        });
    }
    for (int i = 0; i < n; i++) {
        threads.emplace_back([&] {
            int x = cpputil::common::GlobalRand() % num;
            for (int i = 0; i < m; i++) {
                x = cpputil::common::GlobalRand() % num;
                auto it = cache.Get(x);
                if (it) {
                    std::cout << (*it) << std::endl;
                } else {
                    std::cout << "nullptr " << x << " " << cpputil::common::GlobalRand() << " " << cache.TrueSize() << " " << cache.size() << std::endl;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

TEST(CacheTest, Concurrent2LRUCache) {
    // Concurrent2LRUCache<int> cache(20);
    // const int num = 30;
    // std::vector<std::thread> threads;
    // const int n = 30, m = 1000;
    // for (int i = 0; i < n; i++) {
    //     threads.emplace_back([&, i] {
    //         const int x = i;
    //         const int id = i;
    //         for (int i = 0; i < m; i++) {
    //             std::cout << id << " write " << x << std::endl;
    //             cache.Put(x);
    //         }
    //     });
    // }
    // for (int i = 0; i < n; i++) {
    //     threads.emplace_back([&] {
    //         int x = cpputil::common::GlobalRand() % num;
    //         for (int i = 0; i < m; i++) {
    //             x = cpputil::common::GlobalRand() % num;
    //             auto it = cache.Get(x);
    //             if (it) {
    //                 std::cout << (*it) << std::endl;
    //             } else {
    //                 std::cout << "nullptr " << x << " " << cpputil::common::GlobalRand() << std::endl;
    //             }
    //         }
    //     });
    // }
    // for (auto& thread : threads) {
    //     thread.join();
    // }
}

TEST(CacheTest, ConcurrentBucketLRUCache) {
    ConcurrentBucketLRUCache<int> cache("test", 1024);
    const int num = 1500;
    std::vector<std::thread> threads;
    const int n = 30, m = 1000;
    for (int i = 0; i < n; i++) {
        threads.emplace_back([&, i] {
            const int x = i;
            const int id = i;
            for (int i = 0; i < m; i++) {
                std::cout << id << " write " << x << std::endl;
                cache.Put(x);
            }
        });
    }
    for (int i = 0; i < n; i++) {
        threads.emplace_back([&] {
            int x = cpputil::common::GlobalRand() % num;
            for (int i = 0; i < m; i++) {
                x = cpputil::common::GlobalRand() % num;
                auto it = cache.Get(x);
                if (it) {
                    std::cout << (*it) << std::endl;
                } else {
                    std::cout << "nullptr " << x << " " << cpputil::common::GlobalRand() << std::endl;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

TEST(CacheTest, ConcurrentBucket2LRUCache) {
    // ConcurrentBucket2LRUCache<int> cache("test", 1024);
    // std::cout << "finish init" << std::endl;
    // const int num = 1500;
    // std::vector<std::thread> threads;
    // const int n = 30, m = 1000;
    // for (int i = 0; i < n; i++) {
    //     threads.emplace_back([&, i] {
    //         const int x = i;
    //         const int id = i;
    //         for (int i = 0; i < m; i++) {
    //             std::cout << id << " write " << x << std::endl;
    //             cache.Put(x);
    //         }
    //     });
    // }
    // for (int i = 0; i < n; i++) {
    //     threads.emplace_back([&] {
    //         int x = cpputil::common::GlobalRand() % num;
    //         for (int i = 0; i < m; i++) {
    //             x = cpputil::common::GlobalRand() % num;
    //             auto it = cache.Get(x);
    //             if (it) {
    //                 std::cout << (*it) << std::endl;
    //             } else {
    //                 std::cout << "nullptr " << x << " " << cpputil::common::GlobalRand() << std::endl;
    //             }
    //         }
    //     });
    // }
    // for (auto& thread : threads) {
    //     thread.join();
    // }
}