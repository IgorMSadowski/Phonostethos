#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <time.h>

namespace phono_app {

constexpr unsigned kSampleRate = 8000;
constexpr std::size_t kPeriodFrames = 512;
constexpr std::size_t kFftFrames = 1024;
constexpr std::size_t kFftBins = 512;
constexpr std::size_t kMaxPendingAudioBlocks = 7;
constexpr std::size_t kPlaybackCapacityBlocks = 8;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kLedDevice = "/dev/phonostethos_leds";
extern std::atomic<bool> running;

enum class FilterMode : unsigned {
    General = 0,
    Heart = 1,
    Lung = 2,
};

struct Options {
    std::string bind_address = "10.42.0.3";
    std::uint16_t port = 8080;
    std::string capture_pcm = "default";
    std::string playback_pcm = "default";
    std::filesystem::path storage = "/media/usb_storage";
    std::string led_device = kLedDevice;
    bool require_mounted_storage = true;
};

bool storage_ready(const Options& options);

struct Telemetry {
    std::array<float, kFftFrames> waveform{};
    std::array<float, kFftBins> spectrum{};
};

struct PlaybackBlock {
    std::array<std::int16_t, kPeriodFrames> samples{};
    std::size_t count = 0;
};

struct ArchiveIdentity {
    std::string basename;
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
