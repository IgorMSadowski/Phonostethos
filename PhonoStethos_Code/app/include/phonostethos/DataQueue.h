#pragma once

#include "phonostethos/RuntimeTypes.h"

#include "phonostethos/ThreadSupport.h"
#include <utility>

using namespace std;

namespace phono_app {
// ================= QUEUE: AudioDSP -> Network =================
// DataQueue is the report's visualization channel, implemented as ONE latest slot.
// Storage: Telemetry packet_ + has_packet_ flag, protected by mutex_.
// AudioDSP may replace a waiting old snapshot so the GUI gets recent data.
// Both sides use trylock: if busy, they skip this attempt instead of waiting.
// This channel carries waveform/FFT display data, not recording/playback samples.
class DataQueue {
public:
    DataQueue() { initializeMutex(mutex_); } // Prepare protection for the one snapshot slot.
    ~DataQueue() { pthread_mutex_destroy(&mutex_); } // Workers have stopped before destruction.
    DataQueue(const DataQueue&) = delete;
    DataQueue& operator=(const DataQueue&) = delete;

    // CONNECTION STATE: Network enables/disables publication for its current client.
    void set_connected(bool connected) {
        PthreadLock lock(mutex_);
        connected_ = connected; // Network tells this channel whether it has a GUI client.
        if (!connected_) has_packet_ = false; // Discard the previous client's waiting display data.
    }

    // SEND FROM AudioDSP: copy the new snapshot; an older waiting one is replaced.
    // false means it was not stored (mutex busy or no connected GUI).
    bool try_enqueue(const Telemetry& packet) {
        /* API: pthread_mutex_trylock
         * Try once to take the mutex. A nonzero result means it was not acquired. Audio telemetry can be skipped instead of blocking audio.
         */
        if (pthread_mutex_trylock(&mutex_) != 0) return false; // Busy: skip this attempt without waiting.
        if (!connected_) {
            /* API: pthread_mutex_unlock
             * Release the mutex so another thread can use the shared object. The caller must own the mutex.
             */
            pthread_mutex_unlock(&mutex_); // Release the slot before returning to audio/network work.
            return false;
        }
        packet_ = packet; // Copy the new waveform/FFT, replacing an older waiting snapshot.
        has_packet_ = true; // Mark packet as available.
        pthread_mutex_unlock(&mutex_); // Release the slot before returning to audio/network work.
        return true;
    }

    // TAKE IN Network: copy the waiting snapshot and mark the slot empty.
    // No condition wait: Network checks this channel during its normal loop.
    bool try_dequeue(Telemetry& packet) {
        if (pthread_mutex_trylock(&mutex_) != 0) return false; // Busy: skip this attempt without waiting.
        bool available = has_packet_; // Save whether Network found a waiting snapshot.
        if (available) {
            packet = packet_; // Give Network its own copy to serialize/send after unlock.
            has_packet_ = false; // Mark packet as empty.
        }
        pthread_mutex_unlock(&mutex_); // Release the slot before returning to audio/network work.
        return available; // true = a copy was taken; false = the slot was empty.
    }

    // RESET: remove a pending display snapshot without touching audio samples.
    void clear() {
        PthreadLock lock(mutex_);
        has_packet_ = false; // Mark packet as empty.
    }

private:
    // SINGLE-SLOT STORAGE AND ITS STATE (protected by the same mutex).
    pthread_mutex_t mutex_{}; // Protect the waiting packet, occupied flag and connection state.
    Telemetry packet_{};          // ACTUAL SNAPSHOT STORAGE: waveform + spectrum.
    bool has_packet_ = false;     // true = Network has a waiting snapshot to take.
    bool connected_ = false; // Publish visualization only while Network has a GUI client.
};
}  // namespace phono_app
