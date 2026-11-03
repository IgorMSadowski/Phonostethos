#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <time.h>

using namespace std;

namespace phono_app {

constexpr unsigned kSampleRate = 8000;
constexpr size_t kPeriodFrames = 512;
constexpr size_t kFftFrames = 1024;
constexpr size_t kFftBins = 512;
constexpr size_t kCircularBufferSamples = 4096;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kLedDevice = "/dev/phonostethos_leds";
extern atomic<bool> running;

enum class FilterMode : unsigned {
    General = 0,
    Heart = 1,
    Lung = 2,
};

struct Options {
    string bind_address = "10.42.0.3";
    uint16_t port = 8080;
    string capture_pcm = "default";
    string playback_pcm = "default";
    filesystem::path storage = "/media/usb_storage";
    string led_device = kLedDevice;
    bool require_mounted_storage = true;
};

bool storage_ready(const Options& options);

struct Telemetry {
    array<float, kFftFrames> waveform{};
    array<float, kFftBins> spectrum{};
};

struct ArchiveIdentity {
    string basename;
    dev_t device = 0;
    ino_t inode = 0;
    off_t size = 0;
    timespec modified{};
};

inline bool same_file(const ArchiveIdentity& identity,
                      const struct stat& metadata) {
    return S_ISREG(metadata.st_mode) && identity.device == metadata.st_dev &&
           identity.inode == metadata.st_ino && identity.size == metadata.st_size &&
           identity.modified.tv_sec == metadata.st_mtim.tv_sec &&
           identity.modified.tv_nsec == metadata.st_mtim.tv_nsec;
}

}  // namespace phono_app
