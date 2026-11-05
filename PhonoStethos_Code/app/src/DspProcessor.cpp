#include "phonostethos/DspProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace std;

namespace phono_app {
DspProcessor::~DspProcessor() {
    if (fft_plan_ != nullptr) {
        fftwf_destroy_plan(fft_plan_);
    }
}

bool DspProcessor::initialize() {
    if (fft_plan_ != nullptr) return true;
    fft_plan_ = fftwf_plan_dft_r2c_1d(
        static_cast<int>(kFftFrames), fft_input_.data(), fft_output_.data(),
        FFTW_ESTIMATE);
    return fft_plan_ != nullptr;
}

void DspProcessor::reset() {
    history_.fill(0.0F);
    history_next_ = 0;
    history_size_ = 0;
    filter_.reset();
    configured_mode_ = 99;
    frames_since_fft_ = 0;
    fft_ready_ = false;
}

// The existing tested filter has a high-pass stage followed by a low-pass stage.
void DspProcessor::applyBandpass(const float* input, float* output,
                                 int count, FilterMode mode) {
    const auto selected_mode = static_cast<unsigned>(mode);
    if (selected_mode != configured_mode_) {
        const auto [low, high] = filter_limits(selected_mode);
        filter_.configure(low, high, kSampleRate);
        configured_mode_ = selected_mode;
    }
    for (int i = 0; i < count; ++i) output[i] = static_cast<float>(filter_.process(input[i]));
}

void DspProcessor::applyGainWithLimiter(const float* input, int16_t* output,
                                       int count, int gain) {
    for (int i = 0; i < count; ++i) {
        output[i] = static_cast<int16_t>(clamp(input[i] * gain, -32768.0F, 32767.0F));
    }
}

void DspProcessor::computeFFT(const int16_t* samples, float* magnitudes, int sample_count) {
    const auto count = static_cast<size_t>(sample_count);
    fft_ready_ = false;
    for (size_t i = 0; i < count; ++i) {
        history_[history_next_] = static_cast<float>(samples[i]) / 32768.0F;
        history_next_ = (history_next_ + 1) % kFftFrames;
        history_size_ = min(history_size_ + 1, kFftFrames);
    }
    frames_since_fft_ += count;
    if (frames_since_fft_ < kPeriodFrames || history_size_ < kFftFrames) {
        return;
    }
    frames_since_fft_ %= kPeriodFrames;
    for (size_t i = 0; i < kFftFrames; ++i) {
        waveform_[i] = history_[(history_next_ + i) % kFftFrames];
    }

    float window_sum = 0.0F;
    for (size_t i = 0; i < kFftFrames; ++i) {
        const float window =
            0.5F - 0.5F * cos(
                       static_cast<float>(2.0 * 3.14159265358979323846 * i) /
                       static_cast<float>(kFftFrames - 1));
        fft_input_[i] = waveform_[i] * window;
        window_sum += window;
    }
    fftwf_execute(fft_plan_);

    for (size_t bin = 0; bin < kFftBins; ++bin) {
        magnitudes[bin] =
            hypot(fft_output_[bin][0], fft_output_[bin][1]) / window_sum *
            (bin == 0 ? 1.0F : 2.0F);
    }
    fft_ready_ = true;
}

pair<double, double> DspProcessor::filter_limits(unsigned mode) {
    switch (static_cast<FilterMode>(mode)) {
        case FilterMode::Heart: return {20.0, 600.0};
        case FilterMode::Lung: return {100.0, 1000.0};
        case FilterMode::General:
        default: return {20.0, 2000.0};
    }
}
}  // namespace phono_app
