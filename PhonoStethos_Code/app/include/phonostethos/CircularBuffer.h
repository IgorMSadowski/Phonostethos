#pragma once

#include <algorithm>
#include "phonostethos/RuntimeTypes.h"
#include <chrono>

#include <cstddef>
#include "phonostethos/ThreadSupport.h"

using namespace std;

namespace phono_app {
// Two independent objects use this class: recordingBuffer and playbackBuffer.
class CircularBuffer {

public:
    // Normal buffers use 4096 samples. Tests can select a smaller working capacity.
    explicit CircularBuffer(size_t capacity = kCircularBufferSamples) : capacity_(capacity) {
        if (capacity == 0 || capacity > kCircularBufferSamples) {
            std::fprintf(stderr, "Invalid circular buffer capacity\n");
            std::exit(EXIT_FAILURE);
        }
        initializeMutex(mutex_);
        initializeCondition(data_available_);
    }
    ~CircularBuffer() {
        pthread_cond_destroy(&data_available_);
        pthread_mutex_destroy(&mutex_);
    }
    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

    bool begin_session() {
        PthreadLock lock(mutex_);
        if (count_ != 0) return false;
        head_ = 0;
        tail_ = 0;
        accepting_ = true;
        stopped_ = false;
        return true;
    }

    // FLOWCHART: AudioDSP pushes recording; FileIO pushes playback.
    // The gate and capacity check share the same mutex as the sample copy.
    bool push(const int16_t* data, size_t length) {
        PthreadLock lock(mutex_);
        if (!accepting_ || length > capacity_ - count_) return false;
        for (size_t i = 0; i < length; ++i) {
            data_[head_] = data[i];
            head_ = (head_ + 1) % capacity_;
        }
        count_ += length;
        /* API: pthread_cond_signal
         * Wake one thread waiting on this condition variable. Change the protected condition under its mutex first. The signal itself does not store data.
         */
        pthread_cond_signal(&data_available_);
        return true;
    }

    // FLOWCHART: FileIO pops recording; AudioDSP pops playback.
    // Return the actual sample count, which may be smaller than a full block.
    size_t pop(int16_t* destination, size_t maximum_length,
               chrono::milliseconds timeout = chrono::milliseconds(10)) {
        PthreadLock lock(mutex_);
        const auto deadline = waitDeadline(timeout.count());
        // Waiting releases mutex_; it is locked again before the call returns.
        // Always recheck: a wake-up alone does not mean samples are available.
        while (count_ == 0 && !stopped_) {
            /* API: pthread_cond_timedwait
             * Sleep until signalled or until an absolute timespec deadline. The mutex must already be held. The call releases it while sleeping and reacquires it before returning. Recheck the predicate after waking.
             */
            const int result = pthread_cond_timedwait(
                &data_available_, &mutex_, &deadline);
            if (result == ETIMEDOUT) break;
            checkPthread(result);
        }
        const auto length = min(maximum_length, count_);
        for (size_t i = 0; i < length; ++i) {
            destination[i] = data_[tail_];
            tail_ = (tail_ + 1) % capacity_;
        }
        count_ -= length;
        return length;
    }

    void stop_producing() {
        PthreadLock lock(mutex_);
        accepting_ = false;
        stopped_ = true;
        /* API: pthread_cond_broadcast
         * Wake all waiters so each can recheck the protected stop/data condition.
         */
        pthread_cond_broadcast(&data_available_);
    }

    void clear() {
        PthreadLock lock(mutex_);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        accepting_ = false;
        stopped_ = true;
        pthread_cond_broadcast(&data_available_);
    }

    bool isFull() const {
        PthreadLock lock(mutex_);
        return count_ == capacity_;
    }

    bool isEmpty() const {
        PthreadLock lock(mutex_);
        return count_ == 0;
    }

    size_t size() const {
        PthreadLock lock(mutex_);
        return count_;
    }

    size_t available_capacity() const {
        PthreadLock lock(mutex_);
        return capacity_ - count_;
    }

    bool stopped() const {
        PthreadLock lock(mutex_);
        return stopped_;
    }

private:
    mutable pthread_mutex_t mutex_;
    pthread_cond_t data_available_{};
    int16_t data_[kCircularBufferSamples]{};
    const size_t capacity_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    bool accepting_ = false;
    bool stopped_ = true;
};
}  // namespace phono_app
