/**
 * @file manifest.hpp
 * @brief Manifest 元数据管理 - LSM树的层式结构管理
 *
 * Manifest 管理 LSM 树的所有 SST 文件元数据，包括：
 * - 版本管理：记录当前版本号
 * - 分层管理：将 SST 文件按层组织（Level 0 到 Level N）
 * -  Compaction 协调：触发和管理 Compaction 过程
 *
 * @section manifest_structure Manifest 结构
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                      Manifest 文件格式                          │
 * ├─────────────────────────────────────────────────────────────────┤
 * │  [version(8B)] [levels(8B)] | (id_0, id_1, -1) | (id_2, ...)   │
 * │        │            │            │              │                │
 * │    版本号       层数量      Level 0      Level 1...             │
 * │                                                                 │
 │  每层的 SST 文件以 -1 结尾，表示该层结束                           │
 └─────────────────────────────────────────────────────────────────┘
 *
 * @section compaction_strategy Compaction 策略
 *
 * 本实现使用 Size-Tiered Compaction：
 * 1. 当某一层的总大小超过阈值时触发 Compaction
 * 2. 从该层和下一层收集 SST 文件
 * 3. 合并所有键值对，去重（保留最新版本）
 * 4. 生成新的更大的 SST 文件放到下一层
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "mokv/lsm/skiplist.hpp"
#include "mokv/lsm/sst.hpp"
#include "mokv/lsm/memtable.hpp"
#include "mokv/utils/compression.hpp"

namespace mokv {
namespace lsm {

/**
 * @class Manifest
 * @brief Manifest 元数据管理器 - SST 文件的版本和分层管理
 *
 * Manifest 采用 Copy-on-Write 模式实现无锁读取：
 * - 每次修改创建新版本，旧版本继续服务读取请求
 * - 版本之间通过引用计数管理生命周期
 *
 * @section tiered_storage 分层存储
 * ┌─────────────────────────────────────────────────────────────┐
 * │                    LSM Tree 分层结构                         │
 * ├─────────────────────────────────────────────────────────────┤
 * │                                                                 │
 * │   Level 0:    1 KB   │  新刷入的 SST（可能重叠）               │
 * │   Level 1:   10 MB   │                                          │
 * │   Level 2:  100 MB   │        逐层合并                          │
 * │   Level 3:    1 GB   │                                          │
 * │   Level 4:   10 GB   │                                          │
 * │                                                                 │
 * │   MemTable ──→ L0 ──→ L1 ──→ L2 ──→ L3 ──→ L4                 │
 * │                         │                                     │
 * │                    Compaction                                │
 *                                                                 │
 * @section version_management 版本管理
 * - 每个 Manifest 有一个版本号（version_）
 * - 每次修改（插入/Compaction）版本号+1
 * - 旧版本继续可用，支持无锁读取
 */
class Manifest {
public:
    /**
     * @brief Level - 单层 SST 文件管理
     *
     * 每层包含多个 SST 文件，按 key 范围排序。
     * Level 0 比较特殊：SST 文件可能有 key 范围重叠。
     * Level 1+：每层的 SST 文件 key 范围不重叠。
     */
    class Level {
    public:
        /**
         * @brief 构造函数
         * @param level 层编号（0-based）
         */
        explicit Level(size_t level) : level_(level) {}

        /**
         * @brief 从序列化数据加载 Level
         * @param s 指向序列化数据的指针
         * @param max_sst_id 输出参数，更新最大 SST ID
         * @return 读取的字节数
         */
        size_t Load(char* s, size_t& max_sst_id, const std::filesystem::path& data_dir) {
            size_t index = 0;
            size_t sst_id;
            while (true) {
                sst_id = *reinterpret_cast<size_t*>(s + index);
                index += sizeof(size_t);
                if (sst_id != static_cast<size_t>(-1)) {
                    auto sst_ptr = std::make_shared<SST>();
                    sst_ptr->SetDataDir(data_dir);
                    sst_ptr->SetId(static_cast<int>(sst_id));
                    sst_ptr->Load();
                    ssts_.emplace_back(sst_ptr);
                    if (sst_id > max_sst_id) {
                        max_sst_id = sst_id;
                    }
                } else {
                    break;  // -1 表示该层结束
                }
            }
            return index;
        }

        /**
         * @brief 序列化 Level 到内存
         * @param s 目标内存指针
         * @return 写入的字节数
         */
        size_t Save(char* s) {
            size_t index = 0;
            for (auto& sst_ptr : ssts_) {
                *reinterpret_cast<size_t*>(s + index) = static_cast<size_t>(sst_ptr->id());
                index += sizeof(size_t);
            }
            *reinterpret_cast<size_t*>(s + index) = static_cast<size_t>(-1);  // 结束标记
            index += sizeof(size_t);
            return index;
        }

