#include <gtest/gtest.h>
#include <iostream>
#include <string>
#include "mokv/utils/bloom_filter.hpp"
TEST(BloomFilteTest, Function) {
    mokv::common::BloomFilter bloom_filter;
    const int n = 100000;
    bloom_filter.Init(n, 0.01);
    std::cout << bloom_filter.length() << " length " << std::endl;
    for (int i = 0; i < n; i++) {
        std::string number = std::to_string(i);
        bloom_filter.Insert(number.c_str(), number.size());
    }
    for (int i = 0; i < n; i++) {
        std::string number = std::to_string(i);
        ASSERT_EQ(bloom_filter.Check(number.c_str(), number.size()), true);
    }
    size_t cnt = 0;
    for (int i = n; i < n + n; i++) {
        std::string number = std::to_string(i);
        cnt += bloom_filter.Check(number.c_str(), number.size());
    }
    std::cout << " n " << n << " cnt "<< cnt << std::endl;
    std::cout << "error rate " << double(cnt) / n << std::endl;

    std::cout << "binary size " << bloom_filter.binary_size() << std::endl;
    char* data = new char[bloom_filter.binary_size()];
    bloom_filter.Save(data);
    mokv::common::BloomFilter new_filter;
    std::cout << "load size: " << new_filter.Load(data) << std::endl;
    std::cout << "new binary size " << new_filter.binary_size() << std::endl;
    for (int i = 0; i < n; i++) {
        std::string number = std::to_string(i);
        ASSERT_EQ(new_filter.Check(number.c_str(), number.size()), true);
    }
    size_t new_cnt = 0;
    for (int i = n; i < n + n; i++) {
        std::string number = std::to_string(i);
        new_cnt += new_filter.Check(number.c_str(), number.size());
    }
    ASSERT_EQ(cnt, new_cnt);
    delete[] data;
}