#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

using namespace std;

namespace phono_app {
template <typename T, size_t Capacity>
class CircularBuffer {
    static_assert(Capacity > 0, "CircularBuffer capacity must be positive");

public:
    bool begin_session() {
        lock_guard<mutex> lock(mutex_);
        if (count_ != 0) return false;
        head_ = 0;
        tail_ = 0;
        accepting_ = true;
        stopped_ = false;
        return true;
    }

    bool push(const T* data, size_t length) {
        lock_guard<mutex> lock(mutex_);
        if (!accepting_ || length > Capacity - count_) return false;
        for (size_t i = 0; i < length; ++i) {
            data_[head_] = data[i];
            head_ = (head_ + 1) % Capacity;
        }
        count_ += length;
        data_available_.notify_one();
        return true;
    }

    size_t pop(T* destination, size_t maximum_length,
               chrono::milliseconds timeout = chrono::milliseconds(10)) {
        unique_lock<mutex> lock(mutex_);
        data_available_.wait_for(lock, timeout, [&] {
            return count_ != 0 || stopped_;
        });
        const auto length = min(maximum_length, count_);
        for (size_t i = 0; i < length; ++i) {
            destination[i] = data_[tail_];
            tail_ = (tail_ + 1) % Capacity;
        }
        count_ -= length;
        return length;
    }

    void stop_producing() {
        lock_guard<mutex> lock(mutex_);
        accepting_ = false;
        stopped_ = true;
        data_available_.notify_all();
    }

    void clear() {
        lock_guard<mutex> lock(mutex_);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        accepting_ = false;
        stopped_ = true;
        data_available_.notify_all();
    }

    bool isFull() const {
        lock_guard<mutex> lock(mutex_);
        return count_ == Capacity;
    }

    bool isEmpty() const {
        lock_guard<mutex> lock(mutex_);
        return count_ == 0;
    }

    size_t size() const {
        lock_guard<mutex> lock(mutex_);
        return count_;
    }

    size_t available_capacity() const {
        lock_guard<mutex> lock(mutex_);
        return Capacity - count_;
    }

    bool stopped() const {
        lock_guard<mutex> lock(mutex_);
        return stopped_;
    }

private:
    mutable mutex mutex_;
    condition_variable data_available_;
    array<T, Capacity> data_{};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    bool accepting_ = false;
    bool stopped_ = true;
};
}  // namespace phono_app
