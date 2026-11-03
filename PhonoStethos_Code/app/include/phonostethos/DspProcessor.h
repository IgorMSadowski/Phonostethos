#pragma once

#include "phonostethos/BandPassFilter.h"
#include "phonostethos/CircularBuffer.h"
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
    void process_live(const int16_t* input, int16_t* output,
                      size_t count, unsigned mode, unsigned gain);
    optional<Telemetry> analyze(const int16_t* samples,
                                     size_t count);

private:
    static pair<double, double> filter_limits(unsigned mode);

    CircularBuffer<float, kFftFrames> history_;
    array<float, kFftFrames> fft_input_{};
    array<float, kFftFrames> waveform_{};
    array<fftwf_complex, kFftFrames / 2 + 1> fft_output_{};
    fftwf_plan fft_plan_ = nullptr;
    BandPassFilter filter_;
    unsigned configured_mode_ = 99;
    size_t frames_since_fft_ = 0;
};
}  // namespace phono_app
