#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
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
    RaftLog(mokv::DB* db) {
        db_ = db;
        auto fd = open(log_name_, O_RDWR);
        if (fd != -1) {
            struct stat stat_buf;
            stat(log_name_, &stat_buf);
            auto file_size = 8;
            auto data = (char*)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
            Load(data);
            close(fd);
        } else {
            start_index_ = index_ = commited_ = last_append_ = 0;
        }
        sync_thread = std::thread([this]() {
            while (true) {
                sleep(3);
                // usleep(10000);
                std::unique_lock<std::mutex> lock(lock_);
                if (last_append_ < commited_) {
                    lock.unlock();
                    ++last_append_;
                    auto& entry = queue_.At(last_append_ - start_index_ - 1);
                    db_->Put(entry.key(), entry.value());
                } else {
                    lock.unlock();
                    if (stop_) {
                        break;
                    }
                }
            }
        });
    }
    ~RaftLog() {
        stop_ = true;
        if (sync_thread.joinable()) {
            sync_thread.join();
        }

        auto fd = open(log_name_, O_RDWR);
        if (fd == -1) {
            fd = open(log_name_, O_RDWR | O_CREAT, 0700);
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
        commited_ = std::max(commited_, std::min(index_.load(), leader_commit));
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
    mokv::DB* db_;
    std::mutex lock_;
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
