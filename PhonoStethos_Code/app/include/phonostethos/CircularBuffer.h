#pragma once

#include <array>
#include <cstddef>

namespace phono_app {
template <typename T, std::size_t Capacity>
class CircularBuffer {
    static_assert(Capacity > 0, "CircularBuffer capacity must be positive");

public:
    void clear() {
        data_.fill(T{});
        next_ = 0;
        size_ = 0;
    }

    void push(const T& value) {
        data_[next_] = value;
        next_ = (next_ + 1) % Capacity;
        if (size_ < Capacity) ++size_;
    }

    std::size_t size() const { return size_; }

    void copy_ordered(std::array<T, Capacity>& output) const {
        const auto first = size_ == Capacity ? next_ : 0;
        for (std::size_t i = 0; i < size_; ++i) {
            output[i] = data_[(first + i) % Capacity];
        }
        for (std::size_t i = size_; i < Capacity; ++i) {
            output[i] = T{};
        }
    }

private:
    std::array<T, Capacity> data_{};
    std::size_t next_ = 0;
    std::size_t size_ = 0;
};
}  // namespace phono_app
