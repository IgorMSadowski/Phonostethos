#include "phonostethos/AudioEngine.h"

#include "phonostethos/protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <sched.h>

using namespace std;

namespace phono_app {
AudioEngine::AudioEngine(
    const Options& options, DspProcessor& dsp, atomic<unsigned>& mode, atomic<unsigned>& gain,
    atomic<bool>& recording, atomic<unsigned>& error_code,
    CommandQueue& messages, DataQueue& telemetry,
    pthread_mutex_t& configuration_mutex,
    CircularBuffer<int16_t, kCircularBufferSamples>& recording_buffer,
    CircularBuffer<int16_t, kCircularBufferSamples>& playback_buffer,
    atomic<uint32_t>& dropped_telemetry,
    atomic<uint32_t>& recording_overruns)
    : options_(options),
      mode_(mode),
      gain_(gain),
      recording_(recording),
      error_code_(error_code),
      messages_(messages),
      telemetry_(telemetry),
      configuration_mutex_(configuration_mutex),
      recording_buffer_(recording_buffer),
      playback_buffer_(playback_buffer),
      dropped_telemetry_(dropped_telemetry),
      recording_overruns_(recording_overruns),
      dsp_(dsp) {
    initializeMutex(request_mutex_);
    initializeMutex(capture_mutex_);
}

AudioEngine::~AudioEngine() {
    stop();
    pthread_mutex_destroy(&request_mutex_);
    pthread_mutex_destroy(&capture_mutex_);
}

// pthread callback: context is the AudioEngine whose start() created this worker.
void* AudioEngine::threadEntry(void* context) {
    static_cast<AudioEngine*>(context)->run();
    return nullptr;
}

bool AudioEngine::start() {
    if (worker_started_) return true;
    const int result = pthread_create(&worker_, nullptr, &AudioEngine::threadEntry, this);
    if (result != 0) {
        cerr << "Cannot start AudioEngine: " << strerror(result) << '\n';
        return false;
    }
    worker_started_ = true;
    return true;
}

// A mailbox holds one unprocessed control request; Main never changes ALSA directly.
bool AudioEngine::request(Request request) {
    PthreadLock lock(request_mutex_);
    if (stop_requested_.load() || request_.has_value()) return false;
    request_ = request;
    return true;
}

bool AudioEngine::ready() const { return ready_.load(); }

void AudioEngine::stop() {
    stop_requested_.store(true);
    {
        PthreadLock lock(capture_mutex_);
        if (auto* capture = capture_pcm_.load(); capture != nullptr) {
            (void)snd_pcm_drop(capture);
        }
    }
    if (worker_started_) {
        pthread_join(worker_, nullptr);
        worker_started_ = false;
    }
}

int AudioEngine::configure_pcm(snd_pcm_t* pcm) {
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    int err = snd_pcm_hw_params_any(pcm, params);
    if (err < 0) return err;
    if ((err = snd_pcm_hw_params_set_access(
             pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(
             pcm, params, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, params, 1)) < 0) {
        return err;
    }
    unsigned rate = kSampleRate;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, params, &rate, nullptr)) <
        0) {
        return err;
    }
    if (rate != kSampleRate) return -EINVAL;
    snd_pcm_uframes_t period = kPeriodFrames;
    if ((err = snd_pcm_hw_params_set_period_size_near(
             pcm, params, &period, nullptr)) < 0) {
        return err;
    }
    snd_pcm_uframes_t buffer = period * 4;
    if ((err = snd_pcm_hw_params_set_buffer_size_near(
             pcm, params, &buffer)) < 0) {
        return err;
    }
    if ((err = snd_pcm_hw_params(pcm, params)) < 0) return err;
    snd_pcm_sw_params_t* software;
    snd_pcm_sw_params_alloca(&software);
    if ((err = snd_pcm_sw_params_current(pcm, software)) < 0 ||
        (err = snd_pcm_sw_params_set_avail_min(pcm, software, period)) < 0 ||
        (err = snd_pcm_sw_params_set_stop_threshold(
             pcm, software, buffer)) < 0) {
        return err;
    }
    if (snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK &&
        (err = snd_pcm_sw_params_set_start_threshold(
             pcm, software, period)) < 0) {
        return err;
    }
    if ((err = snd_pcm_sw_params(pcm, software)) < 0) return err;
    return snd_pcm_prepare(pcm);
}

