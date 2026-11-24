/**
 * CircularBuffer.h - implements the shared PCM16 circular sample buffer.
 *
 * CONNECTIONS: This header-only class is used twice by SystemController: AudioDSP pushes recording and FileIO pops it; FileIO pushes playback and AudioDSP pops it. The mutex, condition variable, indexes and stop gate belong to each independent instance.
 *
 * FUNCTIONS:
 * - CircularBuffer(): connect the owned resources/references used by this class.
 * - begin_session(): open an empty audio buffer for the next recording or playback stream.
 * - push(): copy a whole produced block under the buffer mutex, then signal sample availability.
 * - pop(): wait for samples or stop/deadline, then copy and remove the actual available count.
 * - stop_producing(): close the producer gate under the buffer mutex while retaining queued samples.
 * - clear(): discard waiting channel data and reset its state under its owner's rules.
 * - isFull(): read whether every sample slot is occupied while holding the buffer mutex.
 * - isEmpty(): read whether no unread samples remain while holding the buffer mutex.
 * - size(): read the current item/sample count.
 * - available_capacity(): read the number of unused sample slots under the buffer mutex.
 * - stopped(): read the closed producer gate under the buffer mutex.
 */

// API DOCUMENTATION FOR THIS FILE:
// POSIX threads: creation, joining, mutexes and condition variables:
// https://man7.org/linux/man-pages/man7/pthreads.7.html
// pthread mutex lock, try-lock and unlock:
// https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
// Condition waits: mutex release/reacquisition and absolute timeouts:
// https://man7.org/linux/man-pages/man3/pthread_cond_timedwait.3p.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include <algorithm> // Include min/max, copying, sorting and other standard algorithms used in this file.
#include "phonostethos/RuntimeTypes.h" // Include the RuntimeTypes class/types used here; defines shared audio constants, runtime options, telemetry and archive identities.
#include <chrono> // Include typed durations and monotonic deadlines.

#include <cstddef> // Include size_t for sizes and array indexes.
#include "phonostethos/ThreadSupport.h" // Include the ThreadSupport class/types used here; provides the small pthread helpers shared by queues, buffers and managers.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// ================= SHARED AUDIO SAMPLE BUFFERS ================
// SystemController owns TWO separate instances of this same class:
// recording_buffer_: AudioDSP PUSHES processed PCM16 -> FileIO POPS for the WAV.
// playback_buffer_:  FileIO PUSHES WAV PCM16 -> AudioDSP POPS for headphones.
// Each instance has its OWN sample array, mutex, condition, indexes and flags.
// Control requests travel through mailboxes; this buffer carries audio samples.
class CircularBuffer { // Declare CircularBuffer and its related operations/state.

public: // The class operations callable by other code.
    // Normal buffers use 4096 samples. Tests can select a smaller working capacity.
    explicit CircularBuffer(size_t capacity = kCircularBufferSamples) : capacity_(capacity) { // Connect the owned resources/references used by this class.
        if (capacity == 0 || capacity > kCircularBufferSamples) { // Test the condition capacity == 0 || capacity > kCircularBufferSamples.
            std::fprintf(stderr, "Invalid circular buffer capacity\n"); // Print the diagnostic or test result for the operator.
            std::exit(EXIT_FAILURE); // Complete the call above with these remaining arguments.
        } // End this block, declaration or argument list.
        initializeMutex(mutex_); // Each buffer instance has its own lock.
        initializeCondition(data_available_); // Wake this buffer's waiting consumer.
    } // End this block, declaration or argument list.
    ~CircularBuffer() { // Finish the parameters/condition and enter the block below.
        pthread_cond_destroy(&data_available_); // Release this condition variable after its waiters have stopped.
        pthread_mutex_destroy(&mutex_); // Release this initialized mutex after its users have stopped.
    } // End this block, declaration or argument list.
    CircularBuffer(const CircularBuffer&) = delete; // Disallow copying this resource-owning object.
    CircularBuffer& operator=(const CircularBuffer&) = delete; // Disallow copying this resource-owning object.

