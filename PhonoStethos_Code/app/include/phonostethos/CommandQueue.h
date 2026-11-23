/**
 * CommandQueue.h - defines MainMessage and the FIFO consumed by th_Main.
 *
 * CONNECTIONS: This header-only queue joins Network GUI commands and FileIO/AudioDSP results. SystemController owns it and gives each producer the same reference. The mutex protects records; the condition variable wakes Main but stores no message itself.
 *
 * FUNCTIONS:
 * - CommandQueue(): connect the owned resources/references used by this class.
 * - enqueue(): store an item using this channel's locking and delivery policy.
 * - dequeue(): take an available item using this channel's receive policy.
 * - close(): close.
 * - notify_all(): notify all.
 * - closed(): closed.
 */

#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include "phonostethos/RuntimeTypes.h" // Include the RuntimeTypes class/types used here; defines shared audio constants, runtime options, telemetry and archive identities.
#include "phonostethos/protocol.h" // Include the protocol class/types used here; defines and validates the client/server binary protocol.

#include <chrono> // Include typed durations and monotonic deadlines.

#include <deque> // Include FIFO containers used for queued messages or replies.
#include "phonostethos/ThreadSupport.h" // Include the ThreadSupport class/types used here; provides the small pthread helpers shared by queues, buffers and managers.
#include <vector> // Include variable-size byte frames and archive entry lists.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
enum class MessageKind : uint8_t { // Give the following named choices distinct typed values.
    GUI_COMMAND, // Name this field, choice or argument: GUI COMMAND.
    WORKER_RESULT, // Name this field, choice or argument: WORKER RESULT.
    WORKER_ERROR, // Name this field, choice or argument: WORKER ERROR.
}; // End this block, declaration or argument list.

enum class WorkerResultCode : uint8_t { // Give the following named choices distinct typed values.
    NETWORK_STARTED, // Name this field, choice or argument: Network Started.
    NETWORK_FAILED, // Name this field, choice or argument: Network Failed.
    CLIENT_CONNECTED, // Name this field, choice or argument: Client Connected.
    CLIENT_DISCONNECTED, // Name this field, choice or argument: Client Disconnected.
    AUDIO_READY, // Name this field, choice or argument: Audio Ready.
    FILEIO_READY, // Name this field, choice or argument: File IOReady.
    STORAGE_CHANGED, // Name this field, choice or argument: Storage Changed.
    CAPTURE_SUSPENDED, // Name this field, choice or argument: Capture Suspended.
    CAPTURE_RESUMED, // Name this field, choice or argument: Capture Resumed.
    PLAYBACK_STARTED, // Name this field, choice or argument: Playback Started.
    PLAYBACK_STOPPED, // Name this field, choice or argument: Playback Stopped.
    OPEN_READY, // Name this field, choice or argument: Recording Opened.
    RECORD_CLOSED, // Name this field, choice or argument: Recording Closed.
    PLAYBACK_READY, // Name this field, choice or argument: Playback Ready.
    PLAYBACK_FINISHED, // Name this field, choice or argument: Playback Finished.
    PLAYBACK_CLOSED, // Name this field, choice or argument: Playback Closed.
    ARCHIVE_READY, // Name this field, choice or argument: Archive Ready.
    RECORDING_DELETED, // Name this field, choice or argument: Recording Deleted.
    STORAGE_EJECTED, // Name this field, choice or argument: Storage Ejected.
}; // End this block, declaration or argument list.

// One queue item. kind says which fields Main should read:
// GUI_COMMAND -> command; WORKER_RESULT/WORKER_ERROR -> result/error/operation_id.
// operation_id lets Main match a delayed worker result to its pending request.
struct MainMessage { // Group the related fields in one plain data record.
    MessageKind kind = MessageKind::GUI_COMMAND; // Selects command/result/error interpretation.
    phono::protocol::CommandPacket command{}; // GUI command number and argument.
    uint32_t operation_id = 0; // Matches a worker reply to Main's pending operation.
    WorkerResultCode result_code = WorkerResultCode::AUDIO_READY; // Which worker event occurred.
    uint8_t error_code = 0; // Zero means no error; nonzero identifies the failure.
    uint8_t storage_flags = 0; // Worker-reported storage capabilities.
    vector<ArchiveIdentity> archive; // File identities returned by an archive request.
}; // End this block, declaration or argument list.

