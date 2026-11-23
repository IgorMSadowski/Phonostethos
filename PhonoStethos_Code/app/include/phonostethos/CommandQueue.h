#pragma once

#include "phonostethos/RuntimeTypes.h"
#include "phonostethos/protocol.h"

#include <chrono>

#include <deque>
#include "phonostethos/ThreadSupport.h"
#include <vector>

using namespace std;

namespace phono_app {
enum class MessageKind : uint8_t {
    GuiCommand,
    WorkerResult,
    WorkerError,
};

enum class WorkerResultCode : uint8_t {
    NetworkStarted,
    NetworkFailed,
    ClientConnected,
    ClientDisconnected,
    AudioReady,
    FileIOReady,
    StorageChanged,
    CaptureSuspended,
    CaptureResumed,
    PlaybackStarted,
    PlaybackStopped,
    RecordingOpened,
    RecordingClosed,
    PlaybackReady,
    PlaybackFinished,
    PlaybackClosed,
    ArchiveReady,
    RecordingDeleted,
    StorageEjected,
};

// One queue item. kind says which fields Main should read:
// GuiCommand -> command; WorkerResult/WorkerError -> result/error/operation_id.
// operation_id lets Main match a delayed worker result to its pending request.
struct MainMessage {
    MessageKind kind = MessageKind::GuiCommand; // Selects command/result/error interpretation.
    phono::protocol::CommandPacket command{}; // GUI command number and argument.
    uint32_t operation_id = 0; // Matches a worker reply to Main's pending operation.
    WorkerResultCode result_code = WorkerResultCode::AudioReady; // Which worker event occurred.
    uint8_t error_code = 0; // Zero means no error; nonzero identifies the failure.
    uint8_t storage_flags = 0; // Worker-reported storage capabilities.
    vector<ArchiveIdentity> archive; // File identities returned by an archive request.
};

// ================= QUEUE: workers/Network -> Main =============
// Actual mechanism: std::deque<MainMessage> + pthread mutex + condition variable.
// Multiple producers enqueue at the BACK; Main alone dequeues from the FRONT.
// Network adds GUI commands and connection events; FileIO/AudioDSP add results.
// available_ wakes Main when an item arrives; queue_ stores the actual message.
// Main later calls processMessage() to decide whether it is a command or result.
class CommandQueue {
public:
    // LIFETIME: initialize synchronization once, then release it after all users stop.
    CommandQueue() {
        initializeMutex(mutex_); // Prepare the lock shared by all senders and Main.
        initializeCondition(available_); // Prepare the wake-up condition used by Main.
    }
    ~CommandQueue() {
        pthread_cond_destroy(&available_); // Release the condition after its users have stopped.
        pthread_mutex_destroy(&mutex_); // Release the now-unused queue lock.
    }
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    // SEND GUI COMMAND: wrap the decoded TCP command as a MainMessage first.
    bool enqueue(phono::protocol::CommandPacket command) {
        MainMessage message; // Create an internal queue item around the TCP command.
        message.kind = MessageKind::GuiCommand; // Main must interpret the command fields.
        message.command = command; // Copy the command number and its argument.
        return enqueue(move(message)); // Use the common locked send path below.
    }

    // SEND RESULT/COMMAND: keep the locked part short; Main handles it later.
    bool enqueue(MainMessage message) {
        {
            PthreadLock lock(mutex_);
            if (closed_) return false; // A closed queue accepts no new work.
            // Add at the back. move transfers the owned payload into the queue.
            queue_.push_back(move(message));
        }
        /* API: pthread_cond_signal
         * Wake one thread waiting on this condition variable. Change the protected condition under its mutex first. The signal itself does not store data.
         */
        pthread_cond_signal(&available_);
        return true;
    }

    // RECEIVE IN MAIN: wait up to timeout if empty, then take the oldest item.
    // On return, PthreadLock has released mutex_; Main executes handlers unlocked.
    bool dequeue(MainMessage& message,
                 chrono::milliseconds timeout = chrono::milliseconds(10)) {
        PthreadLock lock(mutex_);
        const auto deadline = waitDeadline(timeout.count()); // One absolute deadline for this receive.
        while (!closed_ && queue_.empty()) { // Sleep only while there is no item and no close request.
            /* API: pthread_cond_timedwait
             * Sleep until signalled or until an absolute timespec deadline. The mutex must already be held. The call releases it while sleeping and reacquires it before returning. Recheck the predicate after waking.
             */
            const int result = pthread_cond_timedwait(
                &available_, &mutex_, &deadline);
            if (result == ETIMEDOUT) return false; // No item taken; Main can check other work.
            checkPthread(result); // Handle unexpected pthread errors before using shared data.
        }
        if (queue_.empty()) return false; // Closed and empty: there is no message to give Main.
        message = move(queue_.front()); // Give the oldest message to the caller.
        queue_.pop_front();             // Remove that queue item, exactly once.
        return true;
    }

    // SHUTDOWN / STATUS: wake Main so it can finish when this channel closes.
    void close() {
        {
            PthreadLock lock(mutex_);
            closed_ = true; // Publish shutdown while holding the same queue mutex.
        }
        /* API: pthread_cond_broadcast
         * Wake all waiters so each can recheck the protected stop/data condition.
         */
        pthread_cond_broadcast(&available_);
    }

    void notify_all() { pthread_cond_broadcast(&available_); }

    bool closed() const {
        PthreadLock lock(mutex_);
        return closed_;
    }

private:
    // FIFO STORAGE AND SYNCHRONIZATION (all access goes through the methods above).
    mutable pthread_mutex_t mutex_; // Guards queue_ and closed_.
    pthread_cond_t available_{};     // Lets Main sleep until work/close/deadline.
    deque<MainMessage> queue_;       // ACTUAL MESSAGE STORAGE, in FIFO order.
    bool closed_ = false; // Reject new messages and let waiting receivers finish.
};
}  // namespace phono_app
