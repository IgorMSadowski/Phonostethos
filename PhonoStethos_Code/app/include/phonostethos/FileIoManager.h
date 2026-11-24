/**
 * FileIoManager.h - implements th_FileIO: WAV recording/playback, archive management and FAT32 USB storage operations.
 *
 * CONNECTIONS: FileIoManager.h declares the class; FileIoManager.cpp implements it. SystemController posts one-slot mailbox requests. FileIO pops recording samples, pushes playback samples and reports completion through CommandQueue.
 *
 * FUNCTIONS:
 * - openRecordFile(): create a new WAV exclusively so an existing recording is never overwritten.
 * - openPlaybackFile(): open the resolved WAV and reject an unsupported channel count, rate or sample format.
 * - writeChunk(): write the requested PCM16 sample count through libsndfile and return the actual count.
 * - readChunk(): read at most the requested PCM16 sample count through libsndfile.
 * - deleteRecordFile(): check the server-resolved basename and remove that recording.
 * - closeFile(): check libsndfile flush/finalization and close any open WAV handles/descriptors.
 * - getFileList(): scan storage and return sorted WAV basenames.
 * - ejectStorage(): synchronize and unmount storage in FileIO, or request that operation from Main.
 * - start(): create this worker pthread once and retain its handle for joining.
 * - stop(): request teardown and join workers before destroying their shared resources.
 * - request(): put one control record into the worker mailbox if its slot is free.
 * - run(): execute this class's thread loop in the order shown in its flowchart.
 * - set_file_thread_policy(): request CPU 2 affinity and SCHED_FIFO priority 50.
 * - storage_available(): check whether the configured storage path is currently usable.
 * - post_result(): construct a MainMessage and enqueue completion or error through CommandQueue.
 * - post_storage_status(): derive storage capabilities in FileIO and report them to Main.
 * - write_recording(): pop recorded samples and write/check them outside the sample-buffer mutex.
 * - read_playback(): read WAV samples and push them into the playback buffer for AudioDSP.
 * - take_request(): copy a mailbox request locally and free its slot under the mailbox mutex.
 * - process_request(): dispatch a FileIO control request after releasing the mailbox mutex.
 * - finish_recording(): drain and finalize a recording, then send its worker result to Main.
 * - prepare_playback(): validate the selected file identity, open the WAV and prefill the playback buffer.
 * - list_archive(): scan files and attach their identities to an archive-ready result.
 * - mount_storage(): validate and mount a supported USB filesystem in FileIO.
 * - handle_removed_storage(): stop affected file activity and report unavailable storage.
 * - threadEntry(): recover the object pointer passed to pthread_create and call its worker loop.
 */

// API DOCUMENTATION FOR THIS FILE:
// libsndfile: SNDFILE/SF_INFO, WAV open, sample I/O, errors and checked close:
// https://libsndfile.github.io/libsndfile/api.html
// POSIX threads: creation, joining, mutexes and condition variables:
// https://man7.org/linux/man-pages/man7/pthreads.7.html
// pthread mutex lock, try-lock and unlock:
// https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
// Condition waits: mutex release/reacquisition and absolute timeouts:
// https://man7.org/linux/man-pages/man3/pthread_cond_timedwait.3p.html
// Linux scheduling policies and real-time priorities:
// https://man7.org/linux/man-pages/man7/sched.7.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include "phonostethos/CircularBuffer.h" // Include the CircularBuffer class/types used here; implements the shared PCM16 circular sample buffer.
#include "phonostethos/CommandQueue.h" // Include the CommandQueue class/types used here; defines MainMessage and the FIFO consumed by th_Main.
#include "phonostethos/RuntimeTypes.h" // Include the RuntimeTypes class/types used here; defines shared audio constants, runtime options, telemetry and archive identities.

#include "phonostethos/ThreadSupport.h" // Include the ThreadSupport class/types used here; provides the small pthread helpers shared by queues, buffers and managers.

