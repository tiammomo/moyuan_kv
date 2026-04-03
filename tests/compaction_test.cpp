#include <algorithm>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "mokv/db.hpp"
#include "mokv/lsm/manifest.hpp"
#include "mokv/lsm/memtable.hpp"
#include "mokv/lsm/sst.hpp"
#include "mokv/pool/thread_pool.hpp"


TEST(Compaction, Read) {
    const int n = 40000;
    auto manifest = std::make_shared<mokv::lsm::Manifest>();
    {
        std::vector<std::string> keys;
        std::vector<std::string> values;
        keys.resize(n);
        values.resize(n);
        std::vector<mokv::lsm::EntryView> entries;
        // mokv::lsm::MemeTable entries;
        for (int i = 0; i < n; i++) {
            keys[i] = std::to_string(i);
            values[i] = std::to_string(i);
            // entries.Put(std::to_string(i), std::to_string(i));
            // std::cout << entries.back().key << " " << entries.back().value << std::endl;
        }
        std::sort(keys.begin(), keys.end());
        std::sort(values.begin(), values.end());
        for (int i = 0; i < n; i++) {
            entries.emplace_back(keys[i], values[i]);
        }
        auto sst1 = std::make_shared<mokv::lsm::SST>(entries, 1);
        std::string value;
        auto res = sst1->Get("1", value);
        std::cout << "sst get " << res << " " << value << std::endl;
        manifest = manifest->InsertAndUpdate(sst1);
        // res = manifest->Get("1", value);
        // std::cout << res << " ||| " << value << std::endl;
    }
    {
        std::vector<std::string> keys;
        std::vector<std::string> values;
        keys.resize(n);
        values.resize(n);
        std::vector<mokv::lsm::EntryView> entries;
        // mokv::lsm::MemeTable entries;
        for (int i = n; i < n + n; i++) {
            keys[i - n] = std::to_string(i);
            values[i - n] = std::to_string(i);
            // entries.Put(std::to_string(i), std::to_string(i));
            // std::cout << entries.back().key << " " << entries.back().value << std::endl;
        }
        std::sort(keys.begin(), keys.end());
        std::sort(values.begin(), values.end());
        for (int i = 0; i < n; i++) {
            entries.emplace_back(keys[i], values[i]);
        }
        auto sst2 = std::make_shared<mokv::lsm::SST>(entries, 2);
        std::string value;
        auto res = sst2->Get("10", value);
        std::cout << "sst get2 " << res << " " << value << std::endl;
        manifest = manifest->InsertAndUpdate(sst2);
        // res = manifest->Get("1", value);
        // std::cout << res << " ||| " << value << std::endl;
    }
    for (int i = 0; i < n + n; i++) {
        std::string value;
        bool res = manifest->Get(std::to_string(i), value);
        ASSERT_EQ(res, true);
        ASSERT_EQ(std::to_string(i), value);
    }
    manifest->SizeTieredCompaction(3);
    for (int i = 0; i < n + n; i++) {
        std::string value;
        bool res = manifest->Get(std::to_string(i), value);
        ASSERT_EQ(res, true);
        ASSERT_EQ(std::to_string(i), value);
    }
    {
        std::vector<std::string> keys;
        std::vector<std::string> values;
        keys.resize(n);
        values.resize(n);
        std::vector<mokv::lsm::EntryView> entries;
        // mokv::lsm::MemeTable entries;
        for (int i = n + n / 2; i < n + n + n / 2; i++) {
            keys[i - n - n / 2] = std::to_string(i);
            values[i - n - n / 2] = std::to_string(i);
            // entries.Put(std::to_string(i), std::to_string(i));
            // std::cout << entries.back().key << " " << entries.back().value << std::endl;
        }
        std::sort(keys.begin(), keys.end());
        std::sort(values.begin(), values.end());
        for (int i = 0; i < n; i++) {
            entries.emplace_back(keys[i], values[i]);
        }
        auto sst2 = std::make_shared<mokv::lsm::SST>(entries, 4);
        std::string value;
        auto res = sst2->Get("10", value);
        std::cout << "sst get2 " << res << " " << value << std::endl;
        manifest = manifest->InsertAndUpdate(sst2);
        // res = manifest->Get("1", value);
        // std::cout << res << " ||| " << value << std::endl;
    }

    {
        std::vector<std::string> keys;
        std::vector<std::string> values;
        keys.resize(n);
        values.resize(n);
        std::vector<mokv::lsm::EntryView> entries;
        // mokv::lsm::MemeTable entries;
        for (int i = n + n; i < n + n + n; i++) {
            keys[i - 2 * n] = std::to_string(i);
            values[i - 2 * n] = std::to_string(i);
            // entries.Put(std::to_string(i), std::to_string(i));
            // std::cout << entries.back().key << " " << entries.back().value << std::endl;
        }
        std::sort(keys.begin(), keys.end());
        std::sort(values.begin(), values.end());
        for (int i = 0; i < n; i++) {
            entries.emplace_back(keys[i], values[i]);
        }
        auto sst2 = std::make_shared<mokv::lsm::SST>(entries, 5);
        std::string value;
        auto res = sst2->Get("10", value);
        std::cout << "sst get2 " << res << " " << value << std::endl;
        manifest = manifest->InsertAndUpdate(sst2);
        // res = manifest->Get("1", value);
        // std::cout << res << " ||| " << value << std::endl;
    }
    manifest->SizeTieredCompaction(6);
    for (int i = 0; i < n + n + n; i++) {
        std::string value;
        bool res = manifest->Get(std::to_string(i), value);
        ASSERT_EQ(res, true);
        ASSERT_EQ(std::to_string(i), value);
    }

}