bool AudioEngine::recover_pcm(snd_pcm_t* pcm, snd_pcm_sframes_t error) {
    if (error == -EPIPE || error == -ESTRPIPE) {
        const int result = snd_pcm_recover(pcm, static_cast<int>(error), 1);
        if (result < 0) {
            cerr << "ALSA recovery: " << snd_strerror(result) << '\n';
            return false;
        }
        return true;
    }
    cerr << "ALSA PCM: " << snd_strerror(static_cast<int>(error)) << '\n';
    return false;
}

void AudioEngine::set_audio_thread_policy() {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(1, &cpus);
    const int affinity_result =
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (affinity_result != 0) {
        cerr << "Audio thread CPU affinity not set: "
                  << strerror(affinity_result) << '\n';
    }
    sched_param priority{};
    priority.sched_priority = 80;
    const int scheduling_result =
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
    if (scheduling_result != 0) {
        cerr << "Audio thread SCHED_FIFO unavailable: "
                  << strerror(scheduling_result) << '\n';
    }
}

void AudioEngine::post_result(WorkerResultCode code, uint32_t operation_id,
                              uint8_t error) {
    MainMessage message;
    message.kind = error == 0 ? MessageKind::WorkerResult
                              : MessageKind::WorkerError;
    message.operation_id = operation_id;
    message.result_code = code;
    message.error_code = error;
    messages_.enqueue(move(message));
}

// Only AudioDSP executes these ALSA transitions, at an audio-block boundary.
bool AudioEngine::apply_request(Request request,
                               bool& playback_active) {
    using phono::protocol::Error;
    switch (request.kind) {
        case RequestKind::SuspendCapture: {
            const int result = suspendCapture() ? 0 : -EIO;
            if (result < 0) {
                error_code_.store(static_cast<unsigned>(Error::MicLost));
                post_result(WorkerResultCode::CaptureSuspended,
                            request.operation_id,
                            static_cast<uint8_t>(Error::MicLost));
                return false;
            }
            dsp_.reset();
            post_result(WorkerResultCode::CaptureSuspended,
                        request.operation_id);
            return true;
        }
        case RequestKind::ResumeCapture: {
            const int result = resumeCapture() ? 0 : -EIO;
            if (result < 0) {
                cerr << "Cannot reopen capture PCM '"
                     << options_.capture_pcm << "': " << snd_strerror(result)
                     << '\n';
                error_code_.store(static_cast<unsigned>(Error::MicLost));
                post_result(WorkerResultCode::CaptureResumed,
                            request.operation_id,
                            static_cast<uint8_t>(Error::MicLost));
                return false;
            }
            dsp_.reset();
            post_result(WorkerResultCode::CaptureResumed,
                        request.operation_id);
            return true;
        }
        case RequestKind::StartPlayback:
            playback_active = true;
            dsp_.reset();
            post_result(WorkerResultCode::PlaybackStarted,
                        request.operation_id);
            return true;
        case RequestKind::StopPlayback:
            playback_active = false;
            dsp_.reset();
            post_result(WorkerResultCode::PlaybackStopped,
                        request.operation_id);
            return true;
    }
    return false;
}

