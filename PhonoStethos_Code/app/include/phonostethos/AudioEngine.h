#pragma once

#include "phonostethos/DspProcessor.h"
#include "phonostethos/DataQueue.h"
#include "phonostethos/FileIOManager.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace phono_app {
class AudioEngine {
public:
    AudioEngine(const Options& options, std::atomic<unsigned>& mode,
                std::atomic<unsigned>& gain, std::atomic<bool>& recording,
                std::atomic<unsigned>& error_code, Telemetry& telemetry,
                std::mutex& configuration_mutex,
                std::mutex& telemetry_mutex,
                std::atomic<std::uint32_t>& dropped_telemetry,
                FileIOManager& writer);

    ~AudioEngine();

    bool ready() const;

    void stop();

    bool start_playback(const std::filesystem::path& path, int descriptor);

    void stop_playback();

    bool playing() const;

private:
    static int configure_pcm(snd_pcm_t* pcm);

    static bool recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error);

    void set_audio_thread_policy();

    void run();

    void play_file(const std::filesystem::path& path, int descriptor,
                   std::shared_ptr<std::promise<bool>> ready);

    const Options& options_;
    std::atomic<unsigned>& mode_;
    std::atomic<unsigned>& gain_;
    std::atomic<bool>& recording_;
    std::atomic<unsigned>& error_code_;
    Telemetry& telemetry_;
    std::mutex& configuration_mutex_;
    std::mutex& telemetry_mutex_;
    std::atomic<std::uint32_t>& dropped_telemetry_;
    FileIOManager& writer_;
    DspProcessor dsp_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> playback_requested_{false};
    std::atomic<bool> playback_cancelled_{false};
    std::atomic<snd_pcm_t*> capture_pcm_{nullptr};
    DataQueue playback_data_;
    std::atomic<bool> playback_eof_{false};
    std::mutex playback_mutex_;
    std::thread worker_;
    std::thread playback_thread_;
};
}  // namespace phono_app
