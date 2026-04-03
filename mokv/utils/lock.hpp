#pragma once
#include <pthread.h>

namespace mokv {
namespace common {

class RWLock {
public:
    RWLock() {
        pthread_rwlock_init(&lock_, nullptr);
    }

    ~RWLock() {
        pthread_rwlock_destroy(&lock_);
    }

    pthread_rwlock_t& Get() {
        return lock_;
    }

    class ReadLock {
    public:
        ReadLock(ReadLock&& rhs) {
            if (this == &rhs) {
                return;
            }
            lock_ = rhs.lock_;
            rhs.lock_ = nullptr;
        }
        ReadLock& operator = (ReadLock&& rhs) {
            if (this == &rhs) {
                return *this;
            }
            lock_ = rhs.lock_;
            rhs.lock_ = nullptr;
            return *this;
        }
        ReadLock(RWLock& lock) {
            lock_ = &lock.Get();
            pthread_rwlock_rdlock(lock_);
        }
        ~ReadLock() {
            if (!lock_) {
                return;
            }
            pthread_rwlock_unlock(lock_);
        }
    private:
        pthread_rwlock_t* lock_ = nullptr;
    };

    class WriteLock {
    public:
        WriteLock(WriteLock&& rhs) {
            if (this == &rhs) {
                return;
            }
            lock_ = rhs.lock_;
            rhs.lock_ = nullptr;
        }
        WriteLock& operator = (WriteLock&& rhs) {
            if (this == &rhs) {
                return *this;
            }
            lock_ = rhs.lock_;
            rhs.lock_ = nullptr;
            return *this;
        }
        WriteLock(RWLock& lock) {
            lock_ = &lock.Get();
            pthread_rwlock_wrlock(lock_);
        }
        ~WriteLock() {
            if (!lock_) {
                return;
            }
            pthread_rwlock_unlock(lock_);
        }
    private:
        pthread_rwlock_t* lock_ = nullptr;
    };

    
private:
    pthread_rwlock_t lock_;

};

}
}