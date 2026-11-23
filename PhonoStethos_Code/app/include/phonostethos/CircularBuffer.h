#pragma once

#include <algorithm>
#include "phonostethos/RuntimeTypes.h"
#include <chrono>

#include <cstddef>
#include "phonostethos/ThreadSupport.h"

using namespace std;

namespace phono_app {
// ================= SHARED AUDIO SAMPLE BUFFERS ================
// SystemController owns TWO separate instances of this same class:
// recording_buffer_: AudioDSP PUSHES processed PCM16 -> FileIO POPS for the WAV.
// playback_buffer_:  FileIO PUSHES WAV PCM16 -> AudioDSP POPS for headphones.
// Each instance has its OWN sample array, mutex, condition, indexes and flags.
// Control requests travel through mailboxes; this buffer carries audio samples.
class CircularBuffer {

public:
    // Normal buffers use 4096 samples. Tests can select a smaller working capacity.
    explicit CircularBuffer(size_t capacity = kCircularBufferSamples) : capacity_(capacity) {
        if (capacity == 0 || capacity > kCircularBufferSamples) {
            std::fprintf(stderr, "Invalid circular buffer capacity\n");
            std::exit(EXIT_FAILURE);
        }
        initializeMutex(mutex_); // Each buffer instance has its own lock.
        initializeCondition(data_available_); // Wake this buffer's waiting consumer.
    }
    ~CircularBuffer() {
        pthread_cond_destroy(&data_available_);
        pthread_mutex_destroy(&mutex_);
    }
    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;

    // SESSION START: reopen an empty buffer for a new recording/playback stream.
    bool begin_session() {
        PthreadLock lock(mutex_);
        if (count_ != 0) return false; // Finish/discard the old session before opening a new one.
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
        if (!accepting_ || length > capacity_ - count_) return false; // Accept the whole block or none.
        for (size_t i = 0; i < length; ++i) {
            data_[head_] = data[i]; // Copy the next produced sample into the circular array.
            head_ = (head_ + 1) % capacity_; // Wrap back to array index 0 at the end.
        }
        count_ += length; // These newly stored samples are now available to the consumer.
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
        const auto length = min(maximum_length, count_); // Take only what is present and fits the destination.
        for (size_t i = 0; i < length; ++i) {
            destination[i] = data_[tail_]; // Copy the oldest stored sample to the consumer.
            tail_ = (tail_ + 1) % capacity_; // Advance/wrap the oldest-sample index.
        }
        count_ -= length; // Those samples have been consumed; their array space is free.
        return length; // Unlock on return; caller can do slow disk/audio work outside this lock.
    }

    // STOP/EOF: close the producer gate but keep already accepted samples.
    // The consumer can still pop those samples until the buffer becomes empty.
    void stop_producing() {
        PthreadLock lock(mutex_);
        accepting_ = false;
        stopped_ = true;
        /* API: pthread_cond_broadcast
         * Wake all waiters so each can recheck the protected stop/data condition.
         */
        pthread_cond_broadcast(&data_available_);
    }

    // Discard remaining samples and close the gate (unlike draining after STOP).
    void clear() {
        PthreadLock lock(mutex_);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        accepting_ = false;
        stopped_ = true;
        pthread_cond_broadcast(&data_available_);
    }

    // STATUS QUERIES: take the lock to read a consistent buffer state.
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
    // BUFFER STORAGE / INDEXES / GATE: every instance has an independent set.
    mutable pthread_mutex_t mutex_;
    pthread_cond_t data_available_{}; // Consumer sleeps here when this buffer is empty.
    int16_t data_[kCircularBufferSamples]{}; // ACTUAL PCM16 SAMPLE STORAGE.
    const size_t capacity_; // Application capacity: 4096 samples per instance.
    size_t head_ = 0;       // Producer writes the next sample at this index.
    size_t tail_ = 0;       // Consumer takes the oldest sample from this index.
    size_t count_ = 0;      // Accepted samples currently waiting in this buffer.
    bool accepting_ = false; // Producer gate; checked with count_ under mutex_.
    bool stopped_ = true;    // Consumer may finish after remaining samples drain.
};
}  // namespace phono_app
