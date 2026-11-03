#include "phonostethos/AudioEngine.h"
#include "phonostethos/protocol.h"

#include <fftw3.h>
#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <system_error>
#include <unistd.h>

namespace phono_app {
AudioEngine::AudioEngine(const Options& options, std::atomic<unsigned>& mode,
            std::atomic<unsigned>& gain, std::atomic<bool>& recording,
            std::atomic<unsigned>& error_code, Telemetry& telemetry,
            std::mutex& configuration_mutex,
            std::mutex& telemetry_mutex,
            std::atomic<std::uint32_t>& dropped_telemetry,
            FileIOManager& writer)
    : options_(options),
      mode_(mode),
      gain_(gain),
      recording_(recording),
      error_code_(error_code),
      telemetry_(telemetry),
      configuration_mutex_(configuration_mutex),
      telemetry_mutex_(telemetry_mutex),
      dropped_telemetry_(dropped_telemetry),
      writer_(writer),
      worker_(&AudioEngine::run, this) {}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::ready() const { return ready_.load(); }

void AudioEngine::stop() {
    stop_playback();
    stop_requested_.store(true);
    {
        std::lock_guard<std::mutex> capture_lock(capture_mutex_);
        if (auto* capture = capture_pcm_.load(); capture != nullptr) {
            (void)snd_pcm_drop(capture);
        }
    }
    if (worker_.joinable()) worker_.join();
}

bool AudioEngine::start_playback(const std::filesystem::path& path,
                                 int descriptor) {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (playback_thread_.joinable()) {
        playback_cancelled_.store(true);
        playback_requested_.store(false);
        playback_data_.notify_all();
        playback_thread_.join();
    }
    playback_data_.clear();
    playback_eof_.store(false);
    playback_cancelled_.store(false);
    auto ready = std::make_shared<std::promise<bool>>();
    auto result = ready->get_future();
    playback_requested_.store(false);
    try {
        playback_thread_ =
            std::thread(&AudioEngine::play_file, this, path, descriptor,
                        std::move(ready));
    } catch (const std::system_error& error) {
        ::close(descriptor);
        std::cerr << "Cannot start playback file worker: " << error.what()
                  << '\n';
        return false;
    }
    if (result.get()) return true;
    playback_cancelled_.store(true);
    playback_data_.notify_all();
    if (playback_thread_.joinable()) playback_thread_.join();
    return false;
}

void AudioEngine::stop_playback() {
    playback_cancelled_.store(true);
    playback_requested_.store(false);
    playback_data_.notify_all();
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (playback_thread_.joinable()) playback_thread_.join();
    playback_data_.clear();
    playback_eof_.store(false);
}

bool AudioEngine::playing() const { return playback_requested_.load(); }

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
            std::cerr << "ALSA recovery: " << snd_strerror(result) << '\n';
            return false;
        }
        return true;
    }
    std::cerr << "ALSA PCM: " << snd_strerror(static_cast<int>(error)) << '\n';
    return false;
}

void AudioEngine::set_audio_thread_policy() {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(1, &cpus);
    const int affinity_result =
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (affinity_result != 0) {
        std::cerr << "Audio thread CPU affinity not set: "
                  << std::strerror(affinity_result) << '\n';
    }
    sched_param priority{};
    priority.sched_priority = 80;
    const int scheduling_result =
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
    if (scheduling_result != 0) {
        std::cerr << "Audio thread SCHED_FIFO unavailable: "
                  << std::strerror(scheduling_result) << '\n';
    }
}