        /**
         * @brief 在该层中查找键
         * @param key 要查找的键
         * @param value 输出参数，找到的值
         * @return 找到返回 true
         *
         * @section level0_vs_others Level 0 与其他层的区别
         *
         * Level 0：从 MemTable 直接刷盘，可能有 key 范围重叠
         *        需要逆序查找（最新的 SST 在前面）
         * Level 1+：经过 Compaction，key 范围不重叠
         *        可以使用二分查找快速定位
         */
        bool Get(std::string_view key, std::string& value) {
            if (level_ == 0) {
                // Level 0：逆序查找（新的 SST 在前面，优先级高）
                for (auto it = ssts_.rbegin(); it != ssts_.rend(); ++it) {
                    if ((*it)->Get(key, value)) {
                        return true;
                    }
                }
            } else {
                // 其他层：二分查找
                size_t l = 0, r = ssts_.size();
                while (l < r) {
                    size_t mid = (l + r) >> 1;
                    if (ssts_[mid]->key() > key) {
                        r = mid;
                    } else {
                        l = mid + 1;
                    }
                }
                if (l > 0) {
                    return ssts_[l - 1]->Get(key, value);
                }
            }
            return false;
        }

        /// @brief 插入 SST 文件
        void Insert(std::shared_ptr<SST> sst) {
            ssts_.emplace_back(std::move(sst));
        }

        /// @brief 获取层编号
        size_t level() const { return level_; }

        /// @brief 获取该层 SST 文件数量
        size_t size() const { return ssts_.size(); }

        /// @brief 获取该层所有 SST 文件的总大小
        size_t binary_size() const {
            size_t res = 0;
            for (auto& sst_ptr : ssts_) {
                res += sst_ptr->binary_size();
            }
            return res;
        }

        /// @brief 获取 SST 文件列表的引用
        std::vector<std::shared_ptr<SST>>& ssts() { return ssts_; }

        /// @brief 获取 SST 文件列表的 const 引用
        const std::vector<std::shared_ptr<SST>>& ssts() const { return ssts_; }

