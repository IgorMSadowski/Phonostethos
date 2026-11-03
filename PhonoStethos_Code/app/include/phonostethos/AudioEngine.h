#pragma once

#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"
#include "phonostethos/DspProcessor.h"

#include <alsa/asoundlib.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

using namespace std;

namespace phono_app {
class AudioEngine {
public:
    enum class RequestKind { SuspendCapture, ResumeCapture, StartPlayback, StopPlayback };
    struct Request {
        RequestKind kind;
        uint32_t operation_id;
    };

    AudioEngine(const Options& options, atomic<unsigned>& mode,
                atomic<unsigned>& gain, atomic<bool>& recording,
                atomic<unsigned>& error_code, CommandQueue& messages,
                DataQueue& telemetry,
                mutex& configuration_mutex,
                CircularBuffer<int16_t, kCircularBufferSamples>& recording_buffer,
                CircularBuffer<int16_t, kCircularBufferSamples>& playback_buffer,
                atomic<uint32_t>& dropped_telemetry,
                atomic<uint32_t>& recording_overruns);
    ~AudioEngine();

    bool request(Request request);
    bool ready() const;
    void stop();

private:
    static int configure_pcm(snd_pcm_t* pcm);
    static bool recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error);
    void set_audio_thread_policy();
    void run();
    void post_result(WorkerResultCode code, uint32_t operation_id,
                     uint8_t error = 0);
    bool apply_request(snd_pcm_t*& capture, Request request,
                       bool& playback_active);

    const Options& options_;
    atomic<unsigned>& mode_;
    atomic<unsigned>& gain_;
    atomic<bool>& recording_;
    atomic<unsigned>& error_code_;
    CommandQueue& messages_;
    DataQueue& telemetry_;
    mutex& configuration_mutex_;
    CircularBuffer<int16_t, kCircularBufferSamples>& recording_buffer_;
    CircularBuffer<int16_t, kCircularBufferSamples>& playback_buffer_;
    atomic<uint32_t>& dropped_telemetry_;
    atomic<uint32_t>& recording_overruns_;
    DspProcessor dsp_;
    atomic<bool> ready_{false};
    atomic<bool> stop_requested_{false};
    mutex request_mutex_;
    optional<Request> request_;
    atomic<snd_pcm_t*> capture_pcm_{nullptr};
    mutex capture_mutex_;
    thread worker_;
};
}  // namespace phono_app