// Open/configure capture and headphones inside the audio worker.
bool AudioEngine::initAlsa() {
    using phono::protocol::Error;
    auto*& capture = capture_handle_;
    auto*& playback = playback_handle_;
    int err = snd_pcm_open(&capture, options_.capture_pcm.c_str(),
                           SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        cerr << "Cannot open capture PCM '" << options_.capture_pcm
                  << "': " << snd_strerror(err) << '\n';
        error_code_.store(static_cast<unsigned>(Error::MicLost));
        post_result(WorkerResultCode::AudioReady, 0,
                    static_cast<uint8_t>(Error::MicLost));
        return false;
    }
    if ((err = configure_pcm(capture)) < 0) {
        cerr << "Cannot configure capture PCM: " << snd_strerror(err) << '\n';
        snd_pcm_close(capture);
        capture = nullptr;
        error_code_.store(static_cast<unsigned>(Error::MicLost));
        post_result(WorkerResultCode::AudioReady, 0,
                    static_cast<uint8_t>(Error::MicLost));
        return false;
    }
    capture_pcm_.store(capture);

    err = snd_pcm_open(&playback, options_.playback_pcm.c_str(),
                       SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0 || (err = configure_pcm(playback)) < 0) {
        cerr << "Cannot open/configure playback PCM '"
                  << options_.playback_pcm << "': " << snd_strerror(err)
                  << '\n';
        if (playback != nullptr) snd_pcm_close(playback);
        playback = nullptr;
        capture_pcm_.store(nullptr);
        snd_pcm_close(capture);
        capture = nullptr;
        error_code_.store(static_cast<unsigned>(Error::OutputError));
        post_result(WorkerResultCode::AudioReady, 0,
                    static_cast<uint8_t>(Error::OutputError));
        return false;
    }
    return true;
}

int AudioEngine::readFrames(int16_t* buffer, int frames) {
    return static_cast<int>(snd_pcm_readi(capture_handle_, buffer, frames));
}

int AudioEngine::writeFrames(const int16_t* buffer, int frames) {
    return static_cast<int>(snd_pcm_writei(playback_handle_, buffer, frames));
}

bool AudioEngine::suspendCapture() {
    return capture_handle_ != nullptr && snd_pcm_drop(capture_handle_) >= 0;
}

// Reopening capture also recovers a device that was suspended or reconnected.
bool AudioEngine::resumeCapture() {
    PthreadLock lock(capture_mutex_);
    capture_pcm_.store(nullptr);
    if (capture_handle_ != nullptr) {
        const int closed = snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
        if (closed < 0) return false;
    }
    int result = snd_pcm_open(&capture_handle_, options_.capture_pcm.c_str(),
                              SND_PCM_STREAM_CAPTURE, 0);
    if (result >= 0) result = configure_pcm(capture_handle_);
    if (result < 0 && capture_handle_ != nullptr) {
        snd_pcm_close(capture_handle_);
        capture_handle_ = nullptr;
    }
    capture_pcm_.store(capture_handle_);
    return result >= 0;
}

