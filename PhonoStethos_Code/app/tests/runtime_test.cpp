/**
 * runtime_test.cpp - checks runtime with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - await_result(): await result.
 * - identity(): identity.
 * - main(): run this standalone test and return success only if its checks pass.
 * - fileio(): fileio.
 * - leds(): leds.
 * - led_bytes(): led bytes.
 */

// API DOCUMENTATION FOR THIS FILE:
// libsndfile: SNDFILE/SF_INFO, WAV open, sample I/O, errors and checked close:
// https://libsndfile.github.io/libsndfile/api.html
// POSIX descriptor open/flags; related read/write/close system calls:
// https://man7.org/linux/man-pages/man2/open.2.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include "phonostethos/DspProcessor.h" // Include the DspProcessor class/types used here; performs live band-pass filtering, gain/limiting and rolling FFT analysis.
#include "phonostethos/FileIoManager.h" // Include the FileIoManager class/types used here; implements th_FileIO: WAV recording/playback, archive management and FAT32 USB storage operations.
#include "phonostethos/LedDriver.h" // Include the LedDriver class/types used here; wraps the LED character-device descriptor in user space.

#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <cassert> // Include assertions that stop a test when an expected result is false.
#include <cmath> // Include trigonometry, square roots, logarithms and finite-value checks.
#include <cstdlib> // Include process exit, allocation or numeric conversion functions.
#include <fstream> // Include read mount information through a C++ input stream.
#include <sys/stat.h> // Include file type/identity metadata and permission bits.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.

using namespace phono_app; // Allow the names in this namespace without repeating its prefix.

namespace { // Keep these declarations in the project namespace and avoid global name collisions.
MainMessage await_result(CommandQueue& queue, WorkerResultCode result, uint32_t operation) { // Finish the parameters/condition and enter the block below.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); // Initialize deadline to std::chrono::steady_clock::now() + std::chrono::seconds(5).
    while (std::chrono::steady_clock::now() < deadline) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        MainMessage message; // Declare message for this operation.
        if (queue.dequeue(message) && message.result_code == result && // Test the condition queue.dequeue(message) && message.result_code == result &&.
            message.operation_id == operation) return message; // Continue checking the file/state comparison; no value is assigned here.
    } // End this block, declaration or argument list.
    assert(false && "Worker result did not arrive"); // Verify this expected result; fail the test if it is not satisfied.
    return {}; // Return {} to the caller.
} // End this block, declaration or argument list.

ArchiveIdentity identity(const std::filesystem::path& path) { // Finish the parameters/condition and enter the block below.
    struct stat metadata {}; // Allocate file metadata storage for the checked stat/fstat call.
    /* API: lstat
     * Documentation: https://man7.org/linux/man-pages/man2/lstat.2.html
     * Read path metadata without following its final symbolic link. Check the return and expected regular-file identity before file operations.
     */
    assert(lstat(path.c_str(), &metadata) == 0); // Verify this expected result; fail the test if it is not satisfied.
    return {path.filename().string(), metadata.st_dev, metadata.st_ino, // Return {path.filename().string(), metadata.st_dev, metadata.st_ino, to the caller.
            metadata.st_size, metadata.st_mtim}; // Complete this expression or declaration.
} // End this block, declaration or argument list.
} // End this block, declaration or argument list.

