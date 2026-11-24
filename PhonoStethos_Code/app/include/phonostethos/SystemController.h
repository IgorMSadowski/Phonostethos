/**
 * SystemController.h - implements th_Main: validation, worker coordination, authoritative state and LED masks.
 *
 * CONNECTIONS: SystemController.h declares the coordinator and owned objects; SystemController.cpp implements it. main.cpp starts it. It owns both sample buffers, all three managers and the DSP/LED objects, and handles worker results without blocking on worker I/O.
 *
 * FUNCTIONS:
 * - SystemController(): connect the owned resources/references used by this class.
 * - initSystem(): prepare shared resources, create the three worker pthreads and check audio readiness.
 * - run(): execute this class's thread loop in the order shown in its flowchart.
 * - processCommand(): validate a decoded GUI command and dispatch its named system operation.
 * - handleWorkerResult(): match a worker result to the pending operation before committing system state.
 * - setMode(): publish the selected filter mode under the configuration mutex.
 * - setGain(): publish a validated gain code; AudioDSP converts it to a multiplier.
 * - startRecording(): ask FileIO to open a new WAV and keep recording disabled until OPEN_READY.
 * - stopRecording(): close the producer gate and let FileIO drain and finalize the recording.
 * - startPlayback(): resolve a file ID and coordinate capture suspension and WAV prefill.
 * - stopPlayback(): coordinate playback stop, file closure and return to microphone capture.
 * - deleteRecording(): resolve the selected file ID and request deletion in FileIO.
 * - listFiles(): request an archive scan or queue the requested page of an existing scan.
 * - ejectStorage(): synchronize and unmount storage in FileIO, or request that operation from Main.
 * - initUdevMonitor(): subscribe to Linux block-device events and obtain the monitor descriptor.
 * - checkUsbEvents(): poll udev without blocking and retain storage requests for FileIO.
 * - broadcastStatus(): queue capability and State snapshots for Network rather than writing a socket.
 * - generateFilename(): build a mode/date/time/counter WAV name without overwriting an existing file.
 * - stop(): request teardown and join workers before destroying their shared resources.
 * - begin_operation(): allocate a correlation ID and remember the accepted command in Main.
 * - processMessage(): route MainMessage by kind to command handling or worker-result handling.
 * - handle_notification(): handle unsolicited worker/connection events outside an active GUI transition.
 * - handle_operation_error(): fail the matching transition and coordinate any required cleanup.
 * - handle_recording_result(): advance recording open/close only after the matching FileIO result.
 * - handle_playback_result(): advance playback preparation/stop through worker acknowledgements.
 * - complete_operation(): publish the final command result and clear Main's pending context.
 * - publish_status(): derive the storage/button capability packet from current authoritative state.
 * - publish_state(): serialize transport, mode, gain, storage, session and diagnostic counters.
 * - publish_command_result(): queue a final or pending acknowledgement for the GUI command.
 * - update_leds(): build and write the complete mode-plus-recording mask from Main's acknowledged state.
 * - handle_archive_result(): update the stable connection-scoped file-ID registry from FileIO's scan.
 * - start_playback_cleanup(): stop the consumer before FileIO closes/discards playback data.
 * - fail_start_playback(): undo partial playback preparation and report the failure.
 * - queue_archive_page(): prepare a bounded page of IDs and names for the GUI archive.
 * - set_error(): publish the current system error code atomically.
 * - set_main_thread_policy(): request CPU 0 affinity and SCHED_OTHER priority 0 with nice 0.
 * - request_file(): attempt delivery to FileIO's one-slot control mailbox.
 * - queue_file_request(): deliver a FileIO request or retain it in Main for a later retry.
 * - request_audio(): attempt delivery to AudioDSP's one-slot control mailbox.
 * - dispatch_deferred_operation_request(): retry Main's retained operation when the FileIO mailbox becomes free.
 * - dispatch_deferred_mount(): retry Main's retained storage-device request when FileIO can accept it.
 */

