#pragma once

#include "phonostethos/AudioEngine.h"
#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"
#include "phonostethos/FileIoManager.h"
#include "phonostethos/LedDriver.h"
#include "phonostethos/NetworkManager.h"
#include "phonostethos/RuntimeTypes.h"

#include <map>
#include "phonostethos/ThreadSupport.h"
#include <chrono>
#include <string>
#include <vector>

struct udev;
struct udev_monitor;

using namespace std;

namespace phono_app {
// Main owns the system state; workers report results instead of changing that state.
class SystemController {
public:
    explicit SystemController(const Options& options);
    ~SystemController();

    bool initSystem();
    void run();
    void stop();

    // GUI commands are validated first, then dispatched to these operations.
    void setMode(FilterMode mode);
    void setGain(int gain);
    void startRecording();
    void stopRecording();
    void startPlayback(uint32_t file_id);
    void stopPlayback();
    void ejectStorage();
    void listFiles(uint32_t cursor);
    void deleteRecording(uint32_t file_id);
    void broadcastStatus();

private:
    enum class OperationKind {
        StartRecording,
        StopRecording,
        StartPlayback,
        StopPlayback,
        ListArchive,
        DeleteRecording,
        EjectStorage,
    };

    enum class WorkflowStage {
        WaitingForWorkers,
        ResumingCapture,
        EjectingStorage,
    };

    // Main keeps one unfinished GUI operation. Results arrive later through CommandQueue.
    struct PendingOperation {
        OperationKind kind;
        phono::protocol::Command command;
        uint32_t operation_id;
        uint32_t argument = 0;
        bool file_done = false;   // FileIO has acknowledged its part.
        bool audio_done = false;  // AudioDSP has acknowledged its part.
        uint8_t error_code = 0;
        bool report_result = true;
        bool commit_requested = false; // Prevent duplicate transition requests.
        WorkflowStage stage = WorkflowStage::WaitingForWorkers;
    };

    uint32_t begin_operation(OperationKind kind,
                            phono::protocol::Command command,
                            uint32_t argument);
    void processMessage(const MainMessage& message);
    void processCommand(const phono::protocol::CommandPacket& command);
    void handleWorkerResult(const MainMessage& message);
    bool handle_notification(const MainMessage& message);
    void handle_operation_error(const MainMessage& message);
    void handle_recording_result(const MainMessage& message);
    void handle_playback_result(const MainMessage& message);
    void complete_operation(bool success, uint8_t error_code = 0);
    void publish_status();
    void publish_state();
    void publish_command_result(phono::protocol::Command command,
                                phono::protocol::Result result,
                                uint32_t detail);
    void update_leds();
    void handle_archive_result(const MainMessage& message);
    void start_playback_cleanup(uint32_t operation_id,
                               bool report_result,
                               uint8_t error_code = 0);
    void fail_start_playback(uint8_t error_code);
    void queue_archive_page(uint32_t cursor);
    string generateFilename(FilterMode mode);
    void set_error(uint8_t error_code);
    void set_main_thread_policy();
    bool initUdevMonitor();
    void checkUsbEvents();

    const Options& options_;
    atomic<unsigned> mode_{static_cast<unsigned>(FilterMode::General)};
    atomic<unsigned> gain_{1};
    atomic<bool> recording_{false};
    atomic<unsigned> error_code_{0};
    atomic<bool> led_error_{false};
    atomic<phono::protocol::Transport> transport_{
        phono::protocol::Transport::Live};
    atomic<uint32_t> active_session_{0};
    atomic<uint32_t> dropped_telemetry_{0};
    atomic<uint32_t> recording_overruns_{0};
    atomic<bool> audio_ready_{false};
    atomic<bool> audio_init_complete_{false};
    uint8_t storage_flags_ = 0;
    // ================= THREAD COMMUNICATION MAP =================
    // QUEUE:   Network + FileIO + AudioDSP -> command_queue_ -> Main
    // QUEUE:   Main -> network_.control_replies_ -> Network -> GUI TCP socket
    // QUEUE:   AudioDSP -> data_queue_ -> Network (latest waveform/FFT only)
    // MAILBOX: Main -> writer_.mailbox_ -> FileIO (one unprocessed Request)
    // MAILBOX: Main -> audio_.request_ -> AudioDSP (one unprocessed Request)
    // BUFFER:  AudioDSP PUSH -> recording_buffer_ -> FileIO POP
    // BUFFER:  FileIO PUSH -> playback_buffer_ -> AudioDSP POP
    // The queues/buffers below are actual objects, shared by reference with workers.
    // Each worker's control mailbox is declared inside its own class header.
    // ============================================================
    // SHARED CONFIGURATION: Main writes mode/gain; AudioDSP reads a snapshot.
    // The mutex protects that snapshot; there is no configuration message queue.
    pthread_mutex_t configuration_mutex_{};
    CircularBuffer recording_buffer_; // Recording samples: AudioDSP -> FileIO.
    CircularBuffer playback_buffer_;  // Playback samples: FileIO -> AudioDSP.
    CommandQueue command_queue_;      // Main's incoming commands/results.
    DataQueue data_queue_;            // Network's latest visualization snapshot.
    LedDriver led_driver_;
    FileIoManager writer_;
    // Destruction is in reverse order: AudioDSP joins before the FFT plan is destroyed.
    DspProcessor dsp_processor_;
    AudioEngine audio_;
    NetworkManager network_;
    map<uint32_t, ArchiveIdentity> archive_by_id_;
    map<string, uint32_t> archive_id_by_name_;
    vector<phono::protocol::FileEntry> archive_entries_;
    uint32_t next_file_id_ = 1;
    uint32_t next_session_id_ = 1;
    uint32_t next_operation_id_ = 1;
    // MAIN'S OWN BOOKKEEPING: which GUI operation is awaiting worker results.
    // Main alone uses this record. Workers return MainMessage objects through
    // CommandQueue; they do not write directly into pending_.
    PendingOperation pending_{};
    bool has_pending_operation_ = false; // True until that operation completes.
    bool initialized_ = false;
    bool stopping_ = false;
    chrono::steady_clock::time_point next_storage_check_{};
    udev* udev_context_ = nullptr;
    udev_monitor* udev_monitor_ = nullptr;
    int udev_fd_ = -1;
    // MAILBOX DELIVERY: all send/retry helpers are implemented together in the .cpp.
    bool request_file(FileIoManager::Request request); // Submit to FileIO's one slot.
    bool queue_file_request(FileIoManager::Request request); // Submit or retain for retry.
    bool request_audio(AudioEngine::Request request); // Submit to AudioDSP's one slot.
    void dispatch_deferred_operation_request(); // Retry Main's retained storage operation.
    void dispatch_deferred_mount(); // Retry Main's retained mount/device request.
    // MAIN'S RETRY RECORDS: retain a FileIO request if its mailbox is occupied.
    // Only Main uses these local slots. dispatch_deferred_* retries delivery;
    // the shared FileIO mailbox itself is writer_.mailbox_ in FileIoManager.h.
    FileIoManager::Request deferred_operation_request_{};
    bool has_deferred_operation_ = false; // Mark deferred operation as empty.
    FileIoManager::Request deferred_storage_request_{};
    bool has_deferred_storage_ = false; // Mark deferred storage as empty.
};
}  // namespace phono_app