    private:
        size_t level_;                                    ///< 层编号
        std::vector<std::shared_ptr<SST>> ssts_;         ///< 该层的 SST 文件列表
    };

public:
    /**
     * @brief 构造函数
     *
     * 如果 manifest 文件存在，则加载；否则创建新的。
     */
    explicit Manifest(std::filesystem::path data_dir = ".") : data_dir_(std::move(data_dir)) {
        if (data_dir_.empty()) {
            data_dir_ = ".";
        }
        std::error_code ec;
        std::filesystem::create_directories(data_dir_, ec);
        if (ec) {
            throw std::runtime_error("failed to create manifest directory: " + data_dir_.string());
        }

        const auto manifest_path = ManifestPath();
        auto fd = open(manifest_path.c_str(), O_RDWR);
        if (fd != -1) {
            struct stat stat_buf;
            fstat(fd, &stat_buf);
            auto file_size = stat_buf.st_size;
            auto data = static_cast<char*>(mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0));

            // 读取版本和层数
            size_t index = 0;
            version_ = *reinterpret_cast<size_t*>(data);
            index += sizeof(size_t);
            auto level_count = *reinterpret_cast<size_t*>(data + index);
            index += sizeof(size_t);

            // 加载每一层
            levels_.reserve(level_count);
            for (size_t i = 0; i < level_count; ++i) {
                Level level(i);
                index += level.Load(data + index, max_sst_id_, data_dir_);
                levels_.emplace_back(std::move(level));
            }
            close(fd);
        } else {
            // 文件不存在，创建新的 Manifest
            version_ = 1;
            levels_.emplace_back(Level(0));
        }
    }

    /**
     * @brief 序列化 Manifest 到文件
     * @return 写入的字节数
     */
    size_t Save() {
        const auto manifest_path = ManifestPath();
        auto fd = open(manifest_path.c_str(), O_RDWR);
        if (fd == -1) {
            fd = open(manifest_path.c_str(), O_RDWR | O_CREAT, 0700);
        }

        auto file_size = binary_size();
        if (ftruncate(fd, static_cast<off_t>(file_size)) == -1) {
            close(fd);
            return 0;
        }

        auto data = static_cast<char*>(mmap(nullptr, file_size, PROT_WRITE, MAP_SHARED, fd, 0));
        if (data == MAP_FAILED) {
            close(fd);
            return 0;
        }
        size_t index = 0;

        // 写入版本号
        *reinterpret_cast<size_t*>(data) = version_;
        index += sizeof(size_t);

        // 写入层数
        *reinterpret_cast<size_t*>(data + index) = levels_.size();
        index += sizeof(size_t);

        // 写入每一层
        for (auto& level : levels_) {
            index += level.Save(data + index);
        }

        munmap(data, file_size);
        close(fd);
        return index;
    }

    /// @brief 获取 Manifest 的二进制大小
    size_t binary_size() const {
        size_t res = 2 * sizeof(size_t);
        for (auto& level : levels_) {
            res += (level.size() + 1) * sizeof(size_t);  // +1 用于结束标记
        }
        return res;
    }

    /**
     * @brief 拷贝构造函数
     *
     * 用于 Copy-on-Write：创建新版本时复制旧版本
     */
    Manifest(const Manifest& manifest)
        : version_(manifest.version_ + 1),
          levels_(manifest.levels_),
          max_sst_id_(manifest.max_sst_id_),
          data_dir_(manifest.data_dir_) {}

    /**
     * @brief 查找键
     * @param key 要查找的键
     * @param value 输出参数，找到的值
     * @return 找到返回 true
     *
     * @section lookup_order 查询顺序
     *
     * 1. MemTable（活跃的内存表）
     * 2. 历史 MemTable（已满但未刷盘的）
     * 3. Manifest（按层查找）
     *
     * 注意：这里只处理 Manifest 部分，MemTable 由 DB 类管理
     */
    bool Get(std::string_view key, std::string& value) {
        mokv::common::RWLock::ReadLock r_lock(memtable_rw_lock_);
        ++count_;

        // 按层查找（从低层到高层，低层数据更新）
        for (size_t i = 0; i < levels_.size(); ++i) {
            if (levels_[i].Get(key, value)) {
                return true;
            }
        }
        return false;
    }

    /// @brief 在 Level 0 插入 SST 文件
    void Insert(std::shared_ptr<SST> sst) {
        levels_.begin()->Insert(sst);
    }

    /**
     * @brief 插入并返回新版本 Manifest
     * @param sst 要插入的 SST 文件
     * @return 新版本的 Manifest 指针
     *
     * 采用 Copy-on-Write 模式，不修改当前对象，而是创建新版本
     */
    std::shared_ptr<Manifest> InsertAndUpdate(std::shared_ptr<SST> sst) {
        if (sst->id() > max_sst_id_) {
            max_sst_id_ = sst->id();
        }
        auto res = std::make_shared<Manifest>(*this);  // 复制当前版本
        res->Insert(sst);
        return res;
    }

    /// @brief 获取最大 SST ID
    size_t max_sst_id() const { return max_sst_id_; }

    /// @brief 获取层列表（用于遍历）
    const std::vector<Level>& levels() const { return levels_; }

    /**
     * @brief Compaction 结构体 - 用于优先级队列
     *
     * 用于合并多个 SST 文件，保持有序性并去重。
     * 使用最小堆（priority_queue 默认是最大堆，这里反转比较）
     */
    struct SizeTieredCompactionStruct {
        SizeTieredCompactionStruct(SST& sst, size_t value)
            : it(sst.begin()), second_value(value) {}

        bool operator<(const SizeTieredCompactionStruct& rhs) const {
            auto& lhs_it = *const_cast<SizeTieredCompactionStruct*>(this)->it;
            auto& rhs_it = *const_cast<SizeTieredCompactionStruct&>(rhs).it;
            if (lhs_it.key == rhs_it.key) {
                return second_value > rhs.second_value;  // 值大的优先级低
            }
            return lhs_it.key > rhs_it.key;  // key 大的优先级低（最小堆）
        }

        SST::Iterator it;       ///< 当前迭代器
        size_t second_value;    ///< 第二排序键（用于处理相同 key）
    };

    /**
     * @brief 执行 Size-Tiered Compaction
     * @param level 要 Compaction 的层
     * @param id 新生成的 SST 文件 ID
     *
     * @section compaction_process Compaction 流程
     *
     * 1. 收集当前层和下一层需要合并的 SST 文件
     * 2. 使用最小堆合并所有键值对（保持有序）
     * 3. 去除重复 key（保留最新的值）
     * 4. 生成新的 SST 文件放到下一层
     * 5. 清理被合并的 SST 文件
     */
    void SizeTieredCompaction(size_t level, int id,
                              const CompressionConfig& compression = CompressionConfig()) {
        std::priority_queue<SizeTieredCompactionStruct> queue;
        size_t value = 0;
        std::string_view min_key, max_key;

        // 收集当前层的 SST 文件
        for (auto it = levels_[level].ssts().rbegin();
             it != levels_[level].ssts().rend(); ++it) {
            queue.emplace(**it, value++);

            // 记录 key 范围
            auto& begin_key = (*(*it)->begin()).key;
            auto& end_key = (*(*it)->rbegin()).key;

            if (min_key.empty() || begin_key < min_key) {
                min_key = begin_key;
            }
            if (max_key.empty() || end_key > max_key) {
                max_key = end_key;
            }
        }

        // 如果需要，创建新层
        if (level + 1 == levels_.size()) {
            levels_.emplace_back(Level(level + 1));
        }

        // 收集下一层需要合并的 SST 文件
        int insert_l = -1, insert_r = static_cast<int>(levels_[level + 1].ssts().size());
        for (size_t i = 0; i < levels_[level + 1].ssts().size(); ++i) {
            auto& sst_ptr = levels_[level + 1].ssts()[i];
            auto& sst_begin_key = (*sst_ptr->begin()).key;
            auto& sst_end_key = (*sst_ptr->rbegin()).key;

            if (sst_end_key < min_key) {
                insert_l = static_cast<int>(i);  // 插入位置左侧
            } else if (sst_begin_key > max_key) {
                insert_r = static_cast<int>(i);  // 插入位置右侧
                break;
            } else {
                queue.emplace(*sst_ptr, value++);  // 范围重叠，需要合并
            }
        }

        // 合并所有键值对
        std::vector<EntryView> entries;
        entries.reserve(1024);  // 预分配，减少扩容

        while (!queue.empty()) {
            auto data = queue.top();
            queue.pop();

            // 去重：只保留第一个出现的 key
            if (entries.empty() || entries.back().key != (*data.it).key) {
                entries.emplace_back((*data.it).key, (*data.it).value);
            }

            // 推进迭代器
            ++data.it;
            if (data.it) {
                queue.push(data);
            }
        }

        // 生成新的 SST 文件
        std::shared_ptr<SST> new_sst_ptr;
        if (compression.enable) {
            new_sst_ptr = std::make_shared<SST>(entries, id, compression, data_dir_);
        } else {
            new_sst_ptr = std::make_shared<SST>(entries, id, data_dir_);
        }
        if (static_cast<size_t>(id) > max_sst_id_) {
            max_sst_id_ = id;
        }

        // 重组下一层的 SST 文件
        std::vector<std::shared_ptr<SST>> new_ssts;
        new_ssts.reserve(levels_[level + 1].ssts().size() + 1);

        // 插入位置左侧的 SST
        for (int i = 0; i <= insert_l; ++i) {
            new_ssts.emplace_back(levels_[level + 1].ssts()[i]);
        }

        // 新的 SST
        new_ssts.emplace_back(new_sst_ptr);

        // 插入位置右侧的 SST
        for (int i = insert_r; i < static_cast<int>(levels_[level + 1].ssts().size()); ++i) {
            new_ssts.emplace_back(levels_[level + 1].ssts()[i]);
        }

        // 清理当前层
        levels_[level].ssts().clear();

        // 更新下一层
        levels_[level + 1].ssts() = std::move(new_ssts);
    }

    /**
     * @brief 检查是否需要 Compaction
     * @param id 用于新生成的 SST 文件
     *
     * 从 Level 0 开始检查，满足条件的层执行 Compaction
     */
    void SizeTieredCompaction(int id,
                              const CompressionConfig& compression = CompressionConfig()) {
        for (int i = 0; i < static_cast<int>(levels_.size()) && i < static_cast<int>(max_level_size()); ++i) {
            if (levels_[i].binary_size() > level_max_binary_size(i)) {
                SizeTieredCompaction(i, id, compression);
            } else {
                break;  // 低层不满足条件，高层也不会满足
            }
        }
    }

    /// @brief 检查 Level 0 是否可以执行 Compaction
    bool CanDoCompaction() const {
        return levels_[0].binary_size() > level_max_binary_size(0);
    }

