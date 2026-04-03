#include <atomic>
#include "mokv/utils/global_random.h"

namespace cpputil {

namespace common {

uint64_t GlobalRand() {
    // 简单的原子递增计数器
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

} // common
} // cpputil