// Live path: capture -> filter -> gain/clamp -> headphones -> recording -> FFT/telemetry.
void AudioEngine::run() {
    using phono::protocol::Error;
    set_audio_thread_policy();
    auto*& capture = capture_handle_;
    auto*& playback = playback_handle_;
    if (!initAlsa()) return;
    ready_.store(true);
    error_code_.store(0);
    post_result(WorkerResultCode::AudioReady, 0);

    array<int16_t, kPeriodFrames> input{};
    array<int16_t, kPeriodFrames> output{};
    array<float, kPeriodFrames> floating_input{};
    array<float, kPeriodFrames> filtered{};
    array<float, kFftBins> magnitudes{};
    bool capture_suspended = false;
    bool playback_active = false;
    bool playback_completion_sent = false;
    uint32_t playback_operation = 0;
    while (!stop_requested_.load() && running.load()) {
        optional<Request> request;
        {
            PthreadLock lock(request_mutex_);
            request = move(request_);
            request_.reset();
        }
        if (request.has_value()) {
            if (request->kind == RequestKind::SuspendCapture) {
                capture_suspended = true;
            } else if (request->kind == RequestKind::ResumeCapture) {
                capture_suspended = false;
            } else if (request->kind == RequestKind::StartPlayback) {
                playback_operation = request->operation_id;
                playback_completion_sent = false;
            }
            apply_request(*request, playback_active);
        }

        snd_pcm_sframes_t count = 0;
        if (playback_active) {
            const auto samples = playback_buffer_.pop(
                output.data(), output.size(), chrono::milliseconds(10));
            count = static_cast<snd_pcm_sframes_t>(samples);
            if (samples == 0) {
                if (playback_buffer_.stopped() && playback_buffer_.isEmpty()) {
                    if (!playback_completion_sent) {
                        const auto drain_result = snd_pcm_drain(playback);
                        if (drain_result < 0) {
                            error_code_.store(
                                static_cast<unsigned>(Error::OutputError));
                            post_result(WorkerResultCode::PlaybackFinished,
                                        playback_operation,
                                        static_cast<uint8_t>(Error::OutputError));
                        } else {
                            post_result(WorkerResultCode::PlaybackFinished,
                                        playback_operation);
                        }
                        playback_completion_sent = true;
                        playback_active = false;
                    }
                    continue;
                }
                output.fill(0);
                count = static_cast<snd_pcm_sframes_t>(output.size());
                if (!playback_completion_sent) {
                    post_result(WorkerResultCode::PlaybackFinished,
                                playback_operation,
                                static_cast<uint8_t>(Error::ReadError));
                    playback_completion_sent = true;
                    playback_active = false;
                }
            }
        } else if (!capture_suspended) {
            while (count < static_cast<snd_pcm_sframes_t>(input.size()) &&
                   !stop_requested_.load() && !capture_suspended) {
                const auto received = readFrames(input.data() + count,
                    static_cast<int>(input.size() - count));
                if (received < 0) {
                    if (!recover_pcm(capture, received)) {
                        error_code_.store(static_cast<unsigned>(Error::MicLost));
                        post_result(WorkerResultCode::AudioReady, 0,
                                    static_cast<uint8_t>(Error::MicLost));
                        stop_requested_.store(true);
                        break;
                    }
                } else {
                    count += received;
                }
            }
            if (stop_requested_.load()) break;
            if (capture_suspended || count == 0) continue;

            unsigned mode;
            unsigned gain;
            {
                PthreadLock lock(configuration_mutex_);
                mode = mode_.load();
                gain = gain_.load();
            }
            for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
                floating_input[i] = static_cast<float>(input[i]);
            }
            dsp_.applyBandpass(floating_input.data(), filtered.data(),
                              static_cast<int>(count), static_cast<FilterMode>(mode));
            dsp_.applyGainWithLimiter(filtered.data(), output.data(),
                                     static_cast<int>(count), static_cast<int>(gain));
        } else {
            const timespec pause{0, 1000000L};
            nanosleep(&pause, nullptr);
            continue;
        }

        snd_pcm_sframes_t written = 0;
        while (written < count && !stop_requested_.load()) {
            const auto result = writeFrames(output.data() + written,
                static_cast<int>(count - written));
            if (result < 0) {
                if (!recover_pcm(playback, result)) {
                    error_code_.store(static_cast<unsigned>(Error::OutputError));
                    post_result(WorkerResultCode::AudioReady, 0,
                                static_cast<uint8_t>(Error::OutputError));
                    break;
                }
            } else {
                written += result;
            }
        }

        if (!playback_active && !capture_suspended && recording_.load() &&
            !recording_buffer_.push(output.data(), static_cast<size_t>(count)) &&
            !recording_buffer_.stopped()) {
            recording_.store(false);
            recording_buffer_.stop_producing();
            recording_overruns_.fetch_add(1);
            error_code_.store(static_cast<unsigned>(Error::RecordOverrun));
            post_result(WorkerResultCode::RecordingClosed, 0,
                        static_cast<uint8_t>(Error::RecordOverrun));
        }
        // Headphone output and recording come first. FFT is visualization work.
        dsp_.computeFFT(output.data(), magnitudes.data(), static_cast<int>(count));
        if (dsp_.fftReady()) {
            Telemetry telemetry;
            telemetry.waveform = dsp_.waveform();
            telemetry.spectrum = magnitudes;
            if (!telemetry_.try_enqueue(telemetry)) dropped_telemetry_.fetch_add(1);
        }
    }

    ready_.store(false);
    recording_.store(false);
    playback_buffer_.stop_producing();
    {
        PthreadLock lock(capture_mutex_);
        capture_pcm_.store(nullptr);
        if (capture != nullptr) snd_pcm_close(capture);
    }
    if (playback != nullptr) {
        (void)snd_pcm_drain(playback);
        snd_pcm_close(playback);
        playback = nullptr;
    }
    capture = nullptr;
}
}  // namespace phono_app