// API DOCUMENTATION FOR THIS FILE:
// POSIX threads: creation, joining, mutexes and condition variables:
// https://man7.org/linux/man-pages/man7/pthreads.7.html
// pthread mutex lock, try-lock and unlock:
// https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
// Linux scheduling policies and real-time priorities:
// https://man7.org/linux/man-pages/man7/sched.7.html
// libudev contexts, device properties, enumeration and hotplug monitoring:
// https://www.freedesktop.org/software/systemd/man/latest/libudev.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include "phonostethos/AudioEngine.h" // Include the AudioEngine class/types used here; implements th_AudioDSP: live capture, filtered/gained output, stored playback and FFT publication.
#include "phonostethos/CircularBuffer.h" // Include the CircularBuffer class/types used here; implements the shared PCM16 circular sample buffer.
#include "phonostethos/CommandQueue.h" // Include the CommandQueue class/types used here; defines MainMessage and the FIFO consumed by th_Main.
#include "phonostethos/DataQueue.h" // Include the DataQueue class/types used here; implements the latest waveform/FFT snapshot channel.
#include "phonostethos/FileIoManager.h" // Include the FileIoManager class/types used here; implements th_FileIO: WAV recording/playback, archive management and FAT32 USB storage operations.
#include "phonostethos/LedDriver.h" // Include the LedDriver class/types used here; wraps the LED character-device descriptor in user space.
#include "phonostethos/NetworkManager.h" // Include the NetworkManager class/types used here; implements th_Network and its protected outgoing ControlReplyQueue.
#include "phonostethos/RuntimeTypes.h" // Include the RuntimeTypes class/types used here; defines shared audio constants, runtime options, telemetry and archive identities.

#include <map> // Include stable file-ID/name lookup tables.
#include "phonostethos/ThreadSupport.h" // Include the ThreadSupport class/types used here; provides the small pthread helpers shared by queues, buffers and managers.
#include <chrono> // Include typed durations and monotonic deadlines.
#include <string> // Include owned filenames, device paths and diagnostic text.
#include <vector> // Include variable-size byte frames and archive entry lists.

struct udev; // Forward-declare the opaque libudev type; its API definitions are included in SystemController.cpp.
struct udev_monitor; // Forward-declare the opaque libudev type; its API definitions are included in SystemController.cpp.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// Main owns the system state; workers report results instead of changing that state.
class SystemController { // Declare SystemController and its related operations/state.
public: // The class operations callable by other code.
    explicit SystemController(const Options& options); // Connect the owned resources/references used by this class.
    ~SystemController(); // Complete the call above with these remaining arguments.

    // REPORT 3.4.2: design operations, in the same order as the report.
    bool initSystem(); // Prepare shared resources, create the three worker pthreads and check audio readiness.
    void run(); // Execute this class's thread loop in the order shown in its flowchart.
    void processCommand(const phono::protocol::CommandPacket& request); // Validate a decoded GUI command and dispatch its named system operation.
    void handleWorkerResult(const MainMessage& message); // Match a worker result to the pending operation before committing system state.
    void setMode(FilterMode mode); // Publish the selected filter mode under the configuration mutex.
    void setGain(int gain); // Publish a validated gain code; AudioDSP converts it to a multiplier.
    void startRecording(); // Ask FileIO to open a new WAV and keep recording disabled until OPEN_READY.
    void stopRecording(); // Close the producer gate and let FileIO drain and finalize the recording.
    void startPlayback(uint32_t file_id); // Resolve a file ID and coordinate capture suspension and WAV prefill.
    void stopPlayback(); // Coordinate playback stop, file closure and return to microphone capture.
    void deleteRecording(uint32_t file_id); // Resolve the selected file ID and request deletion in FileIO.
    void listFiles(uint32_t cursor); // Request an archive scan or queue the requested page of an existing scan.
    void ejectStorage(); // Synchronize and unmount storage in FileIO, or request that operation from Main.
    bool initUdevMonitor(); // Subscribe to Linux block-device events and obtain the monitor descriptor.
    void checkUsbEvents(); // Poll udev without blocking and retain storage requests for FileIO.
    void broadcastStatus(); // Queue capability and State snapshots for Network rather than writing a socket.
    string generateFilename(FilterMode mode); // Build a mode/date/time/counter WAV name without overwriting an existing file.

    // Service teardown is separate from normal GUI operation.
    void stop(); // Request teardown and join workers before destroying their shared resources.

private: // Internal helpers and state used by this class.
    enum class OperationKind { // Give the following named choices distinct typed values.
        StartRecording, // Name this field, choice or argument: Start Recording.
        StopRecording, // Name this field, choice or argument: Stop Recording.
        StartPlayback, // Name this field, choice or argument: Start Playback.
        StopPlayback, // Name this field, choice or argument: Stop Playback.
        ListArchive, // Name this field, choice or argument: List Archive.
        DeleteRecording, // Name this field, choice or argument: Delete Recording.
        EjectStorage, // Name this field, choice or argument: Eject Storage.
    }; // End this block, declaration or argument list.

