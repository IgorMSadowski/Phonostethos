/**
 * FileIoManager.cpp - implements th_FileIO: WAV recording/playback, archive management and FAT32 USB storage operations.
 *
 * CONNECTIONS: FileIoManager.h declares the class; FileIoManager.cpp implements it. SystemController posts one-slot mailbox requests. FileIO pops recording samples, pushes playback samples and reports completion through CommandQueue.
 *
 * FUNCTIONS:
 * - ~FileIoManager(): release this object's resources after its users have stopped.
 * - FileIoManager(): connect the owned resources/references used by this class.
 * - unescape_mount_path(): decode Linux mountinfo escape sequences in a pathname.
 * - mounted_source(): read mountinfo to identify the device behind the storage mount point.
 * - mounts(): mounts.
 * - fields(): fields.
 * - storage_ready(): check the mount/path requirement, including the explicit host-test override.
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
 * - threadEntry(): recover the object pointer passed to pthread_create and call its worker loop.
 * - set_file_thread_policy(): request CPU 2 affinity and SCHED_FIFO priority 50.
 * - take_request(): copy a mailbox request locally and free its slot under the mailbox mutex.
 * - process_request(): dispatch a FileIO control request after releasing the mailbox mutex.
 * - finish_recording(): drain and finalize a recording, then send its worker result to Main.
 * - prepare_playback(): validate the selected file identity, open the WAV and prefill the playback buffer.
 * - list_archive(): scan files and attach their identities to an archive-ready result.
 * - mount_storage(): validate and mount a supported USB filesystem in FileIO.
 * - handle_removed_storage(): stop affected file activity and report unavailable storage.
 * - post_result(): construct a MainMessage and enqueue completion or error through CommandQueue.
 * - post_storage_status(): derive storage capabilities in FileIO and report them to Main.
 * - write_recording(): pop recorded samples and write/check them outside the sample-buffer mutex.
 * - read_playback(): read WAV samples and push them into the playback buffer for AudioDSP.
 * - storage_available(): check whether the configured storage path is currently usable.
 * - run(): execute this class's thread loop in the order shown in its flowchart.
 */

#include "phonostethos/FileIoManager.h" // Include the FileIoManager class/types used here; implements th_FileIO: WAV recording/playback, archive management and FAT32 USB storage operations.

#include "phonostethos/protocol.h" // Include the protocol class/types used here; defines and validates the client/server binary protocol.

