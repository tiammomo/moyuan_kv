# 压缩系统

**对应文件**：

- `mokv/utils/compression.hpp`
- `mokv/lsm/sst.hpp`

## 当前目标

当前压缩系统负责两件事：

1. 为 SST DataBlock entries 提供可选压缩
2. 保证压缩块在重载后仍能被正确识别和解压

这轮重构之后，压缩路径已经从“只会写，不稳定读”变成了“写入、重载、外部 LZ4 互通都可测”。

## 压缩类型

```cpp
enum class CompressionType {
    kNone = 0,
    kSnappy = 1,
    kLZ4 = 2
};
```

## 压缩器接口

当前统一接口是：

```cpp
class Compressor {
public:
    virtual CompressionType type() const = 0;
    virtual CompressedData Compress(const char* data, size_t size) = 0;
    virtual size_t Decompress(const CompressedData& compressed_data,
                              char* output,
                              size_t output_size) = 0;
    virtual size_t GetDecompressedSize(const char* compressed_data,
                                       size_t compressed_size) = 0;
    virtual size_t MaxCompressedSize(size_t original_size) = 0;
};
```

当前接口里和压缩大小相关的方法是：

- `GetDecompressedSize(...)`
- `MaxCompressedSize(...)`

## `CompressedData`

当前 `CompressedData` 只保存：

- 压缩字节数组
- 原始大小

压缩类型不放在 `CompressedData` 里，而是由调用方通过压缩器类型和 SST block 头来决定。

## LZ4：当前是标准 `liblz4` frame

`LZ4Compressor` 现在使用的是：

- `LZ4F_compressFrameBound`
- `LZ4F_compressFrame`
- `LZ4F_decompress`
- `LZ4F_getFrameInfo`

这意味着：

- 输出是标准 LZ4 frame
- 可以被外部 `liblz4` / `lz4` CLI 识别
- 项目也能读取外部标准 LZ4 frame

## Snappy：当前是仓内实现

`SnappyCompressor` 当前不是依赖系统 `libsnappy`，而是仓内兼容实现。

因此：

- 对项目内部压缩路径是可用的
- 但文档不应该把它写成“直接依赖外部 snappy 官方库”

## SST 中的压缩块格式

真正让读路径稳定下来的关键，不只是压缩器本身，而是 `sst.hpp` 中的自描述 block 头。

当前 encoded block 头至少包含：

- `magic`
- `version`
- `flags`
- `compression_type`
- `entries_size`

BloomFilter 保持未压缩，entries 部分根据收益决定是否压缩。

## 压缩何时生效

控制入口是 `CompressionConfig`：

```cpp
struct CompressionConfig {
    common::CompressionType type = common::CompressionType::kLZ4;
    bool enable = true;
    size_t min_size_for_compression = 64;
};
```

只有在下面两个条件同时满足时，block 才会进入压缩尝试：

1. `enable == true`
2. `original_size >= min_size_for_compression`

而且即使允许压缩，如果压缩后的结果不更小，也会保留未压缩形式。

## 默认值

当前默认 DB 配置里，压缩是开启的。

这包括：

- 正常 MemTable -> SST 刷盘
- Manifest compaction 生成新 SST

## 外部互通测试

`tests/compression_test.cpp` 当前覆盖了这几件事：

- DB 默认启用压缩
- 压缩 SST 落盘后可重载
- DB 重启后仍能读取压缩数据
- 项目生成的 LZ4 payload 能被外部 `liblz4` 解压
- 外部 `liblz4` 生成的 LZ4 frame 能被项目读取

截至 `2026-04-03`，这些测试已经通过。