private:
    std::filesystem::path ManifestPath() const {
        return data_dir_ / "manifest";
    }

    /// @brief 层数上限
    static constexpr size_t max_level_size() { return 5; }

    /// @brief 每层的最大二进制大小阈值（字节）
    static constexpr size_t level_max_binary_size(size_t level) {
        // Level 0: 1KB, Level 1: 10MB, Level 2: 100MB, ...
        static_assert(max_level_size() >= 5, "需要至少5层");
        constexpr size_t thresholds[] = {
            1024,                    // Level 0: 1 KB
            10 * 1024 * 1024,        // Level 1: 10 MB
            100 * 1024 * 1024,       // Level 2: 100 MB
            1000 * 1024 * 1024,      // Level 3: 1 GB
            10000LL * 1024 * 1024    // Level 4: 10 GB
        };
        return thresholds[level];
    }

private:
    std::atomic_size_t count_{0};           ///< 访问计数（用于调试）
    size_t version_{1};                     ///< Manifest 版本号
    std::vector<Level> levels_;             ///< 各层 SST 文件列表
    mokv::common::RWLock memtable_rw_lock_;  ///< 读写锁
    size_t max_sst_id_{0};                  ///< 最大 SST 文件 ID
    std::filesystem::path data_dir_ = ".";  ///< 数据目录
};

}  // namespace lsm
}  // namespace mokv
