#pragma once

#include "phonostethos/RuntimeTypes.h"

#include "phonostethos/ThreadSupport.h"
#include <utility>

using namespace std;

namespace phono_app {
// One waiting visualization snapshot. Audio never waits for a slow GUI.
class DataQueue {
public:
    DataQueue() { initializeMutex(mutex_); }
    ~DataQueue() { pthread_mutex_destroy(&mutex_); }
    DataQueue(const DataQueue&) = delete;
    DataQueue& operator=(const DataQueue&) = delete;

    void set_connected(bool connected) {
        PthreadLock lock(mutex_);
        connected_ = connected;
        if (!connected_) has_packet_ = false;
    }

    bool try_enqueue(const Telemetry& packet) {
        /* API: pthread_mutex_trylock
         * Try once to take the mutex. A nonzero result means it was not acquired. Audio telemetry can be skipped instead of blocking audio.
         */
        if (pthread_mutex_trylock(&mutex_) != 0) return false;
        if (!connected_) {
            /* API: pthread_mutex_unlock
             * Release the mutex so another thread can use the shared object. The caller must own the mutex.
             */
            pthread_mutex_unlock(&mutex_);
            return false;
        }
        packet_ = packet;
        has_packet_ = true; // Mark packet as available.
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    bool try_dequeue(Telemetry& packet) {
        if (pthread_mutex_trylock(&mutex_) != 0) return false;
        bool available = has_packet_;
        if (available) {
            packet = packet_;
            has_packet_ = false; // Mark packet as empty.
        }
        pthread_mutex_unlock(&mutex_);
        return available;
    }

    void clear() {
        PthreadLock lock(mutex_);
        has_packet_ = false; // Mark packet as empty.
    }

private:
    pthread_mutex_t mutex_{};
    Telemetry packet_{};
    bool has_packet_ = false; // Mark packet as empty.
    bool connected_ = false;
};
}  // namespace phono_app
