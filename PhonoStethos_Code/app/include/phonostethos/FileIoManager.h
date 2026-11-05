#pragma once

#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/RuntimeTypes.h"

#include <deque>
#include "phonostethos/ThreadSupport.h"
#include <optional>

#include <sndfile.h>

using namespace std;

namespace phono_app {
// FileIO worker: owns WAV handles, storage checks, archive operations and eject.
class FileIoManager {
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

    FileIoManager(const Options& options, CommandQueue& messages,
                  CircularBuffer<int16_t, kCircularBufferSamples>& recording,
                  CircularBuffer<int16_t, kCircularBufferSamples>& playback);
    ~FileIoManager();

    // Storage operations below are executed by th_FileIO, not by Main.
    bool openRecordFile(const string& filename);
    bool openPlaybackFile(const string& filename);
    vector<string> getFileList();
    sf_count_t writeChunk(const int16_t* data, sf_count_t count);
    sf_count_t readChunk(int16_t* data, sf_count_t count);
    bool closeFile();
    bool deleteRecordFile(const string& filename);
    bool ejectStorage();

    bool request(Request request);
    bool start();
    void stop();

private:
    void run();
    void set_file_thread_policy();
    void process_request(const Request& request);
    bool write_recording();
    void read_playback();
    bool storage_available() const;
    void post_result(WorkerResultCode code, uint32_t operation_id,
                     uint8_t error = 0);
    void post_storage_status(WorkerResultCode code, uint32_t operation_id);

    SNDFILE* recording_file_ = nullptr;
    SNDFILE* playback_file_ = nullptr;
    int recording_fd_ = -1;
    int playback_fd_ = -1;
    bool recording_active_ = false;
    bool playback_active_ = false;
    uint32_t recording_operation_ = 0;
    uint32_t playback_operation_ = 0;
    uint8_t last_error_ = 0;

    const Options& options_;
    CommandQueue& messages_;
    CircularBuffer<int16_t, kCircularBufferSamples>& recording_;
    CircularBuffer<int16_t, kCircularBufferSamples>& playback_;
    pthread_mutex_t mailbox_mutex_{};
    pthread_cond_t mailbox_ready_{};
    optional<Request> mailbox_;
    bool stopping_ = false;
    atomic<uint8_t> recording_error_{0};
    string mounted_device_;
    bool storage_removed_ = false;
    static void* threadEntry(void* context);
    pthread_t worker_{};
    bool worker_started_ = false;
};
}  // namespace phono_app
