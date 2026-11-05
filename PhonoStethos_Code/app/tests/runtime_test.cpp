#include "phonostethos/DspProcessor.h"
#include "phonostethos/FileIoManager.h"
#include "phonostethos/LedDriver.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace phono_app;

namespace {
MainMessage await_result(CommandQueue& queue, WorkerResultCode result, uint32_t operation) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        MainMessage message;
        if (queue.dequeue(message) && message.result_code == result &&
            message.operation_id == operation) return message;
    }
    assert(false && "Worker result did not arrive");
    return {};
}

ArchiveIdentity identity(const std::filesystem::path& path) {
    struct stat metadata {};
    assert(lstat(path.c_str(), &metadata) == 0);
    return {path.filename().string(), metadata.st_dev, metadata.st_ino,
            metadata.st_size, metadata.st_mtim};
}
}

int main() {
    // A known 500 Hz sine should produce a 0.25 peak at FFT bin 64.
    DspProcessor dsp;
    assert(dsp.initialize()); // Planning before any worker starts.
    std::array<int16_t, 1024> sine{};
    for (size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<int16_t>(8192 * std::sin(2 * kPi * 500 * i / 8000));
    }
    std::array<float, 512> spectrum{};
    dsp.computeFFT(sine.data(), spectrum.data(), 512);
    assert(!dsp.fftReady());
    dsp.computeFFT(sine.data() + 512, spectrum.data(), 512);
    assert(dsp.fftReady());
    assert(std::abs(spectrum[64] - 0.25F) < 0.003F);
    for (size_t i = 0; i < sine.size(); ++i) {
        assert(dsp.waveform()[i] == static_cast<float>(sine[i]) / 32768);
    }
    std::array<float, 4> levels{{-40000, -12.5F, 12.5F, 40000}};
    std::array<int16_t, 4> limited{};
    dsp.applyGainWithLimiter(levels.data(), limited.data(), 4, 2);
    assert((limited == std::array<int16_t, 4>{{-32768, -25, 25, 32767}}));

    char directory[] = "/tmp/phonostethos-runtime-XXXXXX";
    assert(mkdtemp(directory));
    Options options;
    options.storage = directory;
    options.require_mounted_storage = false;
    const auto path = options.storage / "GENERAL_20261104_120000_000001.wav";
    CommandQueue messages;
    CircularBuffer<int16_t, kCircularBufferSamples> recording, playback;
    FileIoManager fileio(options, messages, recording, playback);
    assert(fileio.start());
    await_result(messages, WorkerResultCode::FileIOReady, 0);
    FileIoManager::Request request;
    request.kind = FileIoManager::RequestKind::OpenRecording;
    request.operation_id = 1;
    request.path = path;
    assert(fileio.request(request));
    assert(await_result(messages, WorkerResultCode::RecordingOpened, 1).error_code == 0);

    std::array<int16_t, 1041> original{}; // Two full periods and a final partial chunk.
    for (size_t i = 0; i < original.size(); ++i) original[i] = static_cast<int16_t>(i - 500);
    assert(recording.begin_session());
    assert(recording.push(original.data(), original.size()));
    recording.stop_producing();
    request.kind = FileIoManager::RequestKind::CloseRecording;
    request.operation_id = 2;
    assert(fileio.request(request));
    assert(await_result(messages, WorkerResultCode::RecordingClosed, 2).error_code == 0);
    assert(recording.isEmpty());

    SF_INFO info{};
    SNDFILE* check = sf_open(path.c_str(), SFM_READ, &info);
    assert(check && info.frames == 1041 && info.channels == 1 && info.samplerate == 8000);
    std::array<int16_t, 1041> saved{};
    assert(sf_read_short(check, saved.data(), saved.size()) == 1041);
    assert(saved == original);
    assert(sf_close(check) == 0);

    request.kind = FileIoManager::RequestKind::PreparePlayback;
    request.operation_id = 3;
    request.identity = identity(path);
    assert(fileio.request(request));
    assert(await_result(messages, WorkerResultCode::PlaybackReady, 3).error_code == 0);
    std::vector<int16_t> played;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<int16_t, 512> block{};
        const auto count = playback.pop(block.data(), block.size());
        played.insert(played.end(), block.begin(), block.begin() + count);
        if (playback.stopped() && playback.isEmpty()) break;
    }
    assert(played == std::vector<int16_t>(original.begin(), original.end()));
    request.kind = FileIoManager::RequestKind::StopPlayback;
    request.operation_id = 4;
    assert(fileio.request(request));
    assert(await_result(messages, WorkerResultCode::PlaybackClosed, 4).error_code == 0);
    request.kind = FileIoManager::RequestKind::Delete;
    request.operation_id = 5;
    assert(fileio.request(request));
    assert(await_result(messages, WorkerResultCode::RecordingDeleted, 5).error_code == 0);
    assert(!std::filesystem::exists(path));
    fileio.stop();

    // Test user-space masks without touching GPIO or loading a module.
    const auto led_path = options.storage / "led-bytes";
    { std::ofstream create(led_path); }
    {
        LedDriver leds(led_path.string());
        assert(leds.openDriver());
        assert(leds.setModeLed(FilterMode::General)); // 04
        assert(leds.setRecordLed(true));             // 0c
        assert(leds.setModeLed(FilterMode::Heart));   // 09
        assert(leds.setRecordLed(false));            // 01
        assert(leds.triggerErrorPulse());            // ff
    }                                                // 00 on close
    std::ifstream led_bytes(led_path, std::ios::binary);
    const std::vector<unsigned char> actual((std::istreambuf_iterator<char>(led_bytes)), {});
    assert((actual == std::vector<unsigned char>{4, 12, 9, 1, 255, 0}));
    std::filesystem::remove_all(options.storage);
}