void AudioEngine::run() {
    snd_pcm_t* capture = nullptr;
    snd_pcm_t* playback = nullptr;
    int err = snd_pcm_open(&capture, options_.capture_pcm.c_str(),
                           SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        std::cerr << "Cannot open capture PCM '" << options_.capture_pcm
                  << "': " << snd_strerror(err) << '\n';
        error_code_.store(3);
        return;
    }
    if ((err = configure_pcm(capture)) < 0) {
        std::cerr << "Cannot configure capture PCM: " << snd_strerror(err)
                  << '\n';
        snd_pcm_close(capture);
        error_code_.store(3);
        return;
    }
    capture_pcm_.store(capture);

    err = snd_pcm_open(&playback, options_.playback_pcm.c_str(),
                       SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        std::cerr << "Cannot open playback PCM '" << options_.playback_pcm
                  << "': " << snd_strerror(err) << '\n';
        snd_pcm_close(capture);
        capture_pcm_.store(nullptr);
        error_code_.store(3);
        return;
    }
    if ((err = configure_pcm(playback)) < 0) {
        std::cerr << "Cannot configure playback PCM: " << snd_strerror(err)
                  << '\n';
        snd_pcm_close(playback);
        snd_pcm_close(capture);
        capture_pcm_.store(nullptr);
        error_code_.store(3);
        return;
    }

    std::array<std::int16_t, kPeriodFrames> input{};
    std::array<std::int16_t, kPeriodFrames> output{};
    if (!dsp_.initialize()) {
        std::cerr << "Could not create FFTW plan\n";
        ready_.store(false);
        capture_pcm_.store(nullptr);
        snd_pcm_close(playback);
        snd_pcm_close(capture);
        error_code_.store(3);
        return;
    }
    set_audio_thread_policy();
    error_code_.store(0);
    ready_.store(true);

    bool previous_playback = false;
    while (!stop_requested_.load() && running.load()) {
        snd_pcm_sframes_t count = 0;
        const bool playback_block = playback_requested_.load();
        if (playback_block != previous_playback) {
            dsp_.reset();
            if (playback_block) {
                const int capture_result = snd_pcm_drop(capture);
                if (capture_result < 0) {
                    std::cerr << "Could not pause capture for playback: "
                              << snd_strerror(capture_result) << '\n';
                }
            } else {
                std::lock_guard<std::mutex> capture_lock(capture_mutex_);
                capture_pcm_.store(nullptr);
                snd_pcm_close(capture);
                capture = nullptr;
                int capture_result = snd_pcm_open(
                    &capture, options_.capture_pcm.c_str(),
                    SND_PCM_STREAM_CAPTURE, 0);
                if (capture_result >= 0) {
                    capture_result = configure_pcm(capture);
                }
                if (capture_result < 0) {
                    std::cerr << "Cannot reopen capture PCM after playback: "
                              << snd_strerror(capture_result) << '\n';
                    if (capture != nullptr) {
                        snd_pcm_close(capture);
                        capture = nullptr;
                    }
                    error_code_.store(static_cast<unsigned>(
                        phono::protocol::Error::MicLost));
                    stop_requested_.store(true);
                    break;
                }
                capture_pcm_.store(capture);
            }
            if (playback_block || playback_cancelled_.load()) {
                const int output_result = snd_pcm_drop(playback);
                if (output_result < 0) {
                    std::cerr << "Cannot discard queued playback audio: "
                              << snd_strerror(output_result) << '\n';
                    error_code_.store(static_cast<unsigned>(
                        phono::protocol::Error::OutputError));
                    stop_requested_.store(true);
                    break;
                }
            }
            const int prepare_result = snd_pcm_prepare(playback);
            if (prepare_result < 0) {
                std::cerr << "Cannot prepare playback after mode change: "
                          << snd_strerror(prepare_result) << '\n';
                error_code_.store(static_cast<unsigned>(
                    phono::protocol::Error::OutputError));
                stop_requested_.store(true);
                break;
            }
            previous_playback = playback_block;
        }
        if (playback_block) {
            PlaybackBlock block;
            const bool has_block = playback_data_.wait_pop_for(
                block, std::chrono::milliseconds(10), &playback_cancelled_);
            if (playback_cancelled_.load()) {
                playback_requested_.store(false);
                continue;
            }
            if (!has_block && playback_eof_.load()) {
                const int drain_result = snd_pcm_drain(playback);
                if (drain_result < 0) {
                    std::cerr << "Cannot drain end of playback: "
                              << snd_strerror(drain_result) << '\n';
                    error_code_.store(static_cast<unsigned>(
                        phono::protocol::Error::OutputError));
                }
                playback_requested_.store(false);
                continue;
            }
            const bool underflow = !has_block;
            if (underflow) {
                error_code_.store(static_cast<unsigned>(
                    phono::protocol::Error::ReadError));
                output.fill(0);
                count = static_cast<snd_pcm_sframes_t>(output.size());
            } else {
                input = block.samples;
                count = static_cast<snd_pcm_sframes_t>(block.count);
                std::copy_n(input.begin(), block.count, output.begin());
            }
        } else {
            while (count < static_cast<snd_pcm_sframes_t>(input.size()) &&
                   !stop_requested_.load() &&
                   !playback_requested_.load()) {
                const auto result = snd_pcm_readi(
                    capture, input.data() + count, input.size() - count);
                if (result < 0) {
                    if (!recover_pcm(capture, result)) {
                        error_code_.store(static_cast<unsigned>(
                            phono::protocol::Error::MicLost));
                        stop_requested_.store(true);
                        break;
                    }
                } else {
                    count += result;
                }
            }
            if (stop_requested_.load()) break;
            if (playback_requested_.load() || count == 0) continue;

            unsigned mode;
            unsigned gain;
            {
                std::lock_guard<std::mutex> lock(configuration_mutex_);
                mode = mode_.load();
                gain = gain_.load();
            }
            dsp_.process_live(input.data(), output.data(),
                             static_cast<std::size_t>(count), mode, gain);
        }

        const auto telemetry_frame =
            dsp_.analyze(output.data(), static_cast<std::size_t>(count));

        snd_pcm_sframes_t written = 0;
        while (written < count && !stop_requested_.load()) {
            const auto result = snd_pcm_writei(
                playback, output.data() + written, count - written);
            if (result < 0) {
                if (!recover_pcm(playback, result)) {
                    error_code_.store(static_cast<unsigned>(
                        phono::protocol::Error::OutputError));
                    break;
                }
            } else {
                written += result;
            }
        }

        if (!playback_block && recording_.load() && !writer_.push(output)) {
            recording_.store(false);
        }

        if (telemetry_frame.has_value()) {
            std::unique_lock<std::mutex> lock(telemetry_mutex_,
                                              std::try_to_lock);
            if (lock.owns_lock()) {
                telemetry_ = *telemetry_frame;
            } else {
                dropped_telemetry_.fetch_add(1);
            }
        }
    }

    ready_.store(false);
    playback_requested_.store(false);
    {
        std::lock_guard<std::mutex> capture_lock(capture_mutex_);
        capture_pcm_.store(nullptr);
        if (capture != nullptr) {
            snd_pcm_close(capture);
            capture = nullptr;
        }
    }
    (void)snd_pcm_drain(playback);
    snd_pcm_close(playback);
}

