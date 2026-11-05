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

struct MainMessage {
    MessageKind kind = MessageKind::GuiCommand;
    phono::protocol::CommandPacket command{};
    uint32_t operation_id = 0;
    WorkerResultCode result_code = WorkerResultCode::AudioReady;
    uint8_t error_code = 0;
    uint8_t storage_flags = 0;
    vector<ArchiveIdentity> archive;
};

// Network and workers enqueue; Main dequeues. Messages contain commands OR results.
class CommandQueue {
public:
    CommandQueue() {
        initializeMutex(mutex_);
        initializeCondition(available_);
    }
    ~CommandQueue() {
        pthread_cond_destroy(&available_);
        pthread_mutex_destroy(&mutex_);
    }
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    bool enqueue(phono::protocol::CommandPacket command) {
        MainMessage message;
        message.kind = MessageKind::GuiCommand;
        message.command = command;
        return enqueue(move(message));
    }

    bool enqueue(MainMessage message) {
        {
            PthreadLock lock(mutex_);
            if (closed_) return false;
            queue_.push_back(move(message));
        }
        pthread_cond_signal(&available_);
        return true;
    }

    bool dequeue(MainMessage& message,
                 chrono::milliseconds timeout = chrono::milliseconds(10)) {
        PthreadLock lock(mutex_);
        const auto deadline = waitDeadline(timeout.count());
        while (!closed_ && queue_.empty()) {
            const int result = pthread_cond_timedwait(
                &available_, &mutex_, &deadline);
            if (result == ETIMEDOUT) return false;
            checkPthread(result);
        }
        if (queue_.empty()) return false;
        message = move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        {
            PthreadLock lock(mutex_);
            closed_ = true;
        }
        pthread_cond_broadcast(&available_);
    }

    void notify_all() { pthread_cond_broadcast(&available_); }

    bool closed() const {
        PthreadLock lock(mutex_);
        return closed_;
    }

private:
    mutable pthread_mutex_t mutex_;
    pthread_cond_t available_{};
    deque<MainMessage> queue_;
    bool closed_ = false;
};
}  // namespace phono_app
