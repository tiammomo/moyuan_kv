#include "mokv/lsm/async_sst_writer.hpp"

namespace mokv {
namespace lsm {

AsyncSSTWriter::AsyncSSTWriter(const Config& config)
    : config_(config) {
    // 初始化 IO 引擎
    io_engine_ = std::make_unique<IOEngine>(config_.io_config);

    // 初始化缓冲区池
    buffer_pool_ = std::make_unique<IOBufferPool>(config_.buffer_size, config_.buffer_count);

    // 启动写入线程
    writer_thread_ = std::thread(&AsyncSSTWriter::WriterThread, this);
}

AsyncSSTWriter::~AsyncSSTWriter() {
    running_ = false;
    write_cv_.notify_all();
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
}

void AsyncSSTWriter::WriteAsync(std::shared_ptr<MemTable> memtable, size_t sst_id,
                                 std::function<void(const AsyncWriteResult&)> callback) {
    WriteRequest request;
    request.memtable = std::move(memtable);
    request.sst_id = sst_id;
    request.callback = std::move(callback);
    request.start_time = std::chrono::high_resolution_clock::now().time_since_epoch().count();

    {
        std::unique_lock<std::mutex> lock(write_queue_mutex_);
        write_queue_.push(std::move(request));
    }
    write_cv_.notify_one();
}

std::shared_ptr<SST> AsyncSSTWriter::WriteSync(std::shared_ptr<MemTable> memtable, size_t sst_id) {
    auto start_time = std::chrono::high_resolution_clock::now();

    // 创建 SST
    std::shared_ptr<SST> sst;
    if (config_.enable_compression) {
        sst = std::make_shared<SST>(*memtable, static_cast<int>(sst_id), config_.compression);
    } else {
        sst = std::make_shared<SST>(*memtable, static_cast<int>(sst_id));
    }

    // 统计
    auto end_time = std::chrono::high_resolution_clock::now();
    auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    stats_.total_writes++;
    stats_.successful_writes++;
    stats_.total_bytes += sst->binary_size();
    stats_.total_latency_ns += latency_ns;

    return sst;
}

void AsyncSSTWriter::Flush() {
    std::unique_lock<std::mutex> lock(write_queue_mutex_);
    while (!write_queue_.empty()) {
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lock.lock();
    }
}

void AsyncSSTWriter::WriterThread() {
    while (running_) {
        WriteRequest request;

        {
            std::unique_lock<std::mutex> lock(write_queue_mutex_);
            write_cv_.wait(lock, [this] {
                return !running_ || !write_queue_.empty();
            });

            if (!running_ && write_queue_.empty()) {
                break;
            }

            request = std::move(write_queue_.front());
            write_queue_.pop();
        }

        // 执行写入
        AsyncWriteResult result;
        auto start_time = std::chrono::high_resolution_clock::now();

        try {
            result.sst = WriteSync(request.memtable, request.sst_id);
            result.success = result.sst != nullptr;
        } catch (const std::exception& e) {
            result.success = false;
            result.error_message = e.what();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

        // 统计
        stats_.total_writes++;
        if (result.success) {
            stats_.successful_writes++;
        } else {
            stats_.failed_writes++;
        }
        stats_.total_latency_ns += latency_ns;

        // 调用回调
        if (request.callback) {
            request.callback(result);
        }
    }
}

void BatchAsyncSSTWriter::CommitAsync(std::function<void(size_t, double)> callback) {
    if (batch_.empty()) {
        return;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    size_t success_count = 0;

    for (auto& [memtable, sst_id] : batch_) {
        auto sst = writer_.WriteSync(memtable, sst_id);
        if (sst) {
            ++success_count;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    Clear();

    if (callback) {
        callback(success_count, latency_ms / 1000.0);
    }
}

size_t BatchAsyncSSTWriter::CommitSync() {
    if (batch_.empty()) {
        return 0;
    }

    size_t success_count = 0;
    for (auto& [memtable, sst_id] : batch_) {
        auto sst = writer_.WriteSync(memtable, sst_id);
        if (sst) {
            ++success_count;
        }
    }

    Clear();
    return success_count;
}

}  // namespace lsm
}  // namespace mokv
