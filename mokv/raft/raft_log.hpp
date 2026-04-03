#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>


#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "mokv/db.hpp"
#include "mokv/resource_manager.hpp"
#include "mokv/utils/ring_buffer_queue.hpp"
#include "mokv/raft/protos/raft.grpc.pb.h"
namespace mokv {
namespace raft {
class SnapShot {

};

class RaftLog {
public:
    RaftLog(mokv::DB* db, std::filesystem::path data_dir = ".")
        : log_path_((data_dir.empty() ? std::filesystem::path(".") : std::move(data_dir)) / log_name_) {
        db_ = db;
        std::error_code ec;
        std::filesystem::create_directories(log_path_.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("failed to create raft log directory: " + log_path_.parent_path().string());
        }

        auto fd = open(log_path_.c_str(), O_RDWR);
        if (fd != -1) {
            struct stat stat_buf;
            stat(log_path_.c_str(), &stat_buf);
            auto file_size = 8;
            auto data = (char*)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
            Load(data);
            close(fd);
        } else {
            start_index_ = index_ = commited_ = last_append_ = 0;
        }
        sync_thread = std::thread([this]() {
            while (true) {
                std::unique_lock<std::mutex> lock(lock_);
                sync_cv_.wait(lock, [this]() {
                    return stop_ || last_append_ < commited_;
                });
                if (stop_ && last_append_ >= commited_) {
                    break;
                }
                Entry entry = queue_.At(last_append_ - start_index_);
                ++last_append_;
                lock.unlock();
                db_->Put(entry.key(), entry.value());
                sync_cv_.notify_all();
            }
        });
    }
    ~RaftLog() {
        {
            std::unique_lock<std::mutex> guard(lock_);
            stop_ = true;
        }
        sync_cv_.notify_all();
        if (sync_thread.joinable()) {
            sync_thread.join();
        }

        auto fd = open(log_path_.c_str(), O_RDWR);
        if (fd == -1) {
            fd = open(log_path_.c_str(), O_RDWR | O_CREAT, 0700);
        }
        auto file_size = 8;
        if (ftruncate(fd, static_cast<off_t>(file_size)) == -1) {
            close(fd);
            return;
        }

        auto data = (char*)mmap(NULL, file_size, PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return;
        }
        Save(data);
        munmap(data, file_size);
        close(fd);
    }
    size_t index() {
        return index_;
    }
    size_t commited() {
        return commited_;
    }
    void PopFront() {
        queue_.PopFront();
    }
    Entry& At(size_t index) {
        return queue_.RAt(index_ - index);
    }

    bool Put(const std::string& key, const std::string& value, int64_t term, size_t& idx) {
        std::unique_lock<std::mutex> guard(lock_);
        if (stop_) {
            return false;
        }
        Entry entry;
        entry.set_index(index_ + 1);
        entry.set_key(key);
        entry.set_value(value);
        entry.set_mode(0);
        entry.set_term(term);
        if (queue_.PushBack(entry)) {
            ++index_;
            idx = index_;
            if (entry.commited() > commited_) {
                commited_ = entry.commited();
                sync_cv_.notify_all();
            }
            return true;
        } else {
            return false;
        }
    }

    bool Put(::mokv::raft::Entry& entry) {
        std::unique_lock<std::mutex> guard(lock_);
        if (stop_) {
            return false;
        }
        if (queue_.PushBack(entry)) {
            ++index_;
            if (entry.commited() > commited_) {
                commited_ = entry.commited();
                sync_cv_.notify_all();
            }
            return true;
        } else {
            return false;
        }
    }

    // 优化：批量删除，避免循环开销
    void Reset(size_t expect_index) {
        std::unique_lock<std::mutex> guard(lock_);
        if (index_ > expect_index) {
            size_t count = index_ - expect_index;
            size_t removed = queue_.Truncate(count);
            index_ -= removed;
        }
    }

    // bool Check(Entry& entry, size_t index) {
    //     if (index > index_) {
    //         return false;
    //     }
    //     return queue_.RAt(index_ - index) == entry;
    // }

    void UpdateCommit(size_t leader_commit) {
        std::unique_lock<std::mutex> lock(lock_);
        const size_t previous_commit = commited_;
        commited_ = std::max(commited_, std::min(index_.load(), leader_commit));
        if (commited_ > previous_commit) {
            sync_cv_.notify_all();
        }
    }

    void WaitUntilApplied(size_t index) {
        std::unique_lock<std::mutex> lock(lock_);
        sync_cv_.wait(lock, [this, index]() {
            return stop_ || last_append_ >= index;
        });
    }
private:
    void Load(char* s) {
        start_index_ = index_ = commited_ = last_append_ = *reinterpret_cast<size_t*>(s);
    }
    void Save(char* s) {
        *reinterpret_cast<size_t*>(s) = commited_;
    }
private:   
    constexpr static const char* log_name_ = "raft_log_meta";
    std::filesystem::path log_path_;
    mokv::DB* db_;
    std::mutex lock_;
    std::condition_variable sync_cv_;
    std::thread sync_thread;
    std::atomic_bool stop_{false};
    std::atomic_size_t index_;
    cpputil::pbds::RingBufferQueue<Entry> queue_;
    size_t commited_;
    size_t last_append_;
    size_t start_index_;
};
 
}
}
