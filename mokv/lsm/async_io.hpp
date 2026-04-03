/**
 * @file async_io.hpp
 * @brief 异步 I/O 实现 - io_uring 接口封装
 *
 * 使用 Linux io_uring 实现高性能异步 I/O：
 * - 零拷贝读取（使用 prep_read_fixed）
 * - 批量提交（减少系统调用开销）
 * - 事件驱动（使用 SQPOLL 减少用户态/内核态切换）
 *
 * @section io_uring_structure io_uring 结构
 * ┌─────────────────────────────────────────────────────────────────┐
 * │                        io_uring                                 │
 * ├─────────────────────────────────────────────────────────────────┤
 * │                                                                 │
 * │    ┌─────────────────┐         ┌─────────────────┐              │
 * │    │   Submission   │  <--->  │     Completion  │              │
 * │    │     Queue      │  ring   │      Queue      │              │
 │    │   (用户填写)     │         │   (内核填充)    │              │
 │    └─────────────────┘         └─────────────────┘              │
 │                                                                 │
 │    用户通过内存映射直接操作队列，避免系统调用                    │
 │                                                                 │
 └─────────────────────────────────────────────────────────────────┘
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <queue>

// 仅在 Linux 平台编译
#ifdef __linux__

#include <linux/fs.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <liburing.h>

namespace mokv {
namespace lsm {

/**
 * @brief 异步 I/O 提交项
 */
struct AsyncIORequest {
    enum Type { kRead, kWrite } type;
    int fd;                          ///< 文件描述符
    void* buf;                       ///< 缓冲区
    size_t size;                     ///< I/O 大小
    off_t offset;                    ///< 文件偏移量
    std::function<void(ssize_t)> callback;  ///< 完成回调
    void* user_data;                 ///< 用户数据

    AsyncIORequest(Type t, int f, void* b, size_t s, off_t o,
                   std::function<void(ssize_t)> cb = nullptr, void* ud = nullptr)
        : type(t), fd(f), buf(b), size(s), offset(o), callback(std::move(cb)), user_data(ud) {}
};

/**
 * @brief io_uring 异步 I/O 引擎
 *
 * 提供基于 io_uring 的异步文件 I/O 接口：
 * - 支持批量提交
 * - 支持完成回调
 * - 支持固定缓冲区（零拷贝）
 */
class IOEngine {
public:
    /**
     * @brief IO 配置
     */
    struct Config {
        /// @brief 提交队列大小
        size_t sq_size = 1024;

        /// @brief 完成队列大小
        size_t cq_size = 1024;

        /// @brief 是否使用 SQPOLL（内核线程轮询）
        bool use_sqpoll = true;

        /// @brief SQPOLL CPU
        int sqpoll_cpu = -1;

        /// @brief 批量提交阈值
        size_t batch_submission_threshold = 16;

        /// @brief 异步提交超时（毫秒）
        uint32_t submit_timeout_ms = 1000;
    };

    explicit IOEngine(const Config& config) : config_(config), ready_(false), last_error_(0) {}
    ~IOEngine() {}

    // 禁止拷贝
    IOEngine(const IOEngine&) = delete;
    IOEngine& operator=(const IOEngine&) = delete;

    /**
     * @brief 异步读取
     * @param fd 文件描述符
     * @param buf 目标缓冲区
     * @param size 读取大小
     * @param offset 文件偏移量
     * @param callback 完成回调（可选）
     * @param user_data 用户数据（可选）
     * @return 0 成功，-1 失败
     */
    int AsyncRead(int fd, void* buf, size_t size, off_t offset,
                  std::function<void(ssize_t)> callback = nullptr,
                  void* user_data = nullptr) {
        return SubmitRequest(AsyncIORequest::kRead, fd, buf, size, offset,
                            std::move(callback), user_data);
    }

    /**
     * @brief 异步写入
     * @param fd 文件描述符
     * @param buf 源缓冲区
     * @param size 写入大小
     * @param offset 文件偏移量
     * @param callback 完成回调（可选）
     * @param user_data 用户数据（可选）
     * @return 0 成功，-1 失败
     */
    int AsyncWrite(int fd, void* buf, size_t size, off_t offset,
                   std::function<void(ssize_t)> callback = nullptr,
                   void* user_data = nullptr) {
        return SubmitRequest(AsyncIORequest::kWrite, fd, buf, size, offset,
                            std::move(callback), user_data);
    }

