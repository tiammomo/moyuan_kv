#include <cstddef>
#include <array>

namespace cpputil {
namespace pbds {

template <typename T>
class RingBufferQueue {
public:
    bool PushBack(T&& rhs) {
        if (((head_ + 1) & ring_buffer_size_musk_) == tail_) {
            return false;
        }
        data_[++head_] = std::move(rhs);
        return true;
    }

    bool PushBack(const T& rhs) {
        if (((head_ + 1) & ring_buffer_size_musk_) == tail_) {
            return false;
        }
        data_[++head_] = rhs;
        return true;
    }

    bool Empty() {
        return head_ == tail_;
    }

    bool PopFront() {
        if (tail_ == head_) {
            return false;
        }
        ++tail_;
        return true;
    }

    bool PopBack() {
        if (tail_ == head_) {
            return false;
        }
        --head_;
        return true;
    }

    /// @brief 批量删除尾部元素（优化版本）
    /// @param count 要删除的数量
    /// @return 实际删除的数量
    size_t Truncate(size_t count) {
        if (tail_ == head_) {
            return 0;
        }
        size_t max_count = head_ - tail_;
        count = std::min(count, max_count);
        head_ -= count;
        return count;
    }

    /// @brief 获取元素数量
    size_t Size() const {
        return head_ - tail_;
    }

    T& Back() {
        return data_[head_ + 1];
    }

    T& Front() {
        return data_[tail_ + 1];
    }

    T& At(size_t index) {
        return data_[tail_ + index + 1];
    }

    T& RAt(size_t index) {
        return data_[head_ - index];
    }
private:
    constexpr static const size_t ring_buffer_size_ = (1 << 10) << 8;
    constexpr static const size_t ring_buffer_size_musk_ = ring_buffer_size_ - 1;
    std::array<T, ring_buffer_size_> data_;
    size_t head_ = 0;
    size_t tail_ = 0;
}; 

}
}