    enum class WorkflowStage { // Give the following named choices distinct typed values.
        WaitingForWorkers, // Name this field, choice or argument: Waiting For Workers.
        ResumingCapture, // Name this field, choice or argument: Resuming Capture.
        EjectingStorage, // Name this field, choice or argument: Ejecting Storage.
    }; // End this block, declaration or argument list.

    // Main keeps one unfinished GUI operation. Results arrive later through CommandQueue.
    struct PendingOperation { // Group the related fields in one plain data record.
        OperationKind kind; // Declare kind for this operation.
        phono::protocol::CommandType command; // Declare command for this operation.
        uint32_t operation_id; // Declare operation id for this operation.
        uint32_t argument = 0; // Initialize argument to 0.
        bool file_done = false;   // FileIO has acknowledged its part.
        bool audio_done = false;  // AudioDSP has acknowledged its part.
        uint8_t error_code = 0; // Initialize error code to 0.
        bool report_result = true; // Initialize report result to true.
        bool commit_requested = false; // Prevent duplicate transition requests.
        WorkflowStage stage = WorkflowStage::WaitingForWorkers; // Initialize stage to WorkflowStage::WaitingForWorkers.
    }; // End this block, declaration or argument list.


    const Options& options_; // Declare options for this class instance.
    atomic<unsigned> mode_{static_cast<unsigned>(FilterMode::GENERAL)}; // Selected filter code; Main writes it and AudioDSP reads a configuration snapshot.
    atomic<unsigned> gain_{1}; // Selected gain CODE, not necessarily the numeric multiplier; convert 101/102/103 before DSP.
    atomic<bool> recording_{false}; // Shared recording-enabled flag; the buffer gate is checked under its own mutex too.
    atomic<unsigned> error_code_{0}; // Current protocol error value sent in server status/state.
    atomic<bool> led_error_{false}; // Remember a device-interface failure; this is not physical LED feedback.
    atomic<phono::protocol::Transport> transport_{ // One transport enum represents LIVE/RECORDING/PLAYBACK/transitions/error without conflicting booleans.
        phono::protocol::Transport::Live}; // Complete this expression or declaration.
    atomic<uint32_t> active_session_{0}; // Recording session number or active playback file ID, according to transport state.
    atomic<uint32_t> dropped_telemetry_{0}; // Count visualization updates skipped when the channel is busy or disconnected.
    atomic<uint32_t> recording_overruns_{0}; // Count failed recording-buffer pushes; those failures must remain visible.
    atomic<bool> audio_ready_{false}; // Main's acknowledged audio availability, updated from worker results.
    atomic<bool> audio_init_complete_{false}; // Let startup distinguish a received audio result from the startup timeout.
    uint8_t storage_flags_ = 0; // Separate present/mounted/readable/writable/free-space bits used to derive GUI capabilities.
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
    pthread_mutex_t configuration_mutex_{}; // Protect the mode/gain pair while Main publishes and AudioDSP copies it.
    CircularBuffer recording_buffer_; // Recording samples: AudioDSP -> FileIO.
    CircularBuffer playback_buffer_;  // Playback samples: FileIO -> AudioDSP.
    CommandQueue command_queue_;      // Main's incoming commands/results.
    DataQueue data_queue_;            // Network's latest visualization snapshot.
    LedDriver led_driver_; // Main uses this user-space wrapper to write complete masks to the kernel driver.
    FileIoManager writer_; // The FileIoManager object that owns th_FileIO, its mailbox and all WAV handles.
    // Destruction is in reverse order: AudioDSP joins before the FFT plan is destroyed.
    DspProcessor dsp_processor_; // Main-owned DSP/FFTW state; only AudioDSP processes audio with it.
    AudioEngine audio_; // The AudioEngine object that owns th_AudioDSP and its control mailbox.
    NetworkManager network_; // The NetworkManager object that owns th_Network, TCP descriptors and ControlReplyQueue.
    map<uint32_t, ArchiveIdentity> archive_by_id_; // Resolve a GUI file ID to the saved basename/metadata identity.
    map<string, uint32_t> archive_id_by_name_; // Keep a recording's ID stable when refreshing this connection's archive.
    vector<phono::protocol::FileEntry> archive_entries_; // The sorted ID/name records used to build archive reply pages.
    uint32_t next_file_id_ = 1; // Allocate connection-scoped file IDs without treating the current row index as identity.
    uint32_t next_session_id_ = 1; // Recording filename suffix counter; exclusive file creation prevents overwrite after reboot.
    uint32_t next_operation_id_ = 1; // Correlation counter for worker results; unrelated to the filename or GUI file ID.
    // MAIN'S OWN BOOKKEEPING: which GUI operation is awaiting worker results.
    // Main alone uses this record. Workers return MainMessage objects through
    // CommandQueue; they do not write directly into pending_.
    PendingOperation pending_{}; // Main-only record of the accepted operation and the worker acknowledgements still needed.
    bool has_pending_operation_ = false; // True until that operation completes.
    bool initialized_ = false; // Initialize initialized to false.
    bool stopping_ = false; // Initialize stopping to false.
    chrono::steady_clock::time_point next_storage_check_{}; // Next monotonic deadline for a periodic storage-capability refresh.
    udev* udev_context_ = nullptr; // Initialize udev context to nullptr.
    udev_monitor* udev_monitor_ = nullptr; // Initialize udev monitor to nullptr.
    int udev_fd_ = -1; // Monitor descriptor, or -1 when the monitor is unavailable/closed.
    // MAILBOX DELIVERY: all send/retry helpers are implemented together in the .cpp.
    // MAIN'S RETRY RECORDS: retain a FileIO request if its mailbox is occupied.
    // Only Main uses these local slots. dispatch_deferred_* retries delivery;
    // the shared FileIO mailbox itself is writer_.mailbox_ in FileIoManager.h.
    FileIoManager::Request deferred_operation_request_{}; // Main-only retry record when FileIO's shared mailbox is temporarily occupied.
    bool has_deferred_operation_ = false; // Mark deferred operation as empty.
    FileIoManager::Request deferred_storage_request_{}; // Main-only retry record for a udev mount/removal/capability request.
    bool has_deferred_storage_ = false; // Mark deferred storage as empty.
    // ==================== PRIVATE HELPER METHODS ====================
    uint32_t begin_operation(OperationKind kind, // Pass this next argument/value to the call or initializer above.
                            phono::protocol::CommandType command, // Declare command for this operation.
                            uint32_t argument); // Declare argument for this operation.
    void processMessage(const MainMessage& message); // Route MainMessage by kind to command handling or worker-result handling.
    bool handle_notification(const MainMessage& message); // Handle unsolicited worker/connection events outside an active GUI transition.
    void handle_operation_error(const MainMessage& message); // Fail the matching transition and coordinate any required cleanup.
    void handle_recording_result(const MainMessage& message); // Advance recording open/close only after the matching FileIO result.
    void handle_playback_result(const MainMessage& message); // Advance playback preparation/stop through worker acknowledgements.
    void complete_operation(bool success, uint8_t error_code = 0); // Publish the final command result and clear Main's pending context.
    void publish_status(); // Derive the storage/button capability packet from current authoritative state.
    void publish_state(); // Serialize transport, mode, gain, storage, session and diagnostic counters.
    void publish_command_result(phono::protocol::CommandType command, // Pass this next argument/value to the call or initializer above.
                                phono::protocol::Result result, // Declare result for this operation.
                                uint32_t detail); // Declare detail for this operation.
    void update_leds(); // Build and write the complete mode-plus-recording mask from Main's acknowledged state.
    void handle_archive_result(const MainMessage& message); // Update the stable connection-scoped file-ID registry from FileIO's scan.
    void start_playback_cleanup(uint32_t operation_id, // Pass this next argument/value to the call or initializer above.
                               bool report_result, // Declare report result for this operation.
                               uint8_t error_code = 0); // Initialize error code to 0).
    void fail_start_playback(uint8_t error_code); // Undo partial playback preparation and report the failure.
    void queue_archive_page(uint32_t cursor); // Prepare a bounded page of IDs and names for the GUI archive.
    void set_error(uint8_t error_code); // Publish the current system error code atomically.
    void set_main_thread_policy(); // Request CPU 0 affinity and SCHED_OTHER priority 0 with nice 0.
    bool request_file(FileIoManager::Request request); // Submit to FileIO's one slot.
    bool queue_file_request(FileIoManager::Request request); // Submit or retain for retry.
    bool request_audio(AudioEngine::Request request); // Submit to AudioDSP's one slot.
    void dispatch_deferred_operation_request(); // Retry Main's retained storage operation.
    void dispatch_deferred_mount(); // Retry Main's retained mount/device request.

}; // End this block, declaration or argument list.
}  // namespace phono_app