    /**
     * @brief 异步读取固定缓冲区
     * @param fd 文件描述符
     * @param buf_index 缓冲区索引
     * @param size 读取大小
     * @param offset 文件偏移量
     * @param callback 完成回调（可选）
     * @return 0 成功，-1 失败
     */
    int AsyncReadFixed(int fd, size_t buf_index, size_t size, off_t offset,
                       std::function<void(ssize_t)> callback = nullptr);

    /**
     * @brief 注册固定缓冲区
     * @param index 缓冲区索引
     * @param buf 缓冲区指针
     * @param size 缓冲区大小
     * @return 0 成功，-1 失败
     */
    int RegisterBuffer(size_t index, void* buf, size_t size);

    /**
     * @brief 取消注册缓冲区
     * @param index 缓冲区索引
     * @return 0 成功，-1 失败
     */
    int UnregisterBuffer(size_t index);

    /**
     * @brief 提交所有挂起的请求
     * @return 提交的请求数
     */
    size_t SubmitPending();

    /**
     * @brief 等待并处理完成事件
     * @param min_complete 最小完成数（0 表示等待至少一个）
     * @param timeout_ms 超时时间（毫秒，0 表示无限等待）
     * @return 处理的完成事件数
     */
    size_t WaitComplete(size_t min_complete = 1, uint32_t timeout_ms = 0);

    /**
     * @brief 立即处理所有完成的事件
     * @return 处理的完成事件数
     */
    size_t PollComplete();

    /**
     * @brief 同步等待所有挂起请求完成
     */
    void Flush();

    /**
     * @brief 获取挂起的请求数
     */
    size_t PendingCount() const {
        return pending_count_.load(std::memory_order_relaxed);
    }

    /**
     * @brief 获取已完成的请求数
     */
    size_t CompletedCount() const {
        return completed_count_.load(std::memory_order_relaxed);
    }

    /// @brief 检查是否初始化成功
    bool IsReady() const { return ready_; }

    /// @brief 获取最后错误
    int GetLastError() const { return last_error_; }

private:
    struct io_uring ring_;
    Config config_;
    bool ready_{false};
    int last_error_{0};

    std::atomic_size_t pending_count_{0};
    std::atomic_size_t completed_count_{0};

    std::mutex mutex_;
    std::queue<AsyncIORequest> pending_requests_;

    // 提交队列状态
    unsigned sq_head_{0};
    unsigned sq_tail_{0};
    unsigned sq_mask_{0};
    struct io_uring_sqe* sq_entries_{nullptr};

    // 完成队列状态
    unsigned cq_head_{0};
    unsigned cq_tail_{0};
    unsigned cq_mask_{0};
    struct io_uring_cqe* cq_entries_{nullptr};

    // 缓冲区注册
    struct RegisteredBuffer {
        void* buf;
        size_t size;
    };
    std::vector<RegisteredBuffer> registered_buffers_;

    int SubmitRequest(AsyncIORequest::Type type, int fd, void* buf, size_t size,
                      off_t offset, std::function<void(ssize_t)> callback, void* user_data);

    int SubmitToRing(AsyncIORequest& request);

    void ProcessCompletion(struct io_uring_cqe* cqe);
};

/**
 * @brief 异步 I/O 缓冲区池
 *
 * 管理预分配的缓冲区，供 IOEngine 使用。
 */
class IOBufferPool {
public:
    IOBufferPool(size_t buffer_size = 64 * 1024, size_t buffer_count = 32)
        : buffer_size_(buffer_size), buffer_count_(buffer_count) {
        buffers_.reserve(buffer_count);
        for (size_t i = 0; i < buffer_count; ++i) {
            auto buf = std::aligned_alloc(4096, buffer_size);
            if (buf) {
                buffers_.push_back({buf, false});
            }
        }
    }