int main() { // Run this standalone test and return success only if its checks pass.
    // A known 500 Hz sine should produce a 0.25 peak at FFT bin 64.
    DspProcessor dsp; // Declare dsp for this operation.
    assert(dsp.initialize()); // Planning before any worker starts.
    std::array<int16_t, 1024> sine{}; // Complete this expression or declaration.
    for (size_t i = 0; i < sine.size(); ++i) { // Visit the specified items/indexes in order.
        sine[i] = static_cast<int16_t>(8192 * std::sin(2 * kPi * 500 * i / 8000)); // Set sine[i] to static_cast<int16_t>(8192 * std::sin(2 * kPi * 500 * i / 8000)).
    } // End this block, declaration or argument list.
    std::array<float, 512> spectrum{}; // Complete this expression or declaration.
    dsp.computeFFT(sine.data(), spectrum.data(), 512); // Call dsp.computeFFT with the arguments shown here.
    assert(!dsp.fftReady()); // Verify this expected result; fail the test if it is not satisfied.
    dsp.computeFFT(sine.data() + 512, spectrum.data(), 512); // Call dsp.computeFFT with the arguments shown here.
    assert(dsp.fftReady()); // Verify this expected result; fail the test if it is not satisfied.
    assert(std::abs(spectrum[64] - 0.25F) < 0.003F); // Verify this expected result; fail the test if it is not satisfied.
    for (size_t i = 0; i < sine.size(); ++i) { // Visit the specified items/indexes in order.
        assert(dsp.waveform()[i] == static_cast<float>(sine[i]) / 32768); // Verify this expected result; fail the test if it is not satisfied.
    } // End this block, declaration or argument list.
    std::array<float, 4> levels{{-40000, -12.5F, 12.5F, 40000}}; // Complete this expression or declaration.
    std::array<int16_t, 4> limited{}; // Complete this expression or declaration.
    dsp.applyGainWithLimiter(levels.data(), limited.data(), 4, 2); // Call dsp.applyGainWithLimiter with the arguments shown here.
    assert((limited == std::array<int16_t, 4>{{-32768, -25, 25, 32767}})); // Verify this expected result; fail the test if it is not satisfied.

    char directory[] = "/tmp/phonostethos-runtime-XXXXXX"; // Initialize directory to "/tmp/phonostethos-runtime-XXXXXX".
    assert(mkdtemp(directory)); // Verify this expected result; fail the test if it is not satisfied.
    Options options; // Declare options for this operation.
    options.storage = directory; // Set options.storage to directory.
    options.require_mounted_storage = false; // Set options.require mounted storage to false.
    const auto path = options.storage / "GENERAL_20261104_120000_000001.wav"; // Initialize path to options.storage / "GENERAL_20261104_120000_000001.wav".
    CommandQueue messages; // Declare messages for this operation.
    CircularBuffer recording, playback; // Declare recording for this operation.
    FileIoManager fileio(options, messages, recording, playback); // Complete the call above with these remaining arguments.
    assert(fileio.start()); // Verify this expected result; fail the test if it is not satisfied.
    await_result(messages, WorkerResultCode::FILEIO_READY, 0); // Call await_result with the arguments shown here.
    FileIoManager::Request request; // Declare request for this operation.
    request.kind = FileIoManager::RequestKind::OPEN_RECORD; // Set request.kind to FileIoManager::RequestKind::OPEN_RECORD.
    request.operation_id = 1; // Set request.operation id to 1.
    request.path = path; // Set request.path to path.
    assert(fileio.request(request)); // Verify this expected result; fail the test if it is not satisfied.
    assert(await_result(messages, WorkerResultCode::OPEN_READY, 1).error_code == 0); // Verify this expected result; fail the test if it is not satisfied.

    std::array<int16_t, 1041> original{}; // Two full periods and a final partial chunk.
    for (size_t i = 0; i < original.size(); ++i) original[i] = static_cast<int16_t>(i - 500); // Visit the specified items/indexes in order.
    assert(recording.begin_session()); // Verify this expected result; fail the test if it is not satisfied.
    assert(recording.push(original.data(), original.size())); // Verify this expected result; fail the test if it is not satisfied.
    recording.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    request.kind = FileIoManager::RequestKind::CLOSE_RECORD; // Set request.kind to FileIoManager::RequestKind::CLOSE_RECORD.
    request.operation_id = 2; // Set request.operation id to 2.
    assert(fileio.request(request)); // Verify this expected result; fail the test if it is not satisfied.
    assert(await_result(messages, WorkerResultCode::RECORD_CLOSED, 2).error_code == 0); // Verify this expected result; fail the test if it is not satisfied.
    assert(recording.isEmpty()); // Verify this expected result; fail the test if it is not satisfied.

    SF_INFO info{}; // Declare info for this operation.
    SNDFILE* check = sf_open(path.c_str(), SFM_READ, &info); // Initialize check to sf_open(path.c_str(), SFM_READ, &info).
    assert(check && info.frames == 1041 && info.channels == 1 && info.samplerate == 8000); // Verify this expected result; fail the test if it is not satisfied.
    std::array<int16_t, 1041> saved{}; // Complete this expression or declaration.
    /* API: sf_read_short
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Read at most the requested count of PCM16 samples into the supplied array. Returns the actual count; a short read needs EOF/error classification. Mono samples equal frames.
     */
    assert(sf_read_short(check, saved.data(), saved.size()) == 1041); // Verify this expected result; fail the test if it is not satisfied.
    assert(saved == original); // Verify this expected result; fail the test if it is not satisfied.
    /* API: sf_close
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Finalize/release a libsndfile handle. A nonzero result indicates a close/finalization error. This project closes retained POSIX descriptors separately.
     */
    assert(sf_close(check) == 0); // Verify this expected result; fail the test if it is not satisfied.

    request.kind = FileIoManager::RequestKind::PREPARE_PLAYBACK; // Set request.kind to FileIoManager::RequestKind::PREPARE_PLAYBACK.
    request.operation_id = 3; // Set request.operation id to 3.
    request.identity = identity(path); // Set request.identity to identity(path).
    assert(fileio.request(request)); // Verify this expected result; fail the test if it is not satisfied.
    assert(await_result(messages, WorkerResultCode::PLAYBACK_READY, 3).error_code == 0); // Verify this expected result; fail the test if it is not satisfied.
    std::vector<int16_t> played; // Declare played for this operation.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); // Initialize deadline to std::chrono::steady_clock::now() + std::chrono::seconds(5).
    while (std::chrono::steady_clock::now() < deadline) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        std::array<int16_t, 512> block{}; // Complete this expression or declaration.
        const auto count = playback.pop(block.data(), block.size()); // Initialize count to playback.pop(block.data(), block.size()).
        played.insert(played.end(), block.begin(), block.begin() + count); // Call played.insert with the arguments shown here.
        if (playback.stopped() && playback.isEmpty()) break; // Test the condition playback.stopped() && playback.isEmpty(). If true, leave the current loop or switch branch.
    } // End this block, declaration or argument list.
    assert(played == std::vector<int16_t>(original.begin(), original.end())); // Verify this expected result; fail the test if it is not satisfied.
    request.kind = FileIoManager::RequestKind::STOP_PLAYBACK; // Set request.kind to FileIoManager::RequestKind::STOP_PLAYBACK.
    request.operation_id = 4; // Set request.operation id to 4.
    assert(fileio.request(request)); // Verify this expected result; fail the test if it is not satisfied.
    assert(await_result(messages, WorkerResultCode::PLAYBACK_CLOSED, 4).error_code == 0); // Verify this expected result; fail the test if it is not satisfied.
    request.kind = FileIoManager::RequestKind::DELETE; // Set request.kind to FileIoManager::RequestKind::DELETE.
    request.operation_id = 5; // Set request.operation id to 5.
    assert(fileio.request(request)); // Verify this expected result; fail the test if it is not satisfied.
    assert(await_result(messages, WorkerResultCode::RECORDING_DELETED, 5).error_code == 0); // Verify this expected result; fail the test if it is not satisfied.
    assert(!std::filesystem::exists(path)); // Verify this expected result; fail the test if it is not satisfied.
    fileio.stop(); // Call fileio.stop with the arguments shown here.

    // Test user-space masks without touching GPIO or loading a module.
    const auto led_path = options.storage / "led-bytes"; // Initialize led path to options.storage / "led-bytes".
    { std::ofstream create(led_path); } // Continue the expression or parameter list from the preceding line.
    { // Begin this local scope; scoped locks release when it ends.
        LedDriver leds(led_path.string()); // Complete the call above with these remaining arguments.
        assert(leds.openDriver()); // Verify this expected result; fail the test if it is not satisfied.
        assert(leds.writeByte(0x05)); // HEART + GENERAL
        assert(leds.writeByte(0x08)); // RECORDING
        assert(leds.writeByte(0x00)); // All off
    }                                                // 00 on close
    std::ifstream led_bytes(led_path, std::ios::binary); // Complete the call above with these remaining arguments.
    const std::vector<unsigned char> actual((std::istreambuf_iterator<char>(led_bytes)), {}); // Complete the call above with these remaining arguments.
    assert((actual == std::vector<unsigned char>{0x05, 0x08, 0x00, 0x00})); // Verify this expected result; fail the test if it is not satisfied.
    std::filesystem::remove_all(options.storage); // Complete the call above with these remaining arguments.
} // End this block, declaration or argument list.
