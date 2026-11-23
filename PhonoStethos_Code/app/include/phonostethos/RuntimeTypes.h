/**
 * RuntimeTypes.h - defines shared audio constants, runtime options, telemetry and archive identities.
 *
 * CONNECTIONS: This header is included by managers, queues and DSP classes. It declares running, which main.cpp defines. ArchiveIdentity lets FileIO reject a changed file instead of silently playing/deleting a different file with an old GUI ID.
 *
 * FUNCTIONS:
 * - storage_ready(): check the mount/path requirement, including the explicit host-test override.
 * - same_file(): compare saved file identity with current metadata before play/delete.
 */

#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <atomic> // Include shared flags and counters that must be read/written atomically.
#include <cstddef> // Include size_t for sizes and array indexes.
#include <cstdint> // Include integer types with exact bit widths used by PCM and the binary protocol.
#include <filesystem> // Include storage paths, directory scans and file metadata operations.
#include <string> // Include owned filenames, device paths and diagnostic text.
#include <sys/stat.h> // Include file type/identity metadata and permission bits.
#include <time.h> // Include timespec, monotonic clocks and timed sleeps.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.

constexpr unsigned kSampleRate = 8000; // Processing rate in samples per second: all live and stored audio uses 8000 Hz.
constexpr size_t kPeriodFrames = 512; // One processing block is 512 mono frames, or 64 ms at 8 kHz.
constexpr size_t kFftFrames = 1024; // The rolling FFT history contains 1024 samples (128 ms at 8 kHz).
constexpr size_t kFftBins = 512; // Transmit bins 0..511; the separate Nyquist output at 512 is omitted.
constexpr size_t kCircularBufferSamples = 4096; // Each independent recording/playback buffer has 4096 PCM16 sample slots.
constexpr double kPi = 3.14159265358979323846; // Pi used when converting filter frequency to radians per sample.
constexpr const char* kLedDevice = "/dev/phonostethos_leds"; // Default path exposed by the custom LED character driver.
extern atomic<bool> running; // Declare running for this operation.

enum class FilterMode : unsigned { // Give the following named choices distinct typed values.
    GENERAL = 0, // Set GENERAL to 0,.
    HEART = 1, // Set HEART to 1,.
    LUNG = 2, // Set LUNG to 2,.
}; // End this block, declaration or argument list.

struct Options { // Group the related fields in one plain data record.
    string bind_address = "10.42.0.3"; // Initialize bind address to "10.42.0.3".
    uint16_t port = 8080; // Initialize port to 8080.
    string capture_pcm = "plughw:CARD=HiFiAudio,DEV=0"; // Initialize capture pcm to "plughw:CARD=HiFiAudio,DEV=0".
    string playback_pcm = "plughw:CARD=Headphones,DEV=0"; // Initialize playback pcm to "plughw:CARD=Headphones,DEV=0".
    filesystem::path storage = "/media/usb_storage"; // Initialize storage to "/media/usb_storage".
    string led_device = kLedDevice; // Initialize led device to kLedDevice.
    bool require_mounted_storage = true; // Initialize require mounted storage to true.
}; // End this block, declaration or argument list.

bool storage_ready(const Options& options); // Check the mount/path requirement, including the explicit host-test override.

struct DataPacket { // Group the related fields in one plain data record.
    array<float, kFftFrames> waveform{}; // The 1024 normalized, unwindowed processed samples sent to the GUI.
    array<float, kFftBins> fft_magnitudes{}; // 512 linear peak amplitudes for bins 0..511; framing is added only during serialization.
}; // End this block, declaration or argument list.

// A file ID resolves to this snapshot. Before play/delete, FileIO checks that
// the file still matches (USB removal/replacement must not select a different file).
struct ArchiveIdentity { // Group the related fields in one plain data record.
    string basename; // Declare basename for this operation.
    dev_t device = 0; // Initialize device to 0.
    ino_t inode = 0; // Initialize inode to 0.
    off_t size = 0; // Initialize size to 0.
    timespec modified{}; // Declare modified for this operation.
}; // End this block, declaration or argument list.

inline bool same_file(const ArchiveIdentity& identity, // Pass this next argument/value to the call or initializer above.
                      const struct stat& metadata) { // Finish the parameters/condition and enter the block below.
    return S_ISREG(metadata.st_mode) && identity.device == metadata.st_dev && // Return S_ISREG(metadata.st_mode) && identity.device == metadata.st_dev && to the caller.
           identity.inode == metadata.st_ino && identity.size == metadata.st_size && // Continue checking the file/state comparison; no value is assigned here.
           identity.modified.tv_sec == metadata.st_mtim.tv_sec && // Continue checking the file/state comparison; no value is assigned here.
           identity.modified.tv_nsec == metadata.st_mtim.tv_nsec; // Continue checking the file/state comparison; no value is assigned here.
} // End this block, declaration or argument list.

}  // namespace phono_app
