#pragma once

#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/RuntimeTypes.h"

#include <deque>
#include "phonostethos/ThreadSupport.h"

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

    // The control mailbox stores one of these records plus has_mailbox_request_.
    // Main writes the record under mailbox_mutex_; FileIO takes it under that mutex.
    struct Request {
        RequestKind kind = RequestKind::List; // Which storage operation the worker must do.
        uint32_t operation_id = 0; // Returned in the result so Main can match this request.
        filesystem::path path; // Recording pathname or USB device node, depending on kind.
        string filesystem_type; // Mount request's Linux filesystem type, such as vfat.
        ArchiveIdentity identity; // Validated archive file to play or delete.
    };

    // MAILBOX SEND API: store one request; completion arrives later in CommandQueue.
    bool request(Request request); // false = occupied slot or worker stopping.

    // WORKER LIFETIME: construct resources, create pthread, then stop/join it.
    FileIoManager(const Options& options, CommandQueue& messages,
                  CircularBuffer& recording,
                  CircularBuffer& playback);
    ~FileIoManager(); // Stop/join before destroying its mutexes and buffers' references.
    bool start(); // Create the FileIO worker pthread.
    void stop(); // Signal shutdown and join the worker.

    // Storage operations below are executed by th_FileIO, not by Main.
    bool openRecordFile(const string& filename);
    bool openPlaybackFile(const string& filename);
    vector<string> getFileList();
    sf_count_t writeChunk(const int16_t* data, sf_count_t count);
    sf_count_t readChunk(int16_t* data, sf_count_t count);
    bool closeFile();
    bool deleteRecordFile(const string& filename);
    bool ejectStorage();


private:
    void run();
    void set_file_thread_policy();
    bool storage_available() const;

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
    // SHARED REFERENCES: SystemController passes these existing objects to FileIO.
    // RESULT QUEUE: both methods publish into Main's one shared CommandQueue.
    void post_result(WorkerResultCode code, uint32_t operation_id, uint8_t error = 0);
    void post_storage_status(WorkerResultCode code, uint32_t operation_id);
    CommandQueue& messages_;     // FileIO enqueues results/errors -> Main.
    // SAMPLE BUFFERS: transfer data in these methods; do disk I/O outside their locks.
    bool write_recording(); // Pop recording samples and write the WAV.
    void read_playback();   // Read the WAV and push playback samples.
    CircularBuffer& recording_;  // FileIO POPS samples produced by AudioDSP.
    CircularBuffer& playback_;   // FileIO PUSHES samples consumed by AudioDSP.

    // ================= MAILBOX: Main -> FileIO ===================
    // Storage is ONE Request struct + an occupied flag, protected by a mutex.
    // SEND: request() copies into the free slot and signals mailbox_ready_.
    // TAKE: take_request() copies it locally and clears the occupied flag under the mutex.
    // DO:   process_request() handles the local copy after releasing the mutex.
    // The condition variable wakes an idle FileIO worker; the actual request
    // remains in mailbox_. A signal itself does not contain or save a command.
    int take_request(Request& request); // 1 = request, 0 = normal work, -1 = stop.
    // Request handlers below run after take_request() has released the mailbox mutex.
    void process_request(const Request& request);
    void finish_recording(const Request& request);
    void prepare_playback(const Request& request);
    void list_archive(const Request& request);
    void mount_storage(const Request& request);
    void handle_removed_storage(const Request& request);
    pthread_mutex_t mailbox_mutex_{}; // Protects slot, flag and stopping_ state.
    pthread_cond_t mailbox_ready_{};  // Wake-up mechanism, not message storage.
    Request mailbox_{};               // The one shared control-message slot.
    bool has_mailbox_request_ = false; // true = unprocessed request; false = free.
    // ============================================================
    bool stopping_ = false;
    atomic<uint8_t> recording_error_{0};
    string mounted_device_;
    bool storage_removed_ = false;
    static void* threadEntry(void* context);
    pthread_t worker_{};
    bool worker_started_ = false;
};
}  // namespace phono_app
