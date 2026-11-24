/**
 * FileIOTest.cpp - checks File IOTest with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - wait_result(): wait result.
 * - main(): run this standalone test and return success only if its checks pass.
 * - worker(): worker.
 */

// API DOCUMENTATION FOR THIS FILE:
// libsndfile: SNDFILE/SF_INFO, WAV open, sample I/O, errors and checked close:
// https://libsndfile.github.io/libsndfile/api.html
// POSIX descriptor open/flags; related read/write/close system calls:
// https://man7.org/linux/man-pages/man2/open.2.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


// Exercise the real FileIO worker, its mailbox and both real circular buffers.
#include "phonostethos/FileIoManager.h" // Include the FileIoManager class/types used here; implements th_FileIO: WAV recording/playback, archive management and FAT32 USB storage operations.
#include <sndfile.h> // Include libsndfile WAV handles, format descriptors and PCM read/write APIs.
#include <sys/stat.h> // Include file type/identity metadata and permission bits.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.
#include <chrono> // Include typed durations and monotonic deadlines.
#include <cstdio> // Include C-style file/console formatting used by this file.
#include <cstdlib> // Include process exit, allocation or numeric conversion functions.
#include <string> // Include owned filenames, device paths and diagnostic text.
#include <vector> // Include variable-size byte frames and archive entry lists.

using namespace phono_app; // Allow the names in this namespace without repeating its prefix.

