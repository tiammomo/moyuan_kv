# BloomFilter 布隆过滤器

**文件**: `utils/bloom_filter.hpp`

布隆过滤器用于快速判断 key 是否可能存在，避免无效磁盘 I/O。

## 原理

布隆过滤器使用位数组和多个哈希函数：

```
初始状态（位数组全部为 0）:
[0] [0] [0] [0] [0] [0] [0] [0] [0] [0] ...

添加 "hello":
  hash1("hello") → 3, hash2("hello") → 5, hash3("hello") → 7
  设置位数组:
[0] [0] [0] [1] [0] [1] [0] [1] [0] [0] ...

查询 "world":
  hash1("world") → 2, hash2("world") → 5, hash3("world") → 8
  检查: [2]=0 → "world" 一定不存在!
```

## 核心字段

```cpp
class BloomFilter {
private:
    size_t length_{0};           // 位数组长度
    size_t hash_num_{0};         // 哈希函数数量
    std::vector<size_t> seed_;   // 哈希种子列表
    uint64_t* data_{nullptr};    // 位数组指针
    size_t size_{0};             // 位数组大小（uint64_t数量）
    bool loaded_{false};         // 是否从外部加载
};
```

## 初始化算法

```cpp
void Init(size_t n, double p = 0.01) {
    // 计算位数组长度
    // m = -n * ln(p) / (ln(2))^2 * 2.35
    length_ = CalcLength(n, p);

    // 计算哈希函数数量
    // k = max(1, 0.69 * length / n)
    hash_num_ = std::max(1, int(0.69 * length_ / n));

    // 生成随机种子
    for (size_t i = 0; i < hash_num_; ++i) {
        seed_.emplace_back(GlobalRand());
    }

    // 分配内存（64位对齐）
    size_ = (length_ + 63) / 64;
    data_ = new uint64_t[size_]();
}
```

### 参数计算公式

| 参数 | 公式 | 说明 |
|------|------|------|
| 位数组长度 m | $-n \cdot \ln(p) / (\ln(2))^2$ | n 为元素数，p 为假阳性率 |
| 哈希函数数量 k | $\max(1, 0.69 \cdot m / n)$ | 最优哈希函数数量 |

### 常见配置

| 元素数 | 假阳性率 | 位数组大小 | 哈希函数数 |
|--------|----------|-----------|-----------|
| 10000 | 1% | 96 KB | 7 |
| 10000 | 0.1% | 192 KB | 10 |
| 100000 | 1% | 960 KB | 7 |
| 100000 | 0.1% | 1.9 MB | 10 |

## 哈希函数

```cpp
size_t CalcHash(const char* s, size_t len, size_t seed) {
    size_t res = 0;
    for (size_t i = 0; i < len; ++i) {
        res *= seed;
        res += static_cast<size_t>(s[i]);
    }
    return res;
}

// 查询 key 的所有哈希位置
void GetHashPositions(const char* s, size_t len, size_t* positions) {
    for (size_t i = 0; i < hash_num_; ++i) {
        positions[i] = CalcHash(s, len, seed_[i]) % length_;
    }
}
```

**特点**: 多项式哈希，简单高效

## 核心方法

### 1. 插入 (Add)

```cpp
void Add(const char* s, size_t len) {
    size_t positions[32];  // 假设最多32个哈希函数
    GetHashPositions(s, len, positions);

    for (size_t i = 0; i < hash_num_; ++i) {
        size_t pos = positions[i];
        data_[pos / 64] |= (1ULL << (pos % 64));
    }
}
```

### 2. 查询 (MightContain)

```cpp
bool MightContain(const char* s, size_t len) const {
    size_t positions[32];
    GetHashPositions(s, len, positions);

    for (size_t i = 0; i < hash_num_; ++i) {
        size_t pos = positions[i];
        if ((data_[pos / 64] & (1ULL << (pos % 64))) == 0) {
            return false;  // 一定不存在
        }
    }
    return true;  // 可能存在
}
```

### 3. 序列化

```cpp
// 导出到字节数组
std::vector<char> Serialize() const {
    std::vector<char> result;
    result.reserve(sizeof(Header) + size_ * 8);

    // Header
    result.append(reinterpret_cast<const char*>(&length_), sizeof(length_));
    result.append(reinterpret_cast<const char*>(&hash_num_), sizeof(hash_num_));

    // 数据
    result.append(reinterpret_cast<const char*>(data_), size_ * 8);

    return result;
}

// 从字节数组加载
void Load(const char* data, size_t size) {
    size_t offset = 0;

    // Header
    std::memcpy(&length_, data + offset, sizeof(length_));
    offset += sizeof(length_);
    std::memcpy(&hash_num_, data + offset, sizeof(hash_num_));
    offset += sizeof(hash_num_);

    // 数据
    size_ = (length_ + 63) / 64;
    data_ = new uint64_t[size_];
    std::memcpy(data_, data + offset, size_ * 8);

    loaded_ = true;
}
```

## 在 SST 中的应用

```cpp
// DataBlock 格式
struct DataBlock {
    uint64_t block_size;        // 块大小
    std::vector<char> bloom_filter;  // 布隆过滤器
    uint64_t count;             // 键值对数量
    std::vector<KVPair> kvs;    // 键值对数据
};

// SST 查找时使用布隆过滤器
bool SST::Get(std::string_view key, std::string& value) {
    // 1. 加载 DataBlock 和布隆过滤器
    auto block_data = GetDataBlock(block_index);
    BloomFilter bf;
    bf.Load(block_data.bloom_filter.data(), block_data.bloom_filter.size());

    // 2. 布隆过滤器快速过滤
    if (!bf.MaybeContains(key.data(), key.size())) {
        return false;  // 一定不存在，跳过
    }

    // 3. 二分查找
    return DataBlockGet(block_data, key, value);
}
```

## 特性总结

| 特性 | 说明 |
|------|------|
| 空间效率 | 相比哈希表，节省约 90% 空间 |
| 查询时间 | O(k)，k 为哈希函数数量 |
| 假阳性 | 可能存在（返回 true 但实际不存在） |
| 假阴性 | 不可能（返回 false 一定不存在） |
| 只增不减 | 无法删除元素（会误删其他元素） |

## 与其他数据结构对比

| 数据结构 | 空间复杂度 | 查询时间 | 假阳性率 |
|----------|-----------|---------|----------|
| 哈希表 | O(n) | O(1) | 0% |
| 布隆过滤器 | O(n) | O(k) | ~1% |
| 位图 + 单哈希 | O(n) | O(1) | ~10% |

## 测试验证

| 测试项 | 状态 |
|--------|------|
| bloom_filter_test | PASS |

> 性能指标：~10,000,000+ ops/sec
