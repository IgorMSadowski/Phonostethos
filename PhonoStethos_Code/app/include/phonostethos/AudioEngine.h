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

using namespace std;

namespace phono_app {
class AudioEngine {
public:
    AudioEngine(const Options& options, atomic<unsigned>& mode,
                atomic<unsigned>& gain, atomic<bool>& recording,
                atomic<unsigned>& error_code, Telemetry& telemetry,
                mutex& configuration_mutex,
                mutex& telemetry_mutex,
                atomic<uint32_t>& dropped_telemetry,
                FileIOManager& writer);

    ~AudioEngine();

    bool ready() const;

    void stop();

    bool start_playback(const filesystem::path& path, int descriptor);

    void stop_playback();

    bool playing() const;

private:
    static int configure_pcm(snd_pcm_t* pcm);

    static bool recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error);

    void set_audio_thread_policy();

    void run();

    void play_file(const filesystem::path& path, int descriptor,
                   shared_ptr<promise<bool>> ready);

    const Options& options_;
    atomic<unsigned>& mode_;
    atomic<unsigned>& gain_;
    atomic<bool>& recording_;
    atomic<unsigned>& error_code_;
    Telemetry& telemetry_;
    mutex& configuration_mutex_;
    mutex& telemetry_mutex_;
    atomic<uint32_t>& dropped_telemetry_;
    FileIOManager& writer_;
    DspProcessor dsp_;
    atomic<bool> ready_{false};
    atomic<bool> stop_requested_{false};
    atomic<bool> playback_requested_{false};
    atomic<bool> playback_cancelled_{false};
    atomic<snd_pcm_t*> capture_pcm_{nullptr};
    mutex capture_mutex_;
    DataQueue playback_data_;
    atomic<bool> playback_eof_{false};
    mutex playback_mutex_;
    thread worker_;
    thread playback_thread_;
};
}  // namespace phono_app
