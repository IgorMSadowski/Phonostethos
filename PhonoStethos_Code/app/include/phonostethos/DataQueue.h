#pragma once

#include "phonostethos/RuntimeTypes.h"

#include <mutex>
#include <optional>
#include <utility>

using namespace std;

namespace phono_app {
class DataQueue {
public:
    void set_connected(bool connected) {
        lock_guard<mutex> lock(mutex_);
        connected_ = connected;
        if (!connected_) packet_.reset();
    }

    bool try_enqueue(const Telemetry& packet) {
        unique_lock<mutex> lock(mutex_, try_to_lock);
        if (!lock.owns_lock() || !connected_) return false;
        packet_ = packet;
        return true;
    }

    optional<Telemetry> try_dequeue() {
        unique_lock<mutex> lock(mutex_, try_to_lock);
        if (!lock.owns_lock() || !packet_.has_value()) return nullopt;
        auto packet = move(packet_);
        packet_.reset();
        return packet;
    }

    void clear() {
        lock_guard<mutex> lock(mutex_);
        packet_.reset();
    }

private:
    mutex mutex_;
    optional<Telemetry> packet_;
    bool connected_ = false;
};
}  // namespace phono_app
