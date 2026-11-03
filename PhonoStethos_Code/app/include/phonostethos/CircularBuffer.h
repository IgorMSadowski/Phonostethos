#pragma once

#include <array>
#include <cstddef>

using namespace std;

namespace phono_app {
template <typename T, size_t Capacity>
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

    size_t size() const { return size_; }

    void copy_ordered(array<T, Capacity>& output) const {
        const auto first = size_ == Capacity ? next_ : 0;
        for (size_t i = 0; i < size_; ++i) {
            output[i] = data_[(first + i) % Capacity];
        }
        for (size_t i = size_; i < Capacity; ++i) {
            output[i] = T{};
        }
    }

private:
    array<T, Capacity> data_{};
    size_t next_ = 0;
    size_t size_ = 0;
};
}  // namespace phono_app
