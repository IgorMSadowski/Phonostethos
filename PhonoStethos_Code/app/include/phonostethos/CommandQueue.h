#pragma once

#include "phonostethos/RuntimeTypes.h"
#include "phonostethos/protocol.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
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

class CommandQueue {
public:
    bool enqueue(phono::protocol::CommandPacket command) {
        MainMessage message;
        message.kind = MessageKind::GuiCommand;
        message.command = command;
        return enqueue(move(message));
    }

    bool enqueue(MainMessage message) {
        {
            lock_guard<mutex> lock(mutex_);
            if (closed_) return false;
            queue_.push_back(move(message));
        }
        available_.notify_one();
        return true;
    }

    bool dequeue(MainMessage& message,
                 chrono::milliseconds timeout = chrono::milliseconds(10)) {
        unique_lock<mutex> lock(mutex_);
        available_.wait_for(lock, timeout,
                            [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) return false;
        message = move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        {
            lock_guard<mutex> lock(mutex_);
            closed_ = true;
        }
        available_.notify_all();
    }

    void notify_all() { available_.notify_all(); }

    bool closed() const {
        lock_guard<mutex> lock(mutex_);
        return closed_;
    }

private:
    mutable mutex mutex_;
    condition_variable available_;
    deque<MainMessage> queue_;
    bool closed_ = false;
};
}  // namespace phono_app
