/**
 * @file benchmark.cpp
 * @brief MoKV 性能基准测试
 *
 * 测试项目：
 * - SkipList 读写性能
 * - BlockCache 命中率
 * - Raft 日志写入延迟
 * - 端到端 Put/Get 吞吐量
 */

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "mokv/lsm/skiplist.hpp"
#include "mokv/lsm/block_cache.hpp"
#include "mokv/raft/raft_log.hpp"
#include "mokv/utils/bloom_filter.hpp"

namespace {

// 生成随机字符串
std::string GenerateRandomString(size_t length) {
    static const char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static std::mt19937 rng(std::random_device{}());
    static std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);

    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += charset[dist(rng)];
    }
    return result;
}

// 生成随机键值对
std::vector<std::pair<std::string, std::string>> GenerateRandomPairs(
    size_t count, size_t key_size = 16, size_t value_size = 128) {
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pairs.emplace_back(GenerateRandomString(key_size),
                          GenerateRandomString(value_size));
    }
    return pairs;
}

}  // namespace

// 基准测试结果
struct BenchmarkResult {
    std::string name;
    size_t operations;
    double total_time_ms;
    double throughput_ops_per_sec;
    double avg_latency_us;

    void Print() const {
        std::cout << "\n========== " << name << " ==========\n";
        std::cout << "Operations: " << operations << "\n";
        std::cout << "Total time: " << total_time_ms << " ms\n";
        std::cout << "Throughput: " << throughput_ops_per_sec << " ops/sec\n";
        std::cout << "Avg latency: " << avg_latency_us << " us/op\n";
    }
};

// 运行基准测试
template <typename Func>
BenchmarkResult RunBenchmark(const std::string& name, size_t iterations, Func func) {
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < iterations; ++i) {
        func(i);
    }

    auto end = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

    BenchmarkResult result;
    result.name = name;
    result.operations = iterations;
    result.total_time_ms = total_ms;
    result.throughput_ops_per_sec = iterations / (total_ms / 1000.0);
    result.avg_latency_us = (total_ms * 1000.0) / iterations;
    return result;
}

int main() {
    std::cout << "MoKV Performance Benchmark\n";
    std::cout << "================================\n\n";

    const size_t kTestSize = 100000;
    const size_t kValueSize = 256;

    // ============ SkipList 基准测试 ============
    std::cout << "Testing SkipList...\n";

    {
        mokv::lsm::ConcurrentSkipList skip_list;
        auto pairs = GenerateRandomPairs(kTestSize, 16, kValueSize);

        // 插入测试
        auto insert_result = RunBenchmark("SkipList Insert", kTestSize,
            [&](size_t i) { skip_list.Put(pairs[i].first, pairs[i].second); });
        insert_result.Print();

        // 读取测试
        auto read_result = RunBenchmark("SkipList Read", kTestSize,
            [&](size_t i) {
                std::string value;
                skip_list.Get(pairs[i].first, value);
            });
        read_result.Print();

        // 混合读写测试 (80% 读, 20% 写)
        std::cout << "\nTesting SkipList Mixed (80% Read, 20% Write)...\n";
        size_t read_count = 0, write_count = 0;
        auto mixed_result = RunBenchmark("SkipList Mixed", kTestSize,
            [&](size_t i) {
                if (i % 5 != 0) {
                    std::string value;
                    if (skip_list.Get(pairs[i % kTestSize].first, value)) {
                        ++read_count;
                    }
                } else {
                    auto new_pairs = GenerateRandomPairs(1, 16, kValueSize);
                    if (skip_list.Put(new_pairs[0].first, new_pairs[0].second)) {
                        ++write_count;
                    }
                }
            });
        std::cout << "Read operations: " << read_count << "\n";
        std::cout << "Write operations: " << write_count << "\n";
        mixed_result.Print();
    }

    // ============ BloomFilter 基准测试 ============
    std::cout << "\nTesting BloomFilter...\n";

    {
        constexpr size_t kBloomCapacity = 1000000;

        mokv::common::BloomFilter bloom_filter;
        bloom_filter.Init(kBloomCapacity, 0.01);

        auto keys = GenerateRandomPairs(kTestSize, 16, 0);

        // 插入测试
        auto insert_result = RunBenchmark("BloomFilter Insert", kTestSize,
            [&](size_t i) { bloom_filter.Insert(keys[i].first.data(), keys[i].first.size()); });
        insert_result.Print();

        // 查询测试
        auto check_result = RunBenchmark("BloomFilter Check", kTestSize,
            [&](size_t i) { bloom_filter.Check(keys[i].first.data(), keys[i].first.size()); });
        check_result.Print();
    }

    // ============ BlockCache 基准测试 ============
    std::cout << "\nTesting BlockCache...\n";

    {
        mokv::lsm::BlockCache::Config config;
        config.max_capacity = 64 * 1024 * 1024;  // 64MB
        config.min_block_size = 4096;

        mokv::lsm::BlockCache cache(config);
        auto blocks = GenerateRandomPairs(kTestSize, 16, 4096);

        // 写入缓存
        auto write_result = RunBenchmark("BlockCache Write", kTestSize,
            [&](size_t i) {
                std::vector<char> data(blocks[i].second.begin(), blocks[i].second.end());
                cache.Put(i, i * 4096, std::move(data));
            });
        write_result.Print();

        // 读取缓存
        auto read_result = RunBenchmark("BlockCache Read", kTestSize,
            [&](size_t i) { cache.Get(i, i * 4096); });
        read_result.Print();

        // 统计信息
        auto stats = cache.GetStats();
        std::cout << "\nBlockCache Stats:\n";
        std::cout << "Hit count: " << stats.hit_count << "\n";
        std::cout << "Miss count: " << stats.miss_count << "\n";
        std::cout << "Total access: " << stats.total_access << "\n";
        std::cout << "Hit rate: " << stats.HitRate() * 100 << "%\n";
    }

    // ============ 总结 ============
    std::cout << "\n================================\n";
    std::cout << "Benchmark Complete\n";

    return 0;
}
