#pragma once

#include "phonostethos/RuntimeTypes.h"

#include "phonostethos/ThreadSupport.h"
#include <optional>
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
        if (!connected_) packet_.reset();
    }

    bool try_enqueue(const Telemetry& packet) {
        if (pthread_mutex_trylock(&mutex_) != 0) return false;
        if (!connected_) {
            pthread_mutex_unlock(&mutex_);
            return false;
        }
        packet_ = packet;
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    optional<Telemetry> try_dequeue() {
        if (pthread_mutex_trylock(&mutex_) != 0) return nullopt;
        auto packet = move(packet_);
        packet_.reset();
        pthread_mutex_unlock(&mutex_);
        return packet;
    }

    void clear() {
        PthreadLock lock(mutex_);
        packet_.reset();
    }

private:
    pthread_mutex_t mutex_{};
    optional<Telemetry> packet_;
    bool connected_ = false;
};
}  // namespace phono_app