static bool wait_result(CommandQueue& queue, WorkerResultCode code, uint32_t operation) { // Finish the parameters/condition and enter the block below.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); // Initialize deadline to std::chrono::steady_clock::now() + std::chrono::seconds(5).
    while (std::chrono::steady_clock::now() < deadline) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        MainMessage message; // Declare message for this operation.
        if (!queue.dequeue(message) || message.result_code != code || message.operation_id != operation) continue; // Test the condition !queue.dequeue(message) || message.result_code != code || message.operation_id != operation. If true, skip the rest of this pass and check the next loop iteration.
        if (message.error_code != 0) { // Test the condition message.error_code != 0.
            std::fprintf(stderr, "FAIL: worker returned error code %u\n", message.error_code); // Print the diagnostic or test result for the operator.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    std::fprintf(stderr, "FAIL: worker did not report its result within five seconds\n"); // Print the diagnostic or test result for the operator.
    return false; // Report failure or no available item to the caller.
} // End this block, declaration or argument list.

int main() { // Run this standalone test and return success only if its checks pass.
    char directory[] = "/tmp/phono-fileio-test-XXXXXX"; // Initialize directory to "/tmp/phono-fileio-test-XXXXXX".
    if (mkdtemp(directory) == nullptr) { std::perror("mkdtemp"); return 1; } // Test the condition mkdtemp(directory) == nullptr. If true, continue the expression or parameter list from the preceding line.
    Options options; // Declare options for this operation.
    options.storage = directory; // Set options.storage to directory.
    options.require_mounted_storage = false; // This system test uses an isolated temporary directory.
    std::string path = std::string(directory) + "/GENERAL_20261123_120000_000001.wav"; // Initialize path to std::string(directory) + "/GENERAL_20261123_120000_000001.wav".
    std::printf("Temporary test directory: %s\n", directory); // Print the diagnostic or test result for the operator.
    CommandQueue messages; // Declare messages for this operation.
    CircularBuffer recording_buffer; // Declare recording buffer for this operation.
    CircularBuffer playback_buffer; // Declare playback buffer for this operation.
    FileIoManager worker(options, messages, recording_buffer, playback_buffer); // Complete the call above with these remaining arguments.
    if (!worker.start() || !wait_result(messages, WorkerResultCode::FILEIO_READY, 0)) return 1; // Test the condition !worker.start() || !wait_result(messages, WorkerResultCode::FILEIO_READY, 0). If true, return 1 to the caller.

    // Main -> FileIO mailbox: ask the worker to open a recording file.
    FileIoManager::Request request; // Declare request for this operation.
    request.kind = FileIoManager::RequestKind::OPEN_RECORD; // Set request.kind to FileIoManager::RequestKind::OPEN_RECORD.
    request.operation_id = 1; // Set request.operation id to 1.
    request.path = path; // Set request.path to path.
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::OPEN_READY, 1)) return 1; // Test the condition !worker.request(request) || !wait_result(messages, WorkerResultCode::OPEN_READY, 1). If true, return 1 to the caller.
    short original[1041]; // Two full 512-sample blocks plus a final 17-sample tail.
    for (int i = 0; i < 1041; i++) original[i] = static_cast<short>(i - 500); // Visit the specified items/indexes in order.
    if (!recording_buffer.begin_session() || !recording_buffer.push(original, 1041)) return 1; // Test the condition !recording_buffer.begin_session() || !recording_buffer.push(original, 1041). If true, return 1 to the caller.
    recording_buffer.stop_producing(); // No samples can enter after STOP.
    request.kind = FileIoManager::RequestKind::CLOSE_RECORD; // Set request.kind to FileIoManager::RequestKind::CLOSE_RECORD.
    request.operation_id = 2; // Set request.operation id to 2.
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::RECORD_CLOSED, 2)) return 1; // Test the condition !worker.request(request) || !wait_result(messages, WorkerResultCode::RECORD_CLOSED, 2). If true, return 1 to the caller.

    // Read the saved file independently to check the worker's complete output.
    SF_INFO info{}; // Declare info for this operation.
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info); // Initialize file to sf_open(path.c_str(), SFM_READ, &info).
    if (file == nullptr) return 1; // Test the condition file == nullptr. If true, return 1 to the caller.
    short saved[1041]; // Declare saved for this operation.
    bool exact = info.frames == 1041 && info.samplerate == 8000 && info.channels == 1 && // Initialize exact to info.frames == 1041 && info.samplerate == 8000 && info.channels == 1 &&.
                 /* API: sf_read_short
                  * Documentation: https://libsndfile.github.io/libsndfile/api.html
                  * Read at most the requested count of PCM16 samples into the supplied array. Returns the actual count; a short read needs EOF/error classification. Mono samples equal frames.
                  */
                 sf_read_short(file, saved, 1041) == 1041; // Call sf_read_short with the arguments shown here.
    /* API: sf_close
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Finalize/release a libsndfile handle. A nonzero result indicates a close/finalization error. This project closes retained POSIX descriptors separately.
     */
    if (sf_close(file) != 0) exact = false; // Test the condition sf_close(file) != 0. If true, set exact to false.
    if (exact) { // Test the condition exact.
        for (int i = 0; i < 1041; i++) { // Visit the specified items/indexes in order.
            if (saved[i] != original[i]) exact = false; // Test the condition saved[i] != original[i]. If true, set exact to false.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    if (!exact || !recording_buffer.isEmpty()) { // Test the condition !exact || !recording_buffer.isEmpty().
        std::fprintf(stderr, "FAIL: recording file or final partial block differed\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    std::puts("PASS: recording mailbox -> buffer pop -> WAV write -> STOP drain/close, all 1041 samples saved"); // Complete the call above with these remaining arguments.

    struct stat metadata{}; // Allocate file metadata storage for the checked stat/fstat call.
    /* API: lstat
     * Documentation: https://man7.org/linux/man-pages/man2/lstat.2.html
     * Read path metadata without following its final symbolic link. Check the return and expected regular-file identity before file operations.
     */
    if (lstat(path.c_str(), &metadata) != 0) return 1; // Test the condition lstat(path.c_str(), &metadata) != 0. If true, return 1 to the caller.
    request.kind = FileIoManager::RequestKind::PREPARE_PLAYBACK; // Set request.kind to FileIoManager::RequestKind::PREPARE_PLAYBACK.
    request.operation_id = 3; // Set request.operation id to 3.
    request.identity.basename = "GENERAL_20261123_120000_000001.wav"; // Set request.identity.basename to "GENERAL_20261123_120000_000001.wav".
    request.identity.device = metadata.st_dev; // Set request.identity.device to metadata.st_dev.
    request.identity.inode = metadata.st_ino; // Set request.identity.inode to metadata.st_ino.
    request.identity.size = metadata.st_size; // Set request.identity.size to metadata.st_size.
    request.identity.modified = metadata.st_mtim; // Set request.identity.modified to metadata.st_mtim.
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::PLAYBACK_READY, 3)) return 1; // Test the condition !worker.request(request) || !wait_result(messages, WorkerResultCode::PLAYBACK_READY, 3). If true, return 1 to the caller.
    // FileIO pushes playback; this test consumes as AudioDSP would.
    std::vector<short> played; // Declare played for this operation.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); // Initialize deadline to std::chrono::steady_clock::now() + std::chrono::seconds(5).
    while (std::chrono::steady_clock::now() < deadline) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        short block[512]; // Declare block for this operation.
        size_t count = playback_buffer.pop(block, 512); // Initialize count to playback_buffer.pop(block, 512).
        for (size_t i = 0; i < count; i++) played.push_back(block[i]); // Visit the specified items/indexes in order.
        if (playback_buffer.stopped() && playback_buffer.isEmpty()) break; // Test the condition playback_buffer.stopped() && playback_buffer.isEmpty(). If true, leave the current loop or switch branch.
    } // End this block, declaration or argument list.
    if (played.size() != 1041) return 1; // Test the condition played.size() != 1041. If true, return 1 to the caller.
    for (int i = 0; i < 1041; i++) { // Visit the specified items/indexes in order.
        if (played[i] != original[i]) return 1; // Test the condition played[i] != original[i]. If true, return 1 to the caller.
    } // End this block, declaration or argument list.
    request.kind = FileIoManager::RequestKind::STOP_PLAYBACK; // Set request.kind to FileIoManager::RequestKind::STOP_PLAYBACK.
    request.operation_id = 4; // Set request.operation id to 4.
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::PLAYBACK_CLOSED, 4)) return 1; // Test the condition !worker.request(request) || !wait_result(messages, WorkerResultCode::PLAYBACK_CLOSED, 4). If true, return 1 to the caller.
    std::puts("PASS: playback mailbox -> WAV read -> playback buffer push -> exact samples and EOF"); // Complete the call above with these remaining arguments.
    request.kind = FileIoManager::RequestKind::DELETE; // Set request.kind to FileIoManager::RequestKind::DELETE.
    request.operation_id = 5; // Set request.operation id to 5.
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::RECORDING_DELETED, 5)) return 1; // Test the condition !worker.request(request) || !wait_result(messages, WorkerResultCode::RECORDING_DELETED, 5). If true, return 1 to the caller.
    worker.stop(); // Call worker.stop with the arguments shown here.
    if (access(path.c_str(), F_OK) == 0 || rmdir(directory) != 0) return 1; // Test the condition access(path.c_str(), F_OK) == 0 || rmdir(directory) != 0. If true, return 1 to the caller.
    std::puts("PASS: validated delete, worker stop and temporary-file cleanup"); // Complete the call above with these remaining arguments.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
