#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

#include "mokv/utils/global_random.h"

namespace cpputil {

namespace cache {

namespace utils {

// T must support (^ and &) uint64_t, such as int
template <typename T, size_t Shard = 2, typename Allocator = std::allocator<uint8_t>>
class CMSketch4Bits { // count 0~15 = (1 << 4)
    struct PassBy { // use & for > 16 bit size object
        using type = typename std::conditional<(sizeof(T) > (sizeof(void*) << 1)) || !std::is_trivially_copyable_v<T>, T&, T>::type;
    };

public:
    explicit CMSketch4Bits(size_t capacity_bit): capacity_(1 << capacity_bit), capacity_mask_(capacity_ - 1) {
        for (auto& data_ptr : data_) {
            data_ptr = allocator_.allocate(capacity_ >> 1);
            memset(data_ptr, 0, (capacity_ >> 1));
        }
        for (auto& seed : seed_) {
            seed = cpputil::common::GlobalRand();
        }
    }
    ~CMSketch4Bits() {
        for (auto data_ptr : data_) {
            allocator_.deallocate(data_ptr, capacity_ >> 1);
        }
    }

    void Increment(const typename PassBy::type item) {
        for (size_t i = 0; i < Shard; i++) {
            auto index = (item ^ seed_[i]) & capacity_mask_;
            auto offset = (index & 1) << 2; // 0 or 4
            if (((data_[i][index >> 1] >> offset) & 0x0f) < 15) {
                data_[i][index >> 1] += 1 << offset;
            }
        }
    }
    unsigned int Estimate(const typename PassBy::type item) {
        int res = 15;
        for (size_t i = 0; i < Shard; i++) {
            auto index = (item ^ seed_[i]) & capacity_mask_;
            res = std::min(res, data_[i][index >> 1] >> ((index & 1) << 2));
        }
        return res;
    }
    void Reset() {
        const size_t data_size = capacity_ >> 1;
        for (auto data_ptr : data_) {
            for (size_t i = 0; i < data_size; i++) {
                data_ptr[i] = (data_ptr[i] >> 1) & 0x77; // 0 1 1 1 0 1 1 1
            }
        }
    }
private:
    uint8_t* data_[Shard];
    uint64_t seed_[Shard];
    const size_t capacity_;
    const size_t capacity_mask_;
    Allocator allocator_;
};

} // utils
} // cache 
} // cpputil