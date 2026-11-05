#pragma once

#include "phonostethos/BandPassFilter.h"
#include "phonostethos/RuntimeTypes.h"

#include <fftw3.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

using namespace std;

namespace phono_app {
class DspProcessor {
public:
    DspProcessor() = default;
    ~DspProcessor();

    DspProcessor(const DspProcessor&) = delete;
    DspProcessor& operator=(const DspProcessor&) = delete;

    bool initialize();
    void reset();
    void applyBandpass(const float* input, float* output, int count, FilterMode mode);
    void applyGainWithLimiter(const float* input, int16_t* output, int count, int gain);
    void computeFFT(const int16_t* samples, float* magnitudes, int count);
    bool fftReady() const { return fft_ready_; }
    const array<float, kFftFrames>& waveform() const { return waveform_; }

private:
    static pair<double, double> filter_limits(unsigned mode);

    array<float, kFftFrames> history_{};
    size_t history_next_ = 0;
    size_t history_size_ = 0;
    array<float, kFftFrames> fft_input_{};
    array<float, kFftFrames> waveform_{};
    array<fftwf_complex, kFftFrames / 2 + 1> fft_output_{};
    fftwf_plan fft_plan_ = nullptr;
    BandPassFilter filter_;
    unsigned configured_mode_ = 99;
    size_t frames_since_fft_ = 0;
    bool fft_ready_ = false;
};
}  // namespace phono_app