// ================= QUEUE: workers/Network -> Main =============
// Actual mechanism: std::deque<MainMessage> + pthread mutex + condition variable.
// Multiple producers enqueue at the BACK; Main alone dequeues from the FRONT.
// Network adds GUI commands and connection events; FileIO/AudioDSP add results.
// available_ wakes Main when an item arrives; queue_ stores the actual message.
// Main later calls processMessage() to decide whether it is a command or result.
class CommandQueue { // Declare CommandQueue and its related operations/state.
public: // The class operations callable by other code.
    // LIFETIME: initialize synchronization once, then release it after all users stop.
    CommandQueue() { // Connect the owned resources/references used by this class.
        initializeMutex(mutex_); // Prepare the lock shared by all senders and Main.
        initializeCondition(available_); // Prepare the wake-up condition used by Main.
    } // End this block, declaration or argument list.
    ~CommandQueue() { // Finish the parameters/condition and enter the block below.
        pthread_cond_destroy(&available_); // Release the condition after its users have stopped.
        pthread_mutex_destroy(&mutex_); // Release the now-unused queue lock.
    } // End this block, declaration or argument list.
    CommandQueue(const CommandQueue&) = delete; // Disallow copying this resource-owning object.
    CommandQueue& operator=(const CommandQueue&) = delete; // Disallow copying this resource-owning object.

    // SEND GUI COMMAND: wrap the decoded TCP command as a MainMessage first.
    bool enqueue(phono::protocol::CommandPacket command) { // Store an item using this channel's locking and delivery policy.
        MainMessage message; // Create an internal queue item around the TCP command.
        message.kind = MessageKind::GUI_COMMAND; // Main must interpret the command fields.
        message.command = command; // Copy the command number and its argument.
        return enqueue(move(message)); // Use the common locked send path below.
    } // End this block, declaration or argument list.

    // SEND RESULT/COMMAND: keep the locked part short; Main handles it later.
    bool enqueue(MainMessage message) { // Store an item using this channel's locking and delivery policy.
        { // Begin this local scope; scoped locks release when it ends.
            PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
            if (closed_) return false; // A closed queue accepts no new work.
            // Add at the back. move transfers the owned payload into the queue.
            queue_.push_back(move(message)); // Append the owned item at the back of this container.
        } // End this block, declaration or argument list.
        /* API: pthread_cond_signal
         * Wake one thread waiting on this condition variable. Change the protected condition under its mutex first. The signal itself does not store data.
         */
        pthread_cond_signal(&available_); // Wake one waiter; the protected record or sample buffer stores the actual data.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    // RECEIVE IN MAIN: wait up to timeout if empty, then take the oldest item.
    // On return, PthreadLock has released mutex_; Main executes handlers unlocked.
    bool dequeue(MainMessage& message, // Pass this next argument/value to the call or initializer above.
                 chrono::milliseconds timeout = chrono::milliseconds(10)) { // Initialize timeout to chrono::milliseconds(10)) {.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        const auto deadline = waitDeadline(timeout.count()); // One absolute deadline for this receive.
        while (!closed_ && queue_.empty()) { // Sleep only while there is no item and no close request.
            /* API: pthread_cond_timedwait
             * Sleep until signalled or until an absolute timespec deadline. The mutex must already be held. The call releases it while sleeping and reacquires it before returning. Recheck the predicate after waking.
             */
            const int result = pthread_cond_timedwait( // Release the held mutex while sleeping, then reacquire it before checking the predicate again.
                &available_, &mutex_, &deadline); // Complete the call above with these remaining arguments.
            if (result == ETIMEDOUT) return false; // No item taken; Main can check other work.
            checkPthread(result); // Handle unexpected pthread errors before using shared data.
        } // End this block, declaration or argument list.
        if (queue_.empty()) return false; // Closed and empty: there is no message to give Main.
        message = move(queue_.front()); // Give the oldest message to the caller.
        queue_.pop_front();             // Remove that queue item, exactly once.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    // SHUTDOWN / STATUS: wake Main so it can finish when this channel closes.
    void close() { // Finish the parameters/condition and enter the block below.
        { // Begin this local scope; scoped locks release when it ends.
            PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
            closed_ = true; // Publish shutdown while holding the same queue mutex.
        } // End this block, declaration or argument list.
        /* API: pthread_cond_broadcast
         * Wake all waiters so each can recheck the protected stop/data condition.
         */
        pthread_cond_broadcast(&available_); // Wake all waiters so each can recheck its protected data/stop predicate.
    } // End this block, declaration or argument list.

    void notify_all() { pthread_cond_broadcast(&available_); } // Wake all waiters so each can recheck its protected data/stop predicate.

    bool closed() const { // Continue the expression or parameter list from the preceding line.
        PthreadLock lock(mutex_); // Lock this shared object until the local scope ends, including an early return.
        return closed_; // Return closed_ to the caller.
    } // End this block, declaration or argument list.

private: // Internal helpers and state used by this class.
    // FIFO STORAGE AND SYNCHRONIZATION (all access goes through the methods above).
    mutable pthread_mutex_t mutex_; // Guards queue_ and closed_.
    pthread_cond_t available_{};     // Lets Main sleep until work/close/deadline.
    deque<MainMessage> queue_;       // ACTUAL MESSAGE STORAGE, in FIFO order.
    bool closed_ = false; // Reject new messages and let waiting receivers finish.
}; // End this block, declaration or argument list.
}  // namespace phono_app
