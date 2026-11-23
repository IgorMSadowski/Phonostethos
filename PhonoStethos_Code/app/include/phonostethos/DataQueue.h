/**
 * DataQueue.h - implements the latest waveform/FFT snapshot channel.
 *
 * CONNECTIONS: This header-only single-slot channel connects AudioDSP to Network. It is independent of both PCM audio buffers. Both sides try-lock; an old waiting snapshot may be replaced and a busy/disconnected publish may be skipped without waiting for the GUI.
 *
 * FUNCTIONS:
 * - DataQueue(): connect the owned resources/references used by this class.
 * - set_connected(): enable visualization only for the current client and discard stale disconnected data.
 * - enqueue(): store an item using this channel's locking and delivery policy.
 * - dequeue(): take an available item using this channel's receive policy.
 * - clear(): discard waiting channel data and reset its state under its owner's rules.
 */

#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include "phonostethos/RuntimeTypes.h" // Include the RuntimeTypes class/types used here; defines shared audio constants, runtime options, telemetry and archive identities.

#include "phonostethos/ThreadSupport.h" // Include the ThreadSupport class/types used here; provides the small pthread helpers shared by queues, buffers and managers.
#include <utility> // Include move owned payloads into queues without copying their contents.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// ================= QUEUE: AudioDSP -> Network =================
// DataQueue is the report's visualization channel, implemented as ONE latest slot.
// Storage: DataPacket packet_ + has_packet_ flag, protected by mutex_.
// AudioDSP may replace a waiting old snapshot so the GUI gets recent data.
// Both sides use trylock: if busy, they skip this attempt instead of waiting.
// This channel carries waveform/FFT display data, not recording/playback samples.
class DataQueue { // Declare DataQueue and its related operations/state.
public: // The class operations callable by other code.
    DataQueue() { initializeMutex(mutex_); } // Prepare protection for the one snapshot slot.
    ~DataQueue() { pthread_mutex_destroy(&mutex_); } // Workers have stopped before destruction.
    DataQueue(const DataQueue&) = delete; // Disallow copying this resource-owning object.
    DataQueue& operator=(const DataQueue&) = delete; // Disallow copying this resource-owning object.

    // SEND FROM AudioDSP: copy the new snapshot; an older waiting one is replaced.
    // false means it was not stored (mutex busy or no connected GUI).
    bool enqueue(const DataPacket& packet) { // Store an item using this channel's locking and delivery policy.
        /* API: pthread_mutex_trylock
         * Try once to take the mutex. A nonzero result means it was not acquired. Audio telemetry can be skipped instead of blocking audio.
         */
        if (pthread_mutex_trylock(&mutex_) != 0) return false; // Busy: skip this attempt without waiting.
        if (!connected_) { // Test the condition !connected_.
            /* API: pthread_mutex_unlock
             * Release the mutex so another thread can use the shared object. The caller must own the mutex.
             */
            pthread_mutex_unlock(&mutex_); // Release the slot before returning to audio/network work.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        packet_ = packet; // Copy the new waveform/FFT, replacing an older waiting snapshot.
        has_packet_ = true; // Mark packet as available.
        pthread_mutex_unlock(&mutex_); // Release the slot before returning to audio/network work.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    // TAKE IN Network: copy the waiting snapshot and mark the slot empty.
    // No condition wait: Network checks this channel during its normal loop.
    bool dequeue(DataPacket& packet) { // Take an available item using this channel's receive policy.
        if (pthread_mutex_trylock(&mutex_) != 0) return false; // Busy: skip this attempt without waiting.
        bool available = has_packet_; // Save whether Network found a waiting snapshot.
        if (available) { // Test the condition available.
            packet = packet_; // Give Network its own copy to serialize/send after unlock.
            has_packet_ = false; // Mark packet as empty.
        } // End this block, declaration or argument list.
        pthread_mutex_unlock(&mutex_); // Release the slot before returning to audio/network work.
        return available; // true = a copy was taken; false = the slot was empty.
    } // End this block, declaration or argument list.

    // CONNECTION STATE: Network enables/disables publication for its current client.
    void set_connected(bool connected) { // Enable visualization only for the current client and discard stale disconnected data.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        connected_ = connected; // Network tells this channel whether it has a GUI client.
        if (!connected_) has_packet_ = false; // Discard the previous client's waiting display data.
    } // End this block, declaration or argument list.

    // RESET: remove a pending display snapshot without touching audio samples.
    void clear() { // Discard waiting channel data and reset its state under its owner's rules.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        has_packet_ = false; // Mark packet as empty.
    } // End this block, declaration or argument list.


private: // Internal helpers and state used by this class.
    // SINGLE-SLOT STORAGE AND ITS STATE (protected by the same mutex).
    pthread_mutex_t mutex_{}; // Protect the waiting packet, occupied flag and connection state.
    DataPacket packet_{};          // ACTUAL SNAPSHOT STORAGE: waveform + spectrum.
    bool has_packet_ = false;     // true = Network has a waiting snapshot to take.
    bool connected_ = false; // Publish visualization only while Network has a GUI client.
}; // End this block, declaration or argument list.
}  // namespace phono_app
