#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

using namespace std;

namespace phono_app {
template <typename T, size_t Capacity>
class SynchronizedQueue {
    static_assert(Capacity > 0, "Queue capacity must be positive");

public:
    bool push(T value, const atomic<bool>* cancelled = nullptr) {
        unique_lock<mutex> lock(mutex_);
        space_available_.wait(lock, [&] {
            return closed_ || queue_.size() < Capacity ||
                   (cancelled != nullptr && cancelled->load());
        });
        if (closed_ || (cancelled != nullptr && cancelled->load())) {
            return false;
        }
        queue_.push_back(move(value));
        data_available_.notify_one();
        return true;
    }

    bool try_pop(T& value) {
        lock_guard<mutex> lock(mutex_);
        if (queue_.empty()) return false;
        value = move(queue_.front());
        queue_.pop_front();
        space_available_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool wait_pop_for(T& value, const chrono::duration<Rep, Period>& wait,
                      const atomic<bool>* cancelled = nullptr) {
        unique_lock<mutex> lock(mutex_);
        data_available_.wait_for(lock, wait, [&] {
            return closed_ || !queue_.empty() ||
                   (cancelled != nullptr && cancelled->load());
        });
        if (queue_.empty()) return false;
        value = move(queue_.front());
        queue_.pop_front();
        space_available_.notify_one();
        return true;
    }

    void clear() {
        lock_guard<mutex> lock(mutex_);
        queue_.clear();
        space_available_.notify_all();
    }

    void notify_all() {
        data_available_.notify_all();
        space_available_.notify_all();
    }

    void close() {
        {
            lock_guard<mutex> lock(mutex_);
            closed_ = true;
        }
        notify_all();
    }

private:
    mutex mutex_;
    condition_variable data_available_;
    condition_variable space_available_;
    deque<T> queue_;
    bool closed_ = false;
};
}  // namespace phono_app