void AudioEngine::play_file(const std::filesystem::path& path, int descriptor,
                            std::shared_ptr<std::promise<bool>> ready) {
    SF_INFO info{};
    SNDFILE* file = sf_open_fd(descriptor, SFM_READ, &info, SF_FALSE);
    if (file == nullptr) {
        std::cerr << "Cannot open playback file " << path << ": "
                  << sf_strerror(nullptr) << '\n';
        ::close(descriptor);
        error_code_.store(
            static_cast<unsigned>(phono::protocol::Error::ReadError));
        playback_requested_.store(false);
        ready->set_value(false);
        return;
    }
    if (info.channels != 1 || info.samplerate != kSampleRate ||
        (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV ||
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) {
        std::cerr << "Unsupported playback format in " << path << '\n';
        (void)sf_close(file);
        ::close(descriptor);
        error_code_.store(static_cast<unsigned>(
            phono::protocol::Error::UnsupportedWav));
        playback_requested_.store(false);
        ready->set_value(false);
        return;
    }
    bool readiness_sent = false;
    std::array<std::int16_t, kPeriodFrames> samples{};
    while (!playback_cancelled_.load() && running.load()) {
        const auto count = sf_read_short(file, samples.data(), samples.size());
        if (count < 0) {
            error_code_.store(
                static_cast<unsigned>(phono::protocol::Error::ReadError));
            break;
        }
        if (count == 0) {
            if (sf_error(file) != SF_ERR_NO_ERROR) {
                error_code_.store(
                    static_cast<unsigned>(phono::protocol::Error::ReadError));
            }
            playback_eof_.store(true);
            if (!readiness_sent) {
                playback_requested_.store(true);
                ready->set_value(true);
                readiness_sent = true;
            }
            playback_data_.notify_all();
            break;
        }

        PlaybackBlock block;
        block.count = static_cast<std::size_t>(count);
        std::copy_n(samples.begin(), block.count, block.samples.begin());
        if (!playback_data_.push(std::move(block), &playback_cancelled_)) {
            break;
        }
        if (!readiness_sent) {
            playback_requested_.store(true);
            ready->set_value(true);
            readiness_sent = true;
        }
        playback_data_.notify_all();
    }
    if (sf_close(file) != 0) {
        error_code_.store(
            static_cast<unsigned>(phono::protocol::Error::FinalizeError));
    }
    if (::close(descriptor) != 0) {
        error_code_.store(
            static_cast<unsigned>(phono::protocol::Error::FinalizeError));
    }
    playback_eof_.store(true);
    playback_data_.notify_all();
    if (!readiness_sent) {
        playback_requested_.store(false);
        ready->set_value(false);
    }
}
}  // namespace phono_app