    ~IOBufferPool() {
        for (auto& buf : buffers_) {
            if (buf.ptr) {
                free(buf.ptr);
            }
        }
    }

    /// @brief 获取缓冲区
    void* Allocate() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (auto& buf : buffers_) {
            if (!buf.in_use) {
                buf.in_use = true;
                return buf.ptr;
            }
        }
        return nullptr;  // 没有可用缓冲区
    }

    /// @brief 释放缓冲区
    void Free(void* buf) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (auto& b : buffers_) {
            if (b.ptr == buf) {
                b.in_use = false;
                return;
            }
        }
    }

    /// @brief 获取缓冲区大小
    size_t buffer_size() const { return buffer_size_; }

    /// @brief 获取缓冲区数量
    size_t buffer_count() const { return buffer_count_; }

    /// @brief 获取可用缓冲区数量
    size_t available() const {
        std::unique_lock<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& buf : buffers_) {
            if (!buf.in_use) ++count;
        }
        return count;
    }

private:
    struct BufferInfo {
        void* ptr;
        bool in_use;
    };
    std::vector<BufferInfo> buffers_;
    size_t buffer_size_;
    size_t buffer_count_;
    mutable std::mutex mutex_;
};

/**
 * @brief 异步文件句柄
 *
 * 封装文件的异步 I/O 操作。
 */
class AsyncFile {
public:
    AsyncFile() : fd_(-1), io_engine_(nullptr) {}

    /**
     * @brief 打开文件（同步）
     */
    bool Open(const char* path, int flags = O_RDWR) {
        fd_ = open(path, flags, 0644);
        return fd_ != -1;
    }

    /**
     * @brief 异步读取
     */
    int AsyncRead(void* buf, size_t size, off_t offset,
                  std::function<void(ssize_t)> callback = nullptr) {
        if (!io_engine_ || fd_ == -1) return -1;
        return io_engine_->AsyncRead(fd_, buf, size, offset, std::move(callback));
    }

    /**
     * @brief 异步写入
     */
    int AsyncWrite(void* buf, size_t size, off_t offset,
                   std::function<void(ssize_t)> callback = nullptr) {
        if (!io_engine_ || fd_ == -1) return -1;
        return io_engine_->AsyncWrite(fd_, buf, size, offset, std::move(callback));
    }

    /// @brief 关闭文件
    void Close() {
        if (fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }
    }

    /// @brief 获取文件描述符
    int fd() const { return fd_; }

    /// @brief 设置 IO 引擎
    void SetIOEngine(IOEngine* engine) { io_engine_ = engine; }

    /// @brief 获取文件大小
    off_t Size() const {
        struct stat st;
        if (fstat(fd_, &st) == 0) {
            return st.st_size;
        }
        return -1;
    }

private:
    int fd_;
    IOEngine* io_engine_;
};

}  // namespace lsm
}  // namespace mokv

#else

// 非 Linux 平台提供空实现
namespace mokv {
namespace lsm {

class IOEngine {
public:
    struct Config {};
    explicit IOEngine(const Config&) : ready_(false) {}
    bool IsReady() const { return false; }
    int AsyncRead(int, void*, size_t, off_t, auto = nullptr) { return -1; }
    int AsyncWrite(int, void*, size_t, off_t, auto = nullptr) { return -1; }
    size_t SubmitPending() { return 0; }
    size_t WaitComplete(size_t = 1, uint32_t = 0) { return 0; }
    size_t PollComplete() { return 0; }
    void Flush() {}
};

class IOBufferPool {
public:
    IOBufferPool(size_t = 64 * 1024, size_t = 32) {}
    void* Allocate() { return nullptr; }
    void Free(void*) {}
};

class AsyncFile {
public:
    bool Open(const char*, int = O_RDWR) { return false; }
    int AsyncRead(int, void*, size_t, off_t, auto = nullptr) { return -1; }
    int AsyncWrite(int, void*, size_t, off_t, auto = nullptr) { return -1; }
    void Close() {}
    int fd() const { return -1; }
    off_t Size() const { return -1; }
};

}  // namespace lsm
}  // namespace mokv

#endif  // __linux__