#include <algorithm> // Include min/max, copying, sorting and other standard algorithms used in this file.
#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <cerrno> // Include errno and named system-call error codes.
#include <cstring> // Include byte copying/comparison and readable system-error strings.
#include <fcntl.h> // Include descriptor open flags such as O_CLOEXEC and exclusive creation.
#include <fstream> // Include read mount information through a C++ input stream.
#include <iostream> // Include console diagnostics and test results.
#include <pthread.h> // Include POSIX thread creation, mutexes and condition-variable APIs.
#include <sched.h> // Include CPU affinity masks and Linux scheduling policies/priorities.
#include <sstream> // Include parse the fields of a mount-info record.
#include <sys/mount.h> // Include filesystem mount and unmount calls used only by FileIO.
#include <sys/stat.h> // Include file type/identity metadata and permission bits.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.
#include <utility> // Include move owned payloads into queues without copying their contents.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
namespace { // Keep these declarations in the project namespace and avoid global name collisions.
string unescape_mount_path(string path) { // Decode Linux mountinfo escape sequences in a pathname.
    // /proc/mountinfo escapes spaces, tabs, newlines and backslashes.
    const char* codes[4] = {"\\040", "\\011", "\\012", "\\134"}; // Initialize codes to {"\\040", "\\011", "\\012", "\\134"}.
    const char characters[4] = {' ', '\t', '\n', '\\'}; // Initialize characters to {' ', '\t', '\n', '\\'}.
    for (int index = 0; index < 4; index++) { // Visit the specified items/indexes in order.
        size_t position = path.find(codes[index]); // Initialize position to path.find(codes[index]).
        while (position != string::npos) { // Repeat this block while its condition remains true; recheck shared state on each pass.
            path.replace(position, 4, 1, characters[index]); // Call path.replace with the arguments shown here.
            position = path.find(codes[index], position + 1); // Set position to path.find(codes[index], position + 1).
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    return path; // Return path to the caller.
} // End this block, declaration or argument list.

string mounted_source(const filesystem::path& path) { // Read mountinfo to identify the device behind the storage mount point.
    error_code error; // Declare error for this operation.
    const auto canonical_path = filesystem::canonical(path, error); // Initialize canonical path to filesystem::canonical(path, error).
    if (error) { // Test the condition error.
        return {}; // Return {} to the caller.
    } // End this block, declaration or argument list.
    ifstream mounts("/proc/self/mountinfo"); // Complete the call above with these remaining arguments.
    if (!mounts) { // Test the condition !mounts.
        return {}; // Return {} to the caller.
    } // End this block, declaration or argument list.
    string line; // Declare line for this operation.
    while (getline(mounts, line)) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        istringstream fields(line); // Complete the call above with these remaining arguments.
        string field; // Declare field for this operation.
        bool valid_line = true; // Initialize valid line to true.
        for (int index = 0; index < 5; ++index) { // Visit the specified items/indexes in order.
            if (!(fields >> field)) { // Test the condition !(fields >> field).
                valid_line = false; // Set valid line to false.
                break; // Leave the current loop or switch branch.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
        if (!valid_line || // Test the condition !valid_line ||.
            unescape_mount_path(field) != canonical_path.string()) { // Decode Linux mountinfo escape sequences in a pathname.
            continue; // Skip the rest of this pass and check the next loop iteration.
        } // End this block, declaration or argument list.
        while (fields >> field && field != "-") {} // Repeat this block while its condition remains true; recheck shared state on each pass.
        string filesystem_type; // Declare filesystem type for this operation.
        string source; // Declare source for this operation.
        if (fields >> filesystem_type >> source) { // Test the condition fields >> filesystem_type >> source.
            return source; // Return source to the caller.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    return {}; // Return {} to the caller.
} // End this block, declaration or argument list.
}  // namespace

bool storage_ready(const Options& options) { // Check the mount/path requirement, including the explicit host-test override.
    error_code error; // Declare error for this operation.
    if (!filesystem::is_directory(options.storage, error) || error) { // Test the condition !filesystem::is_directory(options.storage, error) || error.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    if (!options.require_mounted_storage) { // Test the condition !options.require_mounted_storage.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    const auto storage_path = filesystem::canonical(options.storage, error); // Initialize storage path to filesystem::canonical(options.storage, error).
    if (error) { // Test the condition error.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    ifstream mounts("/proc/self/mountinfo"); // Complete the call above with these remaining arguments.
    if (!mounts) { // Test the condition !mounts.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.

    string line; // Declare line for this operation.
    while (getline(mounts, line)) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        istringstream fields(line); // Complete the call above with these remaining arguments.
        string field; // Declare field for this operation.
        bool valid_line = true; // Initialize valid line to true.
        for (int index = 0; index < 5; ++index) { // Visit the specified items/indexes in order.
            if (!(fields >> field)) { // Test the condition !(fields >> field).
                valid_line = false; // Set valid line to false.
                break; // Leave the current loop or switch branch.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
        if (valid_line && // Test the condition valid_line &&.
            unescape_mount_path(field) == storage_path.string()) { // Decode Linux mountinfo escape sequences in a pathname.
            return true; // Report success, readiness or an available result to the caller.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    return false; // Report failure or no available item to the caller.
} // End this block, declaration or argument list.

FileIoManager::FileIoManager( // Begin this function/widget call; its arguments follow below.
    const Options& options, CommandQueue& messages, // Declare options for this operation.
    CircularBuffer& recording, // Declare recording for this operation.
    CircularBuffer& playback) // Declare playback for this operation.
    : options_(options), // Supply the next constructor initializer or argument.
      messages_(messages), // Call messages_ with the arguments shown here.
      recording_(recording), // Call recording_ with the arguments shown here.
      playback_(playback) { // Call playback_ with the arguments shown here.
    initializeMutex(mailbox_mutex_); // Initialize one pthread mutex and reject initialization failure.
    initializeCondition(mailbox_ready_); // Initialize a condition variable that uses CLOCK_MONOTONIC deadlines.
} // End this function or local scope.

FileIoManager::~FileIoManager() { // Finish the parameters/condition and enter the block below.
    stop(); // Request teardown and join workers before destroying their shared resources.
    pthread_cond_destroy(&mailbox_ready_); // Release this condition variable after its waiters have stopped.
    pthread_mutex_destroy(&mailbox_mutex_); // Release this initialized mutex after its users have stopped.
} // End this function or local scope.

// Called only by th_FileIO: no other thread touches these SNDFILE handles.
bool FileIoManager::openRecordFile(const string& filename) { // Create a new WAV exclusively so an existing recording is never overwritten.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    last_error_ = static_cast<uint8_t>(Error::StorageLost); // Set last error to static_cast<uint8_t>(Error::StorageLost).
    if (!storage_available() || recording_file_ || playback_file_) { // Test the condition !storage_available() || recording_file_ || playback_file_.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    /* API: ::open
     * Open the path with the requested flags. Returns a file descriptor or -1 with errno. O_EXCL with O_CREAT prevents overwriting an existing recording.
     */
    recording_fd_ = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); // Set recording fd to ::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600).
    if (recording_fd_ < 0) { // Test the condition recording_fd_ < 0.
        last_error_ = static_cast<uint8_t>(errno == ENOSPC ? Error::DiskFull : Error::OpenError); // Continue checking the file/state comparison; no value is assigned here.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    SF_INFO info{}; // Declare info for this operation.
    info.samplerate = kSampleRate; // Set info.samplerate to kSampleRate.
    info.channels = 1; // Set info.channels to 1.
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16; // Set info.format to SF_FORMAT_WAV | SF_FORMAT_PCM_16.
    /* API: sf_open_fd
     * Create a libsndfile handle around an open descriptor. Supply read/write mode, SF_INFO and whether libsndfile should close the descriptor. This project checks and closes its own descriptor separately.
     */
    recording_file_ = sf_open_fd(recording_fd_, SFM_WRITE, &info, SF_FALSE); // Set recording file to sf_open_fd(recording_fd_, SFM_WRITE, &info, SF_FALSE).
    if (!recording_file_) { // Test the condition !recording_file_.
        const int open_error = errno; // Initialize open error to errno.
        /* API: ::close
         * Release a descriptor after its owner has finished using it. Set the stored descriptor to -1 so it cannot be reused accidentally.
         */
        ::close(recording_fd_); // Release this POSIX descriptor; the owner resets its stored descriptor to -1.
        recording_fd_ = -1; // Set recording fd to -1.
        error_code ignored; // Declare ignored for this operation.
        filesystem::remove(filename, ignored); // Complete the call above with these remaining arguments.
        last_error_ = static_cast<uint8_t>(open_error == ENOSPC ? Error::DiskFull : Error::OpenError); // Continue checking the file/state comparison; no value is assigned here.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    recording_active_ = true; // Set recording active to true.
    recording_error_.store(0); // Update the shared recording error value atomically.
    last_error_ = 0; // Set last error to 0.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

bool FileIoManager::openPlaybackFile(const string& filename) { // Open the resolved WAV and reject an unsupported channel count, rate or sample format.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    last_error_ = static_cast<uint8_t>(Error::StorageLost); // Set last error to static_cast<uint8_t>(Error::StorageLost).
    if (!storage_available() || recording_file_ || playback_file_) { // Test the condition !storage_available() || recording_file_ || playback_file_.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    playback_fd_ = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW); // Set playback fd to ::open(filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW).
    if (playback_fd_ < 0) { // Test the condition playback_fd_ < 0.
        last_error_ = static_cast<uint8_t>(Error::ReadError); // Set last error to static_cast<uint8_t>(Error::ReadError).
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    SF_INFO info{}; // Declare info for this operation.
    playback_file_ = sf_open_fd(playback_fd_, SFM_READ, &info, SF_FALSE); // Set playback file to sf_open_fd(playback_fd_, SFM_READ, &info, SF_FALSE).
    last_error_ = static_cast<uint8_t>(Error::ReadError); // Set last error to static_cast<uint8_t>(Error::ReadError).
    if (!playback_file_) { closeFile(); return false; } // Test the condition !playback_file_. If true, continue the expression or parameter list from the preceding line.
    if (info.channels != 1 || info.samplerate != kSampleRate || // Test the condition info.channels != 1 || info.samplerate != kSampleRate ||.
        (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV || // Continue this calculation/condition with the next line.
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) { // Finish the parameters/condition and enter the block below.
        last_error_ = static_cast<uint8_t>(Error::UnsupportedWav); // Set last error to static_cast<uint8_t>(Error::UnsupportedWav).
        closeFile(); // Check libsndfile flush/finalization and close any open WAV handles/descriptors.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    last_error_ = 0; // Set last error to 0.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

sf_count_t FileIoManager::writeChunk(const int16_t* data, sf_count_t count) { // Write the requested PCM16 sample count through libsndfile and return the actual count.
    /* API: sf_write_short
     * Write the requested number of PCM16 sample items. Check that the returned count equals the requested count before accepting a successful recording write.
     */
    if (recording_file_ == nullptr) { // Test the condition recording_file_ == nullptr.
        return 0; // Return 0 to the caller.
    } // End this block, declaration or argument list.
    return sf_write_short(recording_file_, data, count); // Return sf_write_short(recording_file_, data, count) to the caller.
} // End this function or local scope.

sf_count_t FileIoManager::readChunk(int16_t* data, sf_count_t count) { // Read at most the requested PCM16 sample count through libsndfile.
    /* API: sf_read_short
     * Read PCM16 sample items. A short count can be the final block, zero can be EOF, and sf_error distinguishes a file error.
     */
    if (playback_file_ == nullptr) { // Test the condition playback_file_ == nullptr.
        return 0; // Return 0 to the caller.
    } // End this block, declaration or argument list.
    return sf_read_short(playback_file_, data, count); // Return sf_read_short(playback_file_, data, count) to the caller.
} // End this function or local scope.

bool FileIoManager::deleteRecordFile(const string& filename) { // Check the server-resolved basename and remove that recording.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    last_error_ = static_cast<uint8_t>(Error::StorageLost); // Set last error to static_cast<uint8_t>(Error::StorageLost).
    if (!storage_available() || recording_file_ || playback_file_) { // Test the condition !storage_available() || recording_file_ || playback_file_.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    // A basename is a name, never a path supplied by the GUI.
    if (filename.empty() || filename.find_first_of("/\\") != string::npos || filename == "." || filename == "..") { // Test the condition filename.empty() || filename.find_first_of("/\\") != string::npos || filename == "." || filename == "..".
        last_error_ = static_cast<uint8_t>(Error::InvalidArgument); // Set last error to static_cast<uint8_t>(Error::InvalidArgument).
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    error_code error; // Declare error for this operation.
    if (!filesystem::remove(options_.storage / filename, error) || error) { // Test the condition !filesystem::remove(options_.storage / filename, error) || error.
        last_error_ = static_cast<uint8_t>(Error::WriteError); // Set last error to static_cast<uint8_t>(Error::WriteError).
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    last_error_ = 0; // Set last error to 0.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

// The caller drains recording samples first; this method checks WAV finalization.
bool FileIoManager::closeFile() { // Check libsndfile flush/finalization and close any open WAV handles/descriptors.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    bool success = true; // Initialize success to true.
    if (recording_file_) { // Test the condition recording_file_.
        /* API: sf_write_sync
         * Flush libsndfile buffered recording data. Check sf_error and the final sf_close result as well.
         */
        sf_write_sync(recording_file_); // Call sf_write_sync with the arguments shown here.
        /* API: sf_error
         * Read the libsndfile error associated with a handle. Compare with SF_ERR_NO_ERROR before claiming a write/finalization succeeded.
         */
        if (sf_error(recording_file_) != SF_ERR_NO_ERROR) { // Test the condition sf_error(recording_file_) != SF_ERR_NO_ERROR.
            recording_error_.store(static_cast<uint8_t>(Error::WriteError)); // Update the shared recording error value atomically.
            success = false; // Set success to false.
        } // End this block, declaration or argument list.
        /* API: sf_close
         * Finalize the WAV header and release the libsndfile handle. Zero means success. FileIO owns this handle.
         */
        if (sf_close(recording_file_) != 0) { // Test the condition sf_close(recording_file_) != 0.
            if (recording_error_.load() == 0) recording_error_.store(static_cast<uint8_t>(Error::FinalizeError)); // Test the condition recording_error_.load() == 0. If true, publish the new shared recording error value atomically.
            success = false; // Set success to false.
        } // End this block, declaration or argument list.
        recording_file_ = nullptr; // Set recording file to nullptr.
    } // End this block, declaration or argument list.
    if (recording_fd_ >= 0) { // Test the condition recording_fd_ >= 0.
        if (::close(recording_fd_) != 0) success = false; // Test the condition ::close(recording_fd_) != 0. If true, set success to false.
        recording_fd_ = -1; // Set recording fd to -1.
    } // End this block, declaration or argument list.
    if (playback_file_) { // Test the condition playback_file_.
        if (sf_close(playback_file_) != 0) success = false; // Test the condition sf_close(playback_file_) != 0. If true, set success to false.
        playback_file_ = nullptr; // Set playback file to nullptr.
    } // End this block, declaration or argument list.
    if (playback_fd_ >= 0) { // Test the condition playback_fd_ >= 0.
        if (::close(playback_fd_) != 0) success = false; // Test the condition ::close(playback_fd_) != 0. If true, set success to false.
        playback_fd_ = -1; // Set playback fd to -1.
    } // End this block, declaration or argument list.
    return success; // Return success to the caller.
} // End this function or local scope.

vector<string> FileIoManager::getFileList() { // Scan storage and return sorted WAV basenames.
    vector<string> names; // Declare names for this operation.
    last_error_ = static_cast<uint8_t>(phono::protocol::Error::StorageLost); // Set last error to static_cast<uint8_t>(phono::protocol::Error::StorageLost).
    if (!storage_available()) { // Test the condition !storage_available().
        return names; // Return names to the caller.
    } // End this block, declaration or argument list.
    error_code error; // Declare error for this operation.
    for (filesystem::directory_iterator it(options_.storage, error), end; // Visit the specified items/indexes in order.
         !error && it != end; it.increment(error)) { // Finish the parameters/condition and enter the block below.
        if (it->path().extension() == ".wav") names.push_back(it->path().filename().string()); // Test the condition it->path().extension() == ".wav". If true, append the owned item at the back of this container.
    } // End this block, declaration or argument list.
    if (error) { // Test the condition error.
        last_error_ = static_cast<uint8_t>(phono::protocol::Error::ReadError); // Set last error to static_cast<uint8_t>(phono::protocol::Error::ReadError).
        return {}; // Return {} to the caller.
    } // End this block, declaration or argument list.
    sort(names.begin(), names.end()); // Call sort with the arguments shown here.
    last_error_ = 0; // Set last error to 0.
    return names; // Return names to the caller.
} // End this function or local scope.

bool FileIoManager::ejectStorage() { // Synchronize and unmount storage in FileIO, or request that operation from Main.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    last_error_ = static_cast<uint8_t>(Error::Busy); // Set last error to static_cast<uint8_t>(Error::Busy).
    if (recording_file_ || playback_file_) { // Test the condition recording_file_ || playback_file_.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    if (!storage_available()) { // Test the condition !storage_available().
        last_error_ = static_cast<uint8_t>(Error::StorageLost); // Set last error to static_cast<uint8_t>(Error::StorageLost).
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    last_error_ = static_cast<uint8_t>(Error::UnmountError); // Set last error to static_cast<uint8_t>(Error::UnmountError).
    const int fd = ::open(options_.storage.c_str(), O_RDONLY | O_DIRECTORY); // Initialize fd to ::open(options_.storage.c_str(), O_RDONLY | O_DIRECTORY).
    if (fd < 0) { // Test the condition fd < 0.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    /* API: ::syncfs
     * Synchronize the filesystem containing this open descriptor. Check its result before allowing safe USB removal.
     */
    const bool synced = ::syncfs(fd) == 0; // Initialize synced to ::syncfs(fd) == 0.
    const bool closed = ::close(fd) == 0; // Initialize closed to ::close(fd) == 0.
    /* API: ::umount2
     * Unmount the FAT32 mount point. Zero means success. Failure must not produce a safe-to-remove acknowledgement.
     */
    if (!synced || !closed || ::umount2(options_.storage.c_str(), 0) != 0) { // Test the condition !synced || !closed || ::umount2(options_.storage.c_str(), 0) != 0.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    last_error_ = 0; // Set last error to 0.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

bool FileIoManager::start() { // Create this worker pthread once and retain its handle for joining.
    if (worker_started_) { // Test the condition worker_started_.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    /* API: pthread_create
     * Start a POSIX worker. Pass its pthread_t address, attributes (NULL for defaults), entry callback and context pointer. Returns 0 or an error number.
     */
    const int result = pthread_create(&worker_, nullptr, &FileIoManager::threadEntry, this); // Initialize result to pthread_create(&worker_, nullptr, &FileIoManager::threadEntry, this).
    if (result != 0) { // Test the condition result != 0.
        cerr << "Cannot start FileIoManager: " << strerror(result) << '\n'; // Print the diagnostic or test result for the operator.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    worker_started_ = true; // Set worker started to true.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

void FileIoManager::stop() { // Request teardown and join workers before destroying their shared resources.
    { // Begin this local scope; scoped locks release when it ends.
        PthreadLock lock(mailbox_mutex_); // Lock this shared object until the local scope ends, including an early return.
        if (stopping_) { // Test the condition stopping_.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        stopping_ = true; // Set stopping to true.
    } // End this block, declaration or argument list.
    recording_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    playback_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    /* API: pthread_cond_broadcast
     * Wake all waiters so each can recheck the protected stop/data condition.
     */
    pthread_cond_broadcast(&mailbox_ready_); // Wake all waiters so each can recheck its protected data/stop predicate.
    if (worker_started_) { // Test the condition worker_started_.
        /* API: pthread_join
         * Wait for this worker to return before releasing the objects it uses. Pass the pthread handle and NULL when its return value is unused.
         */
        pthread_join(worker_, nullptr); // Call pthread_join with the arguments shown here.
        worker_started_ = false; // Set worker started to false.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// Publish one request under the mailbox mutex; file I/O happens after unlocking.
// CONTROL: Main deposits one request here. run() takes it and calls process_request().
bool FileIoManager::request(Request request) { // Put one control record into the worker mailbox if its slot is free.
    { // Begin this local scope; scoped locks release when it ends.
        PthreadLock lock(mailbox_mutex_); // Main owns the slot/flag lock until this block ends.
        if (stopping_ || has_mailbox_request_) { // Refuse stopping/full; never overwrite an unread request.
            return false; // Unlock through PthreadLock; the caller must handle rejection.
        } // End this block, declaration or argument list.
        // MAILBOX SEND (Main): copy the record while the mutex protects the slot.
        mailbox_ = request; // Set mailbox to request.
        has_mailbox_request_ = true; // Publish that this slot contains a request.
    } // End this block, declaration or argument list.
    /* API: pthread_cond_signal
     * Wake one thread waiting on this condition variable. Change the protected condition under its mutex first. The signal itself does not store data.
     */
    // Wake FileIO if it is in its idle condition wait. The request is in mailbox_,
    // not in this signal. If FileIO is doing work, it checks the slot next loop.
    pthread_cond_signal(&mailbox_ready_); // Wake one waiter; the protected record or sample buffer stores the actual data.
    return true; // Accepted into the slot; the operation will complete later.
} // End this function or local scope.

void* FileIoManager::threadEntry(void* context) { // Recover the object pointer passed to pthread_create and call its worker loop.
    static_cast<FileIoManager*>(context)->run(); // Complete the call above with these remaining arguments.
    return nullptr; // Return nullptr to the caller.
} // End this function or local scope.

void FileIoManager::set_file_thread_policy() { // Request CPU 2 affinity and SCHED_FIFO priority 50.
    cpu_set_t cpus; // Declare cpus for this operation.
    CPU_ZERO(&cpus); // Call CPU_ZERO with the arguments shown here.
    CPU_SET(2, &cpus); // Call CPU_SET with the arguments shown here.
    const int affinity_result = // Start initializing this local value from the expression on the next line.
        /* API: pthread_setaffinity_np
         * Ask Linux to run this pthread on the CPUs in cpu_set_t. pthread_self identifies the current worker. Check the returned error number.
         */
        /* API: pthread_self
         * Return the calling pthread handle, used when setting this worker's affinity or scheduling policy.
         */
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus); // Call pthread_setaffinity_np with the arguments shown here.
    if (affinity_result != 0) { // Test the condition affinity_result != 0.
        cerr << "FileIO thread CPU affinity not set: " // Print the diagnostic or test result for the operator.
                  << strerror(affinity_result) << '\n'; // Append this part of the diagnostic output.
    } // End this block, declaration or argument list.
    sched_param priority{}; // Declare priority for this operation.
    priority.sched_priority = 50; // Set priority.sched priority to 50.
    const int scheduling_result = // Start initializing this local value from the expression on the next line.
        /* API: pthread_setschedparam
         * Request the scheduling policy and sched_priority stored in sched_param. FIFO 80 is AudioDSP and FIFO 50 is FileIO. SCHED_OTHER requires priority 0.
         */
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority); // Call pthread_setschedparam with the arguments shown here.
    if (scheduling_result != 0) { // Test the condition scheduling_result != 0.
        cerr << "FileIO thread SCHED_FIFO unavailable: " // Print the diagnostic or test result for the operator.
                  << strerror(scheduling_result) << '\n'; // Append this part of the diagnostic output.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// MAILBOX RECEIVE: FileIO may wait when idle, then takes a request outside its I/O work.
// Return 1 = request copied, 0 = continue normal work, -1 = stop this worker.
int FileIoManager::take_request(Request& request) { // Copy a mailbox request locally and free its slot under the mailbox mutex.
    PthreadLock lock(mailbox_mutex_); // Protect the shared record, occupied flag and stop state.
    const auto deadline = waitDeadline(10); // Absolute deadline for this idle wait.
    while (!stopping_ && !has_mailbox_request_ && // Repeat this block while its condition remains true; recheck shared state on each pass.
           !recording_active_ && (!playback_active_ || playback_.isFull())) { // Finish the parameters/condition and enter the block below.
        // API: pthread_cond_timedwait releases this mutex while sleeping and
        // reacquires it on return. Recheck the conditions after every wake-up.
        const int result = pthread_cond_timedwait( // Release the held mutex while sleeping, then reacquire it before checking the predicate again.
            &mailbox_ready_, &mailbox_mutex_, &deadline); // Complete the call above with these remaining arguments.
        if (result == ETIMEDOUT) break; // End the idle wait and let the worker loop recheck work.
        checkPthread(result);           // Other pthread errors must not be ignored.
    } // End this block, declaration or argument list.
    if (has_mailbox_request_) {         // A mailbox request takes precedence over stopping.
        request = mailbox_;            // Copy the shared request into the local output record.
        has_mailbox_request_ = false;  // Make the shared slot available to Main again.
        return 1;                      // Unlock automatically before processing the request.
    } // End this block, declaration or argument list.
    if (stopping_) return -1;           // No queued request remains; tell run() to finish.
    return 0;                          // No request; continue recording/playback work unlocked.
} // End this function or local scope.

// Dispatch control independently of recording data: LIST/EJECT also work while idle.
// FLOWCHART: "Check/process pending FileIO control request" dispatches here.
void FileIoManager::process_request(const Request& request) { // Dispatch a FileIO control request after releasing the mailbox mutex.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    switch (request.kind) { // Choose one branch from the supplied command/state value.
        case RequestKind::OPEN_RECORD: { // Handle the RequestKind::OPEN_RECORD choice in this switch.
            const bool opened = openRecordFile(request.path.string()); // Initialize opened to openRecordFile(request.path.string()).
            post_result(WorkerResultCode::OPEN_READY, request.operation_id, // Call post_result with the arguments shown here.
                        opened ? 0 : last_error_); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        case RequestKind::CLOSE_RECORD: // Handle the RequestKind::CLOSE_RECORD choice in this switch.
            finish_recording(request); // Drain and finalize a recording, then send its worker result to Main.
            return; // Leave this function; any local scoped locks are released.
        case RequestKind::PREPARE_PLAYBACK: // Handle the RequestKind::PREPARE_PLAYBACK choice in this switch.
            prepare_playback(request); // Validate the selected file identity, open the WAV and prefill the playback buffer.
            return; // Leave this function; any local scoped locks are released.
        case RequestKind::STOP_PLAYBACK: { // Handle the RequestKind::STOP_PLAYBACK choice in this switch.
            playback_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
            playback_.clear(); // Discard the previous contents/state of this object.
            const bool success = closeFile(); // Initialize success to closeFile().
            playback_active_ = false; // Set playback active to false.
            post_result(WorkerResultCode::PLAYBACK_CLOSED, // Call post_result with the arguments shown here.
                        request.operation_id, // Pass this next argument/value to the call or initializer above.
                        success ? 0 // Continue the expression or parameter list from the preceding line.
                                : static_cast<uint8_t>(Error::FinalizeError)); // Supply the next constructor initializer or argument.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        case RequestKind::LIST: // Handle the RequestKind::LIST choice in this switch.
            list_archive(request); // Scan files and attach their identities to an archive-ready result.
            return; // Leave this function; any local scoped locks are released.
        case RequestKind::DELETE: { // Handle the RequestKind::DELETE choice in this switch.
            struct stat metadata {}; // Allocate file metadata storage for the checked stat/fstat call.
            const auto path = options_.storage / request.identity.basename; // Initialize path to options_.storage / request.identity.basename.
            /* API: lstat
             * Read path metadata without following its final symbolic link. Check the return and expected regular-file identity before file operations.
             */
            if (::lstat(path.c_str(), &metadata) != 0 || !same_file(request.identity, metadata)) { // Test the condition ::lstat(path.c_str(), &metadata) != 0 || !same_file(request.identity, metadata).
                post_result(WorkerResultCode::RECORDING_DELETED, request.operation_id, // Call post_result with the arguments shown here.
                            static_cast<uint8_t>(Error::StaleFileId)); // Complete the call above with these remaining arguments.
                return; // Leave this function; any local scoped locks are released.
            } // End this block, declaration or argument list.
            const bool deleted = deleteRecordFile(request.identity.basename); // Delete only the validated selected recording and retain the returned success/failure.
            post_result(WorkerResultCode::RECORDING_DELETED, request.operation_id, // Call post_result with the arguments shown here.
                        deleted ? 0 : last_error_); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.

        case RequestKind::EJECT: { // Handle the RequestKind::EJECT choice in this switch.
            const bool ejected = ejectStorage(); // Initialize ejected to ejectStorage().
            post_result(WorkerResultCode::STORAGE_EJECTED, request.operation_id, // Call post_result with the arguments shown here.
                        ejected ? 0 : last_error_); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
        case RequestKind::MOUNT: // Handle the RequestKind::MOUNT choice in this switch.
            mount_storage(request); // Validate and mount a supported USB filesystem in FileIO.
            return; // Leave this function; any local scoped locks are released.
        case RequestKind::DEVICE_REMOVED: // Handle the RequestKind::DEVICE_REMOVED choice in this switch.
            handle_removed_storage(request); // Stop affected file activity and report unavailable storage.
            return; // Leave this function; any local scoped locks are released.
        case RequestKind::REFRESH_STORAGE: // Handle the RequestKind::REFRESH_STORAGE choice in this switch.
            post_storage_status(WorkerResultCode::STORAGE_CHANGED, // Call post_storage_status with the arguments shown here.
                                request.operation_id); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// Stop path: drain accepted samples, close WAV, then report the result to Main.
void FileIoManager::finish_recording(const Request& request) { // Drain and finalize a recording, then send its worker result to Main.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    bool success = true; // Initialize success to true.
    while (!recording_.isEmpty() && success) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        success = write_recording(); // Set success to write_recording().
    } // End this block, declaration or argument list.
    if (!recording_.isEmpty()) recording_.clear(); // Test the condition !recording_.isEmpty(). If true, discard the previous contents/state of this object.
    if (!closeFile()) success = false; // Test the condition !closeFile(). If true, set success to false.
    recording_active_ = false; // Set recording active to false.
    uint8_t final_error = recording_error_.load(); // Read the shared atomic value into this local snapshot.
    if (final_error == 0 && !success) { // Test the condition final_error == 0 && !success.
        final_error = static_cast<uint8_t>(Error::FinalizeError); // Set final error to static_cast<uint8_t>(Error::FinalizeError).
    } // End this block, declaration or argument list.
    post_result(WorkerResultCode::RECORD_CLOSED, // Call post_result with the arguments shown here.
                request.operation_id, final_error); // Complete the call above with these remaining arguments.
    recording_operation_ = 0; // Set recording operation to 0.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// Playback path: validate the selected file, open it and fill playbackBuffer.
void FileIoManager::prepare_playback(const Request& request) { // Validate the selected file identity, open the WAV and prefill the playback buffer.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    if (!storage_available() || recording_file_ != nullptr || // Test the condition !storage_available() || recording_file_ != nullptr ||.
        playback_file_ != nullptr) { // Finish the parameters/condition and enter the block below.
        post_result(WorkerResultCode::PLAYBACK_READY, // Call post_result with the arguments shown here.
                    request.operation_id, // Pass this next argument/value to the call or initializer above.
                    static_cast<uint8_t>(Error::StorageLost)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto path = options_.storage / request.identity.basename; // Initialize path to options_.storage / request.identity.basename.
    struct stat metadata {}; // Allocate file metadata storage for the checked stat/fstat call.
    if (::lstat(path.c_str(), &metadata) != 0 || // Test the condition ::lstat(path.c_str(), &metadata) != 0 ||.
        !same_file(request.identity, metadata)) { // Finish the parameters/condition and enter the block below.
        post_result(WorkerResultCode::PLAYBACK_READY, // Call post_result with the arguments shown here.
                    request.operation_id, // Pass this next argument/value to the call or initializer above.
                    static_cast<uint8_t>(Error::StaleFileId)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (!openPlaybackFile(path.string())) { // Test the condition !openPlaybackFile(path.string()).
        post_result(WorkerResultCode::PLAYBACK_READY, request.operation_id, last_error_); // Construct a MainMessage and enqueue completion or error through CommandQueue.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    struct stat opened_metadata {}; // Allocate file metadata storage for the checked stat/fstat call.
    /* API: fstat
     * Read metadata for an open descriptor. Compare the saved archive identity before using a GUI file ID; a changed file must not be silently substituted.
     */
    if (::fstat(playback_fd_, &opened_metadata) != 0 || !same_file(request.identity, opened_metadata)) { // Test the condition ::fstat(playback_fd_, &opened_metadata) != 0 || !same_file(request.identity, opened_metadata).
        closeFile(); // Check libsndfile flush/finalization and close any open WAV handles/descriptors.
        post_result(WorkerResultCode::PLAYBACK_READY, request.operation_id, // Call post_result with the arguments shown here.
                    static_cast<uint8_t>(Error::StaleFileId)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    playback_.clear(); // Discard the previous contents/state of this object.
    if (!playback_.begin_session()) { // Test the condition !playback_.begin_session().
        sf_close(playback_file_); // Call sf_close with the arguments shown here.
        playback_file_ = nullptr; // Set playback file to nullptr.
        ::close(playback_fd_); // Release this POSIX descriptor; the owner resets its stored descriptor to -1.
        playback_fd_ = -1; // Set playback fd to -1.
        post_result(WorkerResultCode::PLAYBACK_READY, // Call post_result with the arguments shown here.
                    request.operation_id, // Pass this next argument/value to the call or initializer above.
                    static_cast<uint8_t>(Error::Busy)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    playback_active_ = true; // Set playback active to true.
    playback_operation_ = request.operation_id; // Set playback operation to request.operation_id.
    read_playback(); // Read WAV samples and push them into the playback buffer for AudioDSP.
    post_result(WorkerResultCode::PLAYBACK_READY, // Call post_result with the arguments shown here.
                request.operation_id); // Complete the call above with these remaining arguments.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// Build an archive reply with the identity of each current WAV file.
void FileIoManager::list_archive(const Request& request) { // Scan files and attach their identities to an archive-ready result.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    MainMessage message; // Declare message for this operation.
    message.kind = MessageKind::WORKER_RESULT; // Set message.kind to MessageKind::WORKER_RESULT.
    message.operation_id = request.operation_id; // Set message.operation id to request.operation_id.
    message.result_code = WorkerResultCode::ARCHIVE_READY; // Set message.result code to WorkerResultCode::ARCHIVE_READY.
    const auto filenames = getFileList(); // Initialize filenames to getFileList().
    if (last_error_ != 0) { // Test the condition last_error_ != 0.
        message.kind = MessageKind::WORKER_ERROR; // Set message.kind to MessageKind::WORKER_ERROR.
        message.error_code = last_error_; // Set message.error code to last_error_.
    } else { // Follow the alternative path when the previous condition is false.
        for (const auto& filename : filenames) { // Visit the specified items/indexes in order.
            struct stat metadata {}; // Allocate file metadata storage for the checked stat/fstat call.
            const auto path = options_.storage / filename; // Initialize path to options_.storage / filename.
            if (::lstat(path.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode)) { // Test the condition ::lstat(path.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode).
                message.archive.push_back({filename, metadata.st_dev, metadata.st_ino, // Append the owned item at the back of this container.
                                           metadata.st_size, metadata.st_mtim}); // Complete the call above with these remaining arguments.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    messages_.enqueue(move(message)); // Send this record through the named protected queue; the receiver handles it later.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// Mount/check the USB device in FileIO, keeping filesystem calls out of Main.
void FileIoManager::mount_storage(const Request& request) { // Validate and mount a supported USB filesystem in FileIO.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    if (!options_.require_mounted_storage) { // Test the condition !options_.require_mounted_storage.
        error_code mount_error; // Declare mount error for this operation.
        if (filesystem::is_directory(options_.storage, mount_error) && // Test the condition filesystem::is_directory(options_.storage, mount_error) &&.
            !mount_error) { // Finish the parameters/condition and enter the block below.
            mounted_device_ = request.path.string(); // Set mounted device to request.path.string().
            storage_removed_ = false; // Set storage removed to false.
            post_storage_status(WorkerResultCode::STORAGE_CHANGED, // Call post_storage_status with the arguments shown here.
                                request.operation_id); // Complete the call above with these remaining arguments.
            return; // Leave this function; any local scoped locks are released.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    if (storage_available()) { // Test the condition storage_available().
        if (mounted_device_.empty()) { // Test the condition mounted_device_.empty().
            mounted_device_ = mounted_source(options_.storage); // Set mounted device to mounted_source(options_.storage).
            if (mounted_device_.empty()) { // Test the condition mounted_device_.empty().
                mounted_device_ = request.path.string(); // Set mounted device to request.path.string().
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
        post_storage_status(WorkerResultCode::STORAGE_CHANGED, // Call post_storage_status with the arguments shown here.
                            request.operation_id); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (storage_removed_ && ::umount2(options_.storage.c_str(), 0) != 0 && // Test the condition storage_removed_ && ::umount2(options_.storage.c_str(), 0) != 0 &&.
        errno != EINVAL && errno != ENOENT) { // Finish the parameters/condition and enter the block below.
        cerr << "Cannot release removed storage mount: " // Print the diagnostic or test result for the operator.
                  << strerror(errno) << '\n'; // Append this part of the diagnostic output.
        post_result(WorkerResultCode::STORAGE_CHANGED, // Call post_result with the arguments shown here.
                    request.operation_id, // Pass this next argument/value to the call or initializer above.
                    static_cast<uint8_t>(Error::StorageLost)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (request.path.empty() || request.filesystem_type.empty()) { // Test the condition request.path.empty() || request.filesystem_type.empty().
        post_result(WorkerResultCode::STORAGE_CHANGED, // Call post_result with the arguments shown here.
                    request.operation_id, // Pass this next argument/value to the call or initializer above.
                    static_cast<uint8_t>(Error::InvalidArgument)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    /* API: mount
     * Mount the device on the configured directory with the selected filesystem/flags. Returns 0 or -1 with errno. FileIO performs this work outside audio/queue locks.
     */
    if (::mount(request.path.c_str(), options_.storage.c_str(), // Test the condition ::mount(request.path.c_str(), options_.storage.c_str(),.
                request.filesystem_type.c_str(), // Pass this next argument/value to the call or initializer above.
                MS_NODEV | MS_NOSUID | MS_NOEXEC, nullptr) != 0) { // Finish the parameters/condition and enter the block below.
        cerr << "Cannot mount " << request.path << " on " // Print the diagnostic or test result for the operator.
                  << options_.storage << ": " << strerror(errno) // Append this part of the diagnostic output.
                  << '\n'; // Append this part of the diagnostic output.
        post_result(WorkerResultCode::STORAGE_CHANGED, // Call post_result with the arguments shown here.
                    request.operation_id, // Pass this next argument/value to the call or initializer above.
                    static_cast<uint8_t>(Error::StorageLost)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    mounted_device_ = request.path.string(); // Set mounted device to request.path.string().
    storage_removed_ = false; // Set storage removed to false.
    post_storage_status(WorkerResultCode::STORAGE_CHANGED, // Call post_storage_status with the arguments shown here.
                        request.operation_id); // Complete the call above with these remaining arguments.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

// USB removal: stop producers, release open files and notify Main.
void FileIoManager::handle_removed_storage(const Request& request) { // Stop affected file activity and report unavailable storage.
    using phono::protocol::Error; // Bring this named type or value into the current scope.
    if (request.path.string() != mounted_device_) { // Test the condition request.path.string() != mounted_device_.
        post_storage_status(WorkerResultCode::STORAGE_CHANGED, // Call post_storage_status with the arguments shown here.
                            request.operation_id); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    storage_removed_ = true; // Set storage removed to true.
    recording_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    const bool had_recording = recording_file_ != nullptr; // Initialize had recording to recording_file_ != nullptr.
    uint8_t close_error = recording_error_.load(); // Read the shared atomic value into this local snapshot.
    if (recording_file_ != nullptr) { // Test the condition recording_file_ != nullptr.
        while (!recording_.isEmpty() && // Repeat this block while its condition remains true; recheck shared state on each pass.
               write_recording()) {} // Call write_recording with the arguments shown here.
        if (!recording_.isEmpty()) recording_.clear(); // Test the condition !recording_.isEmpty(). If true, discard the previous contents/state of this object.
        if (recording_error_.load() != 0) { // Test the condition recording_error_.load() != 0.
            close_error = recording_error_.load(); // Read the shared atomic value into this local snapshot.
        } // End this block, declaration or argument list.
        sf_write_sync(recording_file_); // Call sf_write_sync with the arguments shown here.
        if (sf_error(recording_file_) != SF_ERR_NO_ERROR && // Test the condition sf_error(recording_file_) != SF_ERR_NO_ERROR &&.
            close_error == 0) { // Continue checking the file/state comparison; no value is assigned here.
            close_error = static_cast<uint8_t>(Error::WriteError); // Set close error to static_cast<uint8_t>(Error::WriteError).
        } // End this block, declaration or argument list.
        if (sf_close(recording_file_) != 0 && close_error == 0) { // Test the condition sf_close(recording_file_) != 0 && close_error == 0.
            close_error = static_cast<uint8_t>(Error::FinalizeError); // Set close error to static_cast<uint8_t>(Error::FinalizeError).
        } // End this block, declaration or argument list.
        recording_file_ = nullptr; // Set recording file to nullptr.
    } // End this block, declaration or argument list.
    if (recording_fd_ >= 0) { // Test the condition recording_fd_ >= 0.
        if (::close(recording_fd_) != 0 && close_error == 0) { // Test the condition ::close(recording_fd_) != 0 && close_error == 0.
            close_error = static_cast<uint8_t>(Error::FinalizeError); // Set close error to static_cast<uint8_t>(Error::FinalizeError).
        } // End this block, declaration or argument list.
        recording_fd_ = -1; // Set recording fd to -1.
    } // End this block, declaration or argument list.
    recording_active_ = false; // Set recording active to false.
    if (had_recording) { // Test the condition had_recording.
        post_result(WorkerResultCode::RECORD_CLOSED, // Call post_result with the arguments shown here.
                    recording_operation_, close_error); // Declare close error for this operation.
        recording_operation_ = 0; // Set recording operation to 0.
    } // End this block, declaration or argument list.
    playback_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    playback_.clear(); // Discard the previous contents/state of this object.
    const bool had_playback = playback_file_ != nullptr; // Initialize had playback to playback_file_ != nullptr.
    uint8_t playback_close_error = 0; // Initialize playback close error to 0.
    if (playback_file_ != nullptr) { // Test the condition playback_file_ != nullptr.
        if (sf_close(playback_file_) != 0) { // Test the condition sf_close(playback_file_) != 0.
            playback_close_error = // Continue the expression or parameter list from the preceding line.
                static_cast<uint8_t>(Error::FinalizeError); // Complete the call above with these remaining arguments.
        } // End this block, declaration or argument list.
        playback_file_ = nullptr; // Set playback file to nullptr.
    } // End this block, declaration or argument list.
    if (playback_fd_ >= 0) { // Test the condition playback_fd_ >= 0.
        if (::close(playback_fd_) != 0 && playback_close_error == 0) { // Test the condition ::close(playback_fd_) != 0 && playback_close_error == 0.
            playback_close_error = // Continue the expression or parameter list from the preceding line.
                static_cast<uint8_t>(Error::FinalizeError); // Complete the call above with these remaining arguments.
        } // End this block, declaration or argument list.
        playback_fd_ = -1; // Set playback fd to -1.
    } // End this block, declaration or argument list.
    playback_active_ = false; // Set playback active to false.
    if (had_playback) { // Test the condition had_playback.
        post_result(WorkerResultCode::PLAYBACK_CLOSED, // Call post_result with the arguments shown here.
                    playback_operation_, playback_close_error); // Declare playback close error for this operation.
    } // End this block, declaration or argument list.
    playback_operation_ = 0; // Set playback operation to 0.
    mounted_device_.clear(); // Discard the previous contents/state of this object.
    if (::umount2(options_.storage.c_str(), 0) != 0 && errno != EINVAL && // Test the condition ::umount2(options_.storage.c_str(), 0) != 0 && errno != EINVAL &&.
        errno != ENOENT) { // Finish the parameters/condition and enter the block below.
        cerr << "Cannot unmount removed storage: " << strerror(errno) // Print the diagnostic or test result for the operator.
                  << '\n'; // Append this part of the diagnostic output.
    } // End this block, declaration or argument list.
    { // Begin this local scope; scoped locks release when it ends.
        MainMessage message; // Declare message for this operation.
        message.kind = MessageKind::WORKER_RESULT; // Set message.kind to MessageKind::WORKER_RESULT.
        message.operation_id = request.operation_id; // Set message.operation id to request.operation_id.
        message.result_code = WorkerResultCode::STORAGE_CHANGED; // Set message.result code to WorkerResultCode::STORAGE_CHANGED.
        messages_.enqueue(move(message)); // Send this record through the named protected queue; the receiver handles it later.
    } // End this block, declaration or argument list.
    return; // Leave this function; any local scoped locks are released.
} // End this function or local scope.

void FileIoManager::post_result(WorkerResultCode code, uint32_t operation_id, // Pass this next argument/value to the call or initializer above.
                                uint8_t error) { // Declare error for this operation.
    MainMessage message; // Declare message for this operation.
    message.kind = MessageKind::WORKER_RESULT; // Set message.kind to MessageKind::WORKER_RESULT.
    if (error != 0) { // Test the condition error != 0.
        message.kind = MessageKind::WORKER_ERROR; // Set message.kind to MessageKind::WORKER_ERROR.
    } // End this block, declaration or argument list.
    message.operation_id = operation_id; // Set message.operation id to operation_id.
    message.result_code = code; // Set message.result code to code.
    message.error_code = error; // Set message.error code to error.
    // QUEUE SEND: messages_ is the SAME command_queue_ that Main dequeues.
    // This reports completion/failure; it does not write into Main's pending_ record.
    messages_.enqueue(move(message)); // Send this record through the named protected queue; the receiver handles it later.
} // End this function or local scope.

void FileIoManager::post_storage_status(WorkerResultCode code, // Pass this next argument/value to the call or initializer above.
                                        uint32_t operation_id) { // Declare operation id for this operation.
    MainMessage message; // Declare message for this operation.
    message.kind = MessageKind::WORKER_RESULT; // Set message.kind to MessageKind::WORKER_RESULT.
    message.operation_id = operation_id; // Set message.operation id to operation_id.
    message.result_code = code; // Set message.result code to code.
    error_code error; // Declare error for this operation.
    if (!mounted_device_.empty() && !storage_removed_) { // Test the condition !mounted_device_.empty() && !storage_removed_.
        message.storage_flags |= phono::protocol::StoragePresent; // Update message.storage flags using |= phono::protocol::StoragePresent.
    } // End this block, declaration or argument list.
    if (storage_available()) { // Test the condition storage_available().
        message.storage_flags |= phono::protocol::StorageMounted; // Update message.storage flags using |= phono::protocol::StorageMounted.
        if (::access(options_.storage.c_str(), R_OK) == 0) { // Test the condition ::access(options_.storage.c_str(), R_OK) == 0.
            message.storage_flags |= phono::protocol::StorageReadable; // Update message.storage flags using |= phono::protocol::StorageReadable.
        } // End this block, declaration or argument list.
        if (::access(options_.storage.c_str(), W_OK) == 0) { // Test the condition ::access(options_.storage.c_str(), W_OK) == 0.
            message.storage_flags |= phono::protocol::StorageWritable; // Update message.storage flags using |= phono::protocol::StorageWritable.
        } // End this block, declaration or argument list.
        error.clear(); // Discard the previous contents/state of this object.
        const auto space = filesystem::space(options_.storage, error); // Initialize space to filesystem::space(options_.storage, error).
        if (!error && space.available >= 1024 * 1024) { // Test the condition !error && space.available >= 1024 * 1024.
            message.storage_flags |= phono::protocol::StorageSpaceOk; // Update message.storage flags using |= phono::protocol::StorageSpaceOk.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    messages_.enqueue(move(message)); // Send this record through the named protected queue; the receiver handles it later.
} // End this function or local scope.

// pop() releases its buffer mutex before the potentially slow disk write.
bool FileIoManager::write_recording() { // Pop recorded samples and write/check them outside the sample-buffer mutex.
    auto* file = recording_file_; // Initialize file to recording_file_.
    if (file == nullptr) { // Test the condition file == nullptr.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    if (!storage_available()) { // Test the condition !storage_available().
        recording_error_.store( // Publish the new shared recording error value atomically.
            static_cast<uint8_t>(phono::protocol::Error::StorageLost)); // Complete the call above with these remaining arguments.
        recording_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    array<int16_t, kPeriodFrames> samples{}; // Complete this expression or declaration.
    // BUFFER RECEIVE: recording_ aliases SystemController::recording_buffer_.
    // AudioDSP pushed the samples; pop unlocks before the disk write below.
    const auto count = recording_.pop(samples.data(), samples.size()); // Initialize count to recording_.pop(samples.data(), samples.size()).
    if (count == 0) { // Test the condition count == 0.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    const auto written = writeChunk(samples.data(), static_cast<sf_count_t>(count)); // Initialize written to writeChunk(samples.data(), static_cast<sf_count_t>(count)).
    if (written != static_cast<sf_count_t>(count)) { // Test the condition written != static_cast<sf_count_t>(count).
        const auto error = errno == ENOSPC // Initialize error to errno == ENOSPC.
                               ? phono::protocol::Error::DiskFull // Continue the expression or parameter list from the preceding line.
                               : phono::protocol::Error::WriteError; // Supply the next constructor initializer or argument.
        cerr << "Audio recording write failed: " << sf_strerror(file) << '\n'; // Print the diagnostic or test result for the operator.
        recording_error_.store(static_cast<uint8_t>(error)); // Update the shared recording error value atomically.
        recording_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

// Read only what fits: FileIO produces playback; AudioDSP consumes it.
void FileIoManager::read_playback() { // Read WAV samples and push them into the playback buffer for AudioDSP.
    auto* file = playback_file_; // Initialize file to playback_file_.
    const auto operation_id = playback_operation_; // Initialize operation id to playback_operation_.
    if (file == nullptr || !playback_active_) { // Test the condition file == nullptr || !playback_active_.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    const auto available = playback_.available_capacity(); // Initialize available to playback_.available_capacity().
    if (available == 0) { // Test the condition available == 0.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    array<int16_t, kPeriodFrames> samples{}; // Complete this expression or declaration.
    const auto requested = min(available, samples.size()); // Initialize requested to min(available, samples.size()).
    const auto count = readChunk(samples.data(), static_cast<sf_count_t>(requested)); // Initialize count to readChunk(samples.data(), static_cast<sf_count_t>(requested)).
    if (count < 0 || sf_error(file) != SF_ERR_NO_ERROR) { // Test the condition count < 0 || sf_error(file) != SF_ERR_NO_ERROR.
        cerr << "Playback WAV read failed: " << sf_strerror(file) << '\n'; // Print the diagnostic or test result for the operator.
        playback_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
        playback_active_ = false; // Set playback active to false.
        post_result(WorkerResultCode::PLAYBACK_FINISHED, operation_id, // Call post_result with the arguments shown here.
                    static_cast<uint8_t>( // Begin this function/widget call; its arguments follow below.
                        phono::protocol::Error::ReadError)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    if (count == 0) { // Test the condition count == 0.
        playback_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
        playback_active_ = false; // Set playback active to false.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    // BUFFER SEND: playback_ aliases SystemController::playback_buffer_.
    // AudioDSP will pop these samples; the buffer condition signals availability.
    if (!playback_.push(samples.data(), static_cast<size_t>(count))) { // Test the condition !playback_.push(samples.data(), static_cast<size_t>(count)).
        cerr << "Playback buffer changed while FileIO was producing samples\n"; // Print the diagnostic or test result for the operator.
    } // End this block, declaration or argument list.
} // End this function or local scope.

bool FileIoManager::storage_available() const { // Check whether the configured storage path is currently usable.
    return !storage_removed_ && storage_ready(options_); // Return !storage_removed_ && storage_ready(options_) to the caller.
} // End this function or local scope.

// This pthread owns all WAV handles and filesystem operations.
// FLOWCHART: th_FileIO -> take control request -> pop recording / push playback.
void FileIoManager::run() { // Execute this class's thread loop in the order shown in its flowchart.
    set_file_thread_policy(); // Request CPU 2 affinity and SCHED_FIFO priority 50.
    if (storage_available()) mounted_device_ = mounted_source(options_.storage); // Test the condition storage_available(). If true, set mounted device to mounted_source(options_.storage).
    post_storage_status(WorkerResultCode::FILEIO_READY, 0); // Derive storage capabilities in FileIO and report them to Main.
    while (true) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        Request request{}; // Local control record; shared mailbox data stays protected.
        int mailbox_result = take_request(request); // 1 = request, 0 = no request, -1 = stop.
        if (mailbox_result < 0) break; // Worker is stopping and has no pending mailbox item.
        if (mailbox_result > 0) { // Test the condition mailbox_result > 0.
            if (request.kind == RequestKind::OPEN_RECORD) { // Test the condition request.kind == RequestKind::OPEN_RECORD.
                recording_operation_ = request.operation_id; // Set recording operation to request.operation_id.
            } // End this block, declaration or argument list.
            // Mailbox mutex is already released. Slow file calls must not hold it.
            process_request(request); // Dispatch a FileIO control request after releasing the mailbox mutex.
        } // End this block, declaration or argument list.

        if (recording_active_ && recording_file_ != nullptr) { // Test the condition recording_active_ && recording_file_ != nullptr.
            if (!storage_available()) { // Test the condition !storage_available().
                recording_.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
                recording_.clear(); // Discard the previous contents/state of this object.
                sf_close(recording_file_); // Call sf_close with the arguments shown here.
                recording_file_ = nullptr; // Set recording file to nullptr.
                if (recording_fd_ >= 0) ::close(recording_fd_); // Test the condition recording_fd_ >= 0. If true, supply the next constructor initializer or argument.
                recording_fd_ = -1; // Set recording fd to -1.
                recording_active_ = false; // Set recording active to false.
                recording_error_.store( // Publish the new shared recording error value atomically.
                    static_cast<uint8_t>(phono::protocol::Error::StorageLost)); // Complete the call above with these remaining arguments.
                post_result(WorkerResultCode::RECORD_CLOSED, // Call post_result with the arguments shown here.
                            recording_operation_, // Name this field, choice or argument: recording operation.
                            static_cast<uint8_t>( // Begin this function/widget call; its arguments follow below.
                                phono::protocol::Error::StorageLost)); // Complete the call above with these remaining arguments.
            } else { // Follow the alternative path when the previous condition is false.
                if (!write_recording()) { // Test the condition !write_recording().
                    Request close_request; // Declare close request for this operation.
                    close_request.kind = RequestKind::CLOSE_RECORD; // Set close request.kind to RequestKind::CLOSE_RECORD.
                    close_request.operation_id = recording_operation_; // Set close request.operation id to recording_operation_.
                    process_request(close_request); // Dispatch a FileIO control request after releasing the mailbox mutex.
                } // End this block, declaration or argument list.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
        if (playback_active_) { // Test the condition playback_active_.
            read_playback(); // Read WAV samples and push them into the playback buffer for AudioDSP.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    if (recording_file_ != nullptr) { // Test the condition recording_file_ != nullptr.
        while (!recording_.isEmpty() && // Repeat this block while its condition remains true; recheck shared state on each pass.
               write_recording()) {} // Call write_recording with the arguments shown here.
        sf_write_sync(recording_file_); // Call sf_write_sync with the arguments shown here.
        sf_close(recording_file_); // Call sf_close with the arguments shown here.
    } // End this block, declaration or argument list.
    if (recording_fd_ >= 0) ::close(recording_fd_); // Test the condition recording_fd_ >= 0. If true, supply the next constructor initializer or argument.
    if (playback_file_ != nullptr) sf_close(playback_file_); // Test the condition playback_file_ != nullptr. If true, call sf_close with the arguments shown here.
    if (playback_fd_ >= 0) ::close(playback_fd_); // Test the condition playback_fd_ >= 0. If true, supply the next constructor initializer or argument.
} // End this function or local scope.


}  // namespace phono_app
