#pragma once

#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"
#include "phonostethos/DspProcessor.h"

#include <alsa/asoundlib.h>

#include <atomic>

#include "phonostethos/ThreadSupport.h"
#include <optional>

using namespace std;

namespace phono_app {
// ALSA owner and th_AudioDSP execution context; uses Main-owned DSP and buffers.
class AudioEngine {
public:
    enum class RequestKind { SuspendCapture, ResumeCapture, StartPlayback, StopPlayback };
    struct Request {
        RequestKind kind;
        uint32_t operation_id;
    };

    AudioEngine(const Options& options, DspProcessor& dsp, atomic<unsigned>& mode,
                atomic<unsigned>& gain, atomic<bool>& recording,
                atomic<unsigned>& error_code, CommandQueue& messages,
                DataQueue& telemetry,
                pthread_mutex_t& configuration_mutex,
                CircularBuffer<int16_t, kCircularBufferSamples>& recording_buffer,
                CircularBuffer<int16_t, kCircularBufferSamples>& playback_buffer,
                atomic<uint32_t>& dropped_telemetry,
                atomic<uint32_t>& recording_overruns);
    ~AudioEngine();

    // These PCM operations are called by th_AudioDSP, never by Main.
    bool initAlsa();
    int readFrames(int16_t* buffer, int frames);
    int writeFrames(const int16_t* buffer, int frames);
    bool suspendCapture();
    bool resumeCapture();

    bool request(Request request);
    bool ready() const;
    bool start();
    void stop();

private:
    static int configure_pcm(snd_pcm_t* pcm);
    static bool recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error);
    void set_audio_thread_policy();
    void run();
    void post_result(WorkerResultCode code, uint32_t operation_id,
                     uint8_t error = 0);
    bool apply_request(Request request,
                       bool& playback_active);

    const Options& options_;
    atomic<unsigned>& mode_;
    atomic<unsigned>& gain_;
    atomic<bool>& recording_;
    atomic<unsigned>& error_code_;
    CommandQueue& messages_;
    DataQueue& telemetry_;
    pthread_mutex_t& configuration_mutex_;
    CircularBuffer<int16_t, kCircularBufferSamples>& recording_buffer_;
    CircularBuffer<int16_t, kCircularBufferSamples>& playback_buffer_;
    atomic<uint32_t>& dropped_telemetry_;
    atomic<uint32_t>& recording_overruns_;
    DspProcessor& dsp_;
    atomic<bool> ready_{false};
    atomic<bool> stop_requested_{false};
    pthread_mutex_t request_mutex_{};
    optional<Request> request_;
    snd_pcm_t* capture_handle_ = nullptr;
    snd_pcm_t* playback_handle_ = nullptr;
    atomic<snd_pcm_t*> capture_pcm_{nullptr};
    pthread_mutex_t capture_mutex_{};
    static void* threadEntry(void* context);
    pthread_t worker_{};
    bool worker_started_ = false;
};
}  // namespace phono_app
