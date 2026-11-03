#pragma once

#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/RuntimeTypes.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <sndfile.h>

using namespace std;

namespace phono_app {
class FileIOManager {
public:
    enum class RequestKind {
        OpenRecording,
        CloseRecording,
        PreparePlayback,
        StopPlayback,
        List,
        Delete,
        Eject,
        Mount,
        DeviceRemoved,
        RefreshStorage,
    };

    struct Request {
        RequestKind kind = RequestKind::List;
        uint32_t operation_id = 0;
        filesystem::path path;
        string filesystem_type;
        ArchiveIdentity identity;
    };

    FileIOManager(const Options& options, CommandQueue& messages,
                  CircularBuffer<int16_t, kCircularBufferSamples>& recording,
                  CircularBuffer<int16_t, kCircularBufferSamples>& playback);
    ~FileIOManager();

    bool request(Request request);
    void stop();

private:
    void run();
    void set_file_thread_policy();
    void process_request(const Request& request, SNDFILE*& recording_file,
                         int& recording_fd, SNDFILE*& playback_file,
                         int& playback_fd, bool& recording_active,
                         bool& playback_active,
                         uint32_t& recording_operation,
                         uint32_t& playback_operation);
    bool write_recording(SNDFILE* file);
    void read_playback(SNDFILE* file, bool& playback_active,
                       uint32_t operation_id);
    bool storage_available() const;
    void post_result(WorkerResultCode code, uint32_t operation_id,
                     uint8_t error = 0);
    void post_storage_status(WorkerResultCode code, uint32_t operation_id);

    const Options& options_;
    CommandQueue& messages_;
    CircularBuffer<int16_t, kCircularBufferSamples>& recording_;
    CircularBuffer<int16_t, kCircularBufferSamples>& playback_;
    mutex mailbox_mutex_;
    condition_variable mailbox_ready_;
    optional<Request> mailbox_;
    bool stopping_ = false;
    atomic<uint8_t> recording_error_{0};
    string mounted_device_;
    bool storage_removed_ = false;
    thread worker_;
};
}  // namespace phono_app