#include <sndfile.h> // Include libsndfile WAV handles, format descriptors and PCM read/write APIs.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// FileIO worker: owns WAV handles, storage checks, archive operations and eject.
class FileIoManager { // Declare FileIoManager and its related operations/state.
public: // The class operations callable by other code.
    enum class RequestKind { // Give the following named choices distinct typed values.
        OPEN_RECORD, // Name this field, choice or argument: Open Recording.
        CLOSE_RECORD, // Name this field, choice or argument: Close Recording.
        PREPARE_PLAYBACK, // Name this field, choice or argument: Prepare Playback.
        STOP_PLAYBACK, // Name this field, choice or argument: Stop Playback.
        LIST, // Name this field, choice or argument: LIST.
        DELETE, // Name this field, choice or argument: DELETE.
        EJECT, // Name this field, choice or argument: EJECT.
        MOUNT, // Name this field, choice or argument: MOUNT.
        DEVICE_REMOVED, // Name this field, choice or argument: Device Removed.
        REFRESH_STORAGE, // Name this field, choice or argument: Refresh Storage.
    }; // End this block, declaration or argument list.

    // The control mailbox stores one of these records plus has_mailbox_request_.
    // Main writes the record under mailbox_mutex_; FileIO takes it under that mutex.
    struct Request { // Group the related fields in one plain data record.
        RequestKind kind = RequestKind::LIST; // Which storage operation the worker must do.
        uint32_t operation_id = 0; // Returned in the result so Main can match this request.
        filesystem::path path; // Recording pathname or USB device node, depending on kind.
        string filesystem_type; // Mount request's Linux filesystem type, such as vfat.
        ArchiveIdentity identity; // Validated archive file to play or delete.
    }; // End this block, declaration or argument list.

    FileIoManager(const Options& options, CommandQueue& messages, // Call FileIoManager with the arguments shown here.
                  CircularBuffer& recording, CircularBuffer& playback); // Declare recording for this operation.
    ~FileIoManager(); // Complete the call above with these remaining arguments.

    // REPORT 3.4.2: WAV/storage operations, executed only by th_FileIO.
    bool openRecordFile(const string& filename); // Create a new WAV exclusively so an existing recording is never overwritten.
    bool openPlaybackFile(const string& filename); // Open the resolved WAV and reject an unsupported channel count, rate or sample format.
    sf_count_t writeChunk(const int16_t* data, sf_count_t count); // Write the requested PCM16 sample count through libsndfile and return the actual count.
    sf_count_t readChunk(int16_t* data, sf_count_t count); // Read at most the requested PCM16 sample count through libsndfile.
    bool deleteRecordFile(const string& filename); // Check the server-resolved basename and remove that recording.
    bool closeFile(); // Check libsndfile flush/finalization and close any open WAV handles/descriptors.
    vector<string> getFileList(); // Scan storage and return sorted WAV basenames.
    bool ejectStorage(); // Synchronize and unmount storage in FileIO, or request that operation from Main.

    // Lifecycle and one-slot mailbox API, called by Main.
    bool start(); // Create this worker pthread once and retain its handle for joining.
    void stop(); // Request teardown and join workers before destroying their shared resources.
    bool request(Request request); // Put one control record into the worker mailbox if its slot is free.

private: // Internal helpers and state used by this class.
    // Worker loop follows the thread flowchart.
    void run(); // Execute this class's thread loop in the order shown in its flowchart.

    SNDFILE* recording_file_ = nullptr; // FileIO-only libsndfile recording handle; nullptr means no recording WAV is open.
    SNDFILE* playback_file_ = nullptr; // FileIO-only libsndfile playback handle; nullptr means no playback WAV is open.
    int recording_fd_ = -1; // Recording descriptor retained separately so final synchronization/close can be checked.
    int playback_fd_ = -1; // Read-only playback descriptor; -1 means it has not been opened or is closed.
    bool recording_active_ = false; // FileIO must continue consuming recording samples while this flag is true.
    bool playback_active_ = false; // This execution context is currently handling stored playback.
    uint32_t recording_operation_ = 0; // Correlation ID returned with this recording's completion/error.
    uint32_t playback_operation_ = 0; // Correlation ID returned with this playback's completion/error.
    uint8_t last_error_ = 0; // Classified protocol error retained when a storage helper reports failure.

