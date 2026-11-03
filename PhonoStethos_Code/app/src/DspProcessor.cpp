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
    history_.clear();
    filter_.reset();
    configured_mode_ = 99;
    frames_since_fft_ = 0;
}

void DspProcessor::process_live(const int16_t* input,
                                int16_t* output, size_t count,
                                unsigned mode, unsigned gain) {
    if (mode != configured_mode_) {
        const auto [low, high] = filter_limits(mode);
        filter_.configure(low, high, kSampleRate);
        configured_mode_ = mode;
    }

    for (size_t i = 0; i < count; ++i) {
        const double filtered = filter_.process(input[i]);
        output[i] = static_cast<int16_t>(
            clamp(filtered * gain, -32768.0, 32767.0));
    }
}

optional<Telemetry> DspProcessor::analyze(const int16_t* samples,
                                                size_t count) {
    for (size_t i = 0; i < count; ++i) {
        history_.push(static_cast<float>(samples[i]) / 32768.0F);
    }
    frames_since_fft_ += count;
    if (frames_since_fft_ < kPeriodFrames || history_.size() < kFftFrames) {
        return nullopt;
    }
    frames_since_fft_ %= kPeriodFrames;
    history_.copy_ordered(waveform_);

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

    Telemetry telemetry;
    telemetry.waveform = waveform_;
    for (size_t bin = 0; bin < kFftBins; ++bin) {
        telemetry.spectrum[bin] =
            hypot(fft_output_[bin][0], fft_output_[bin][1]) / window_sum *
            (bin == 0 ? 1.0F : 2.0F);
    }
    return telemetry;
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
