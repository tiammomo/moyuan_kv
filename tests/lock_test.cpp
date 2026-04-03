#include <gtest/gtest.h>
#include <iostream>

#include "mokv/utils/lock.hpp"

TEST(Lock, Function) {
    mokv::common::RWLock lock;
    {
        mokv::common::RWLock::ReadLock r_lock_1(lock);
        mokv::common::RWLock::ReadLock r_lock_2(lock);
    }

    {
        // mokv::common::RWLock::ReadLock r_lock_1(lock);
        mokv::common::RWLock::WriteLock w_lock(lock);
    }
}