    const Options& options_; // Declare options for this class instance.
    // SHARED REFERENCES: SystemController passes these existing objects to FileIO.
    // RESULT QUEUE: both methods publish into Main's one shared CommandQueue.
    CommandQueue& messages_;     // FileIO enqueues results/errors -> Main.
    // SAMPLE BUFFERS: transfer data in these methods; do disk I/O outside their locks.
    CircularBuffer& recording_;  // Shared recording-enabled flag; the buffer gate is checked under its own mutex too.
    CircularBuffer& playback_;   // FileIO PUSHES samples consumed by AudioDSP.

    // ================= MAILBOX: Main -> FileIO ===================
    // Storage is ONE Request struct + an occupied flag, protected by a mutex.
    // SEND: request() copies into the free slot and signals mailbox_ready_.
    // TAKE: take_request() copies it locally and clears the occupied flag under the mutex.
    // DO:   process_request() handles the local copy after releasing the mutex.
    // The condition variable wakes an idle FileIO worker; the actual request
    // remains in mailbox_. A signal itself does not contain or save a command.
    // Request handlers below run after take_request() has released the mailbox mutex.
    pthread_mutex_t mailbox_mutex_{}; // Protect the FileIO request struct, occupied flag and stopping state.
    pthread_cond_t mailbox_ready_{};  // Wake an idle FileIO worker; the request itself remains in mailbox_.
    Request mailbox_{};               // The one shared control-message slot.
    bool has_mailbox_request_ = false; // true = unprocessed request; false = free.
    // ============================================================
    bool stopping_ = false; // Initialize stopping to false.
    atomic<uint8_t> recording_error_{0}; // Remember a recording failure while remaining data is drained/finalized.
    string mounted_device_; // Device node currently associated with the configured storage mount point.
    bool storage_removed_ = false; // Remember surprise removal until the obsolete mount is released.
    pthread_t worker_{}; // POSIX pthread handle created by start and later passed to pthread_join.
    bool worker_started_ = false; // Track whether a pthread was actually created and must be joined.
    // ==================== PRIVATE HELPER METHODS ====================
    void set_file_thread_policy(); // Request CPU 2 affinity and SCHED_FIFO priority 50.
    bool storage_available() const; // Check whether the configured storage path is currently usable.
    void post_result(WorkerResultCode code, uint32_t operation_id, uint8_t error = 0); // Construct a MainMessage and enqueue completion or error through CommandQueue.
    void post_storage_status(WorkerResultCode code, uint32_t operation_id); // Derive storage capabilities in FileIO and report them to Main.
    bool write_recording(); // Pop recording samples and write the WAV.
    void read_playback();   // Read the WAV and push playback samples.
    int take_request(Request& request); // 1 = request, 0 = normal work, -1 = stop.
    void process_request(const Request& request); // Dispatch a FileIO control request after releasing the mailbox mutex.
    void finish_recording(const Request& request); // Drain and finalize a recording, then send its worker result to Main.
    void prepare_playback(const Request& request); // Validate the selected file identity, open the WAV and prefill the playback buffer.
    void list_archive(const Request& request); // Scan files and attach their identities to an archive-ready result.
    void mount_storage(const Request& request); // Validate and mount a supported USB filesystem in FileIO.
    void handle_removed_storage(const Request& request); // Stop affected file activity and report unavailable storage.
    static void* threadEntry(void* context); // Recover the object pointer passed to pthread_create and call its worker loop.

}; // End this block, declaration or argument list.
}  // namespace phono_app