    // FLOWCHART: AudioDSP pushes recording; FileIO pushes playback.
    // The gate and capacity check share the same mutex as the sample copy.
    bool push(const int16_t* data, size_t length) { // Copy a whole produced block under the buffer mutex, then signal sample availability.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        if (!accepting_ || length > capacity_ - count_) return false; // Accept the whole block or none.
        for (size_t i = 0; i < length; ++i) { // Visit the specified items/indexes in order.
            data_[head_] = data[i]; // Copy the next produced sample into the circular array.
            head_ = (head_ + 1) % capacity_; // Wrap back to array index 0 at the end.
        } // End this block, declaration or argument list.
        count_ += length; // These newly stored samples are now available to the consumer.
        /* API: pthread_cond_signal
         * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
         * Wake one thread waiting on this condition variable. Change the protected condition under its mutex first. The signal itself does not store data.
         */
        pthread_cond_signal(&data_available_); // Wake one waiter; the protected record or sample buffer stores the actual data.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    // FLOWCHART: FileIO pops recording; AudioDSP pops playback.
    // Return the actual sample count, which may be smaller than a full block.
    size_t pop(int16_t* destination, size_t maximum_length, // Pass this next argument/value to the call or initializer above.
               chrono::milliseconds timeout = chrono::milliseconds(10)) { // Initialize timeout to chrono::milliseconds(10)) {.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        const auto deadline = waitDeadline(timeout.count()); // Initialize deadline to waitDeadline(timeout.count()).
        // Waiting releases mutex_; it is locked again before the call returns.
        // Always recheck: a wake-up alone does not mean samples are available.
        while (count_ == 0 && !stopped_) { // Repeat this block while its condition remains true; recheck shared state on each pass.
            /* API: pthread_cond_timedwait
             * Documentation: https://man7.org/linux/man-pages/man3/pthread_cond_timedwait.3p.html
             * Sleep until signalled or until an absolute timespec deadline. The mutex must already be held. The call releases it while sleeping and reacquires it before returning. Recheck the predicate after waking.
             */
            const int result = pthread_cond_timedwait( // Release the held mutex while sleeping, then reacquire it before checking the predicate again.
                &data_available_, &mutex_, &deadline); // Complete the call above with these remaining arguments.
            if (result == ETIMEDOUT) break; // Test the condition result == ETIMEDOUT. If true, leave the current loop or switch branch.
            checkPthread(result); // Handle pthread error numbers directly instead of reading errno.
        } // End this block, declaration or argument list.
        const auto length = min(maximum_length, count_); // Take only what is present and fits the destination.
        for (size_t i = 0; i < length; ++i) { // Visit the specified items/indexes in order.
            destination[i] = data_[tail_]; // Copy the oldest stored sample to the consumer.
            tail_ = (tail_ + 1) % capacity_; // Advance/wrap the oldest-sample index.
        } // End this block, declaration or argument list.
        count_ -= length; // Those samples have been consumed; their array space is free.
        return length; // Unlock on return; caller can do slow disk/audio work outside this lock.
    } // End this block, declaration or argument list.

    // STATUS QUERIES: take the lock to read a consistent buffer state.
    bool isFull() const { // Read whether every sample slot is occupied while holding the buffer mutex.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        return count_ == capacity_; // Return count_ == capacity_ to the caller.
    } // End this block, declaration or argument list.

    bool isEmpty() const { // Read whether no unread samples remain while holding the buffer mutex.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        return count_ == 0; // Return count_ == 0 to the caller.
    } // End this block, declaration or argument list.

    // SESSION START: reopen an empty buffer for a new recording/playback stream.
    bool begin_session() { // Open an empty audio buffer for the next recording or playback stream.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        if (count_ != 0) return false; // Finish/discard the old session before opening a new one.
        head_ = 0; // Set head to 0.
        tail_ = 0; // Set tail to 0.
        accepting_ = true; // Set accepting to true.
        stopped_ = false; // Set stopped to false.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    // STOP/EOF: close the producer gate but keep already accepted samples.
    // The consumer can still pop those samples until the buffer becomes empty.
    void stop_producing() { // Close the producer gate under the buffer mutex while retaining queued samples.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        accepting_ = false; // Set accepting to false.
        stopped_ = true; // Set stopped to true.
        /* API: pthread_cond_broadcast
         * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
         * Wake all waiters so each can recheck the protected stop/data condition.
         */
        pthread_cond_broadcast(&data_available_); // Wake all waiters so each can recheck its protected data/stop predicate.
    } // End this block, declaration or argument list.

    // Discard remaining samples and close the gate (unlike draining after STOP).
    void clear() { // Discard waiting channel data and reset its state under its owner's rules.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        head_ = 0; // Set head to 0.
        tail_ = 0; // Set tail to 0.
        count_ = 0; // Set count to 0.
        accepting_ = false; // Set accepting to false.
        stopped_ = true; // Set stopped to true.
        pthread_cond_broadcast(&data_available_); // Wake all waiters so each can recheck its protected data/stop predicate.
    } // End this block, declaration or argument list.

    size_t size() const { // Read the current item/sample count.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        return count_; // Return count_ to the caller.
    } // End this block, declaration or argument list.

    size_t available_capacity() const { // Read the number of unused sample slots under the buffer mutex.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        return capacity_ - count_; // Return capacity_ - count_ to the caller.
    } // End this block, declaration or argument list.

    bool stopped() const { // Read the closed producer gate under the buffer mutex.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        return stopped_; // Return stopped_ to the caller.
    } // End this block, declaration or argument list.


private: // Internal helpers and state used by this class.
    // BUFFER STORAGE / INDEXES / GATE: every instance has an independent set.
    mutable pthread_mutex_t mutex_; // Declare mutex for this class instance.
    pthread_cond_t data_available_{}; // Consumer sleeps here when this buffer is empty.
    int16_t data_[kCircularBufferSamples]{}; // ACTUAL PCM16 SAMPLE STORAGE.
    const size_t capacity_; // Application capacity: 4096 samples per instance.
    size_t head_ = 0;       // Producer writes the next sample at this index.
    size_t tail_ = 0;       // Consumer takes the oldest sample from this index.
    size_t count_ = 0;      // Accepted samples currently waiting in this buffer.
    bool accepting_ = false; // Producer gate; checked with count_ under mutex_.
    bool stopped_ = true;    // Consumer may finish after remaining samples drain.
}; // End this block, declaration or argument list.
}  // namespace phono_app
