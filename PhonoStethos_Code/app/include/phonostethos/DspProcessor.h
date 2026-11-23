/**
 * DspProcessor.h - performs live band-pass filtering, gain/limiting and rolling FFT analysis.
 *
 * CONNECTIONS: DspProcessor.h declares state and operations; DspProcessor.cpp implements them. AudioEngine calls them from th_AudioDSP. BandPassFilter.h implements the biquad cascade; RuntimeTypes.h supplies 8 kHz/512/1024 constants. FFTW planning occurs before workers start.
 *
 * FUNCTIONS:
 * - applyBandpass(): select the report's mode profile and run the persistent biquad filter cascade.
 * - applyGainWithLimiter(): multiply floating-point samples and clamp them before conversion to PCM16.
 * - computeFFT(): update the rolling 1024-sample history and compute Hann-normalized spectrum magnitudes.
 * - initialize(): prepare resources before the audio worker starts using them.
 * - reset(): clear processing history so a new stream does not inherit old samples.
 * - fftReady(): report whether the latest input produced a complete new FFT window.
 * - waveform(): expose the unwindowed normalized history associated with the latest FFT.
 * - filter_limits(): select GENERAL, HEART or LUNG high-pass and low-pass cutoff frequencies.
 */

#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include "phonostethos/BandPassFilter.h" // Include the BandPassFilter class/types used here; implements the floating-point high-pass/low-pass biquad cascade.
#include "phonostethos/RuntimeTypes.h" // Include the RuntimeTypes class/types used here; defines shared audio constants, runtime options, telemetry and archive identities.

#include <fftw3.h> // Include single-precision FFTW plans, complex output arrays and FFT execution APIs.

#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <cstddef> // Include size_t for sizes and array indexes.
#include <cstdint> // Include integer types with exact bit widths used by PCM and the binary protocol.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
class DspProcessor { // Declare DspProcessor and its related operations/state.
public: // The class operations callable by other code.
    DspProcessor() = default; // Use the compiler-provided default operation.
    ~DspProcessor(); // Destroy the FFTW plan after AudioEngine has stopped using it.

    DspProcessor(const DspProcessor&) = delete; // Disallow copying this resource-owning object.
    DspProcessor& operator=(const DspProcessor&) = delete; // Disallow copying this resource-owning object.

    void applyBandpass(const float* input, float* output, int count, FilterMode mode); // Select the report's mode profile and run the persistent biquad filter cascade.
    void applyGainWithLimiter(const float* input, int16_t* output, int count, float gain); // Multiply floating-point samples and clamp them before conversion to PCM16.
    void computeFFT(const int16_t* samples, float* magnitudes, int count); // Update the rolling 1024-sample history and compute Hann-normalized spectrum magnitudes.
    // Startup/reset and visualization accessors support the operations above.
    bool initialize(); // Prepare resources before the audio worker starts using them.
    void reset(); // Clear processing history so a new stream does not inherit old samples.
    bool fftReady() const { return fft_ready_; } // Let AudioEngine publish only after a complete new FFT window is available.
    const array<float, kFftFrames>& waveform() const { return waveform_; } // Return the matching normalized waveform without making another array copy.

private: // Internal helpers and state used by this class.
    static void filter_limits(unsigned mode, double& low, double& high); // Select GENERAL, HEART or LUNG high-pass and low-pass cutoff frequencies.

    array<float, kFftFrames> history_{}; // Normalized processed samples in the rolling FFT history.
    size_t history_next_ = 0; // Next history write position; when full, it is also the oldest retained sample.
    size_t history_size_ = 0; // Valid samples accumulated; FFT output waits until all 1024 are present.
    array<float, kFftFrames> fft_input_{}; // Separate Hann-windowed real input; windowing does not alter the transmitted waveform.
    array<float, kFftFrames> waveform_{}; // Chronological, unwindowed normalized history associated with the latest spectrum.
    array<fftwf_complex, kFftFrames / 2 + 1> fft_output_{}; // The rolling FFT history contains 1024 samples (128 ms at 8 kHz).
    fftwf_plan fft_plan_ = nullptr; // Prepared single-precision FFTW plan; nullptr means no plan has been created.
    BandPassFilter filter_; // Persistent high-pass/low-pass biquad cascade used by applyBandpass.
    unsigned configured_mode_ = 99; // 99 means no profile is configured yet; the first block forces coefficient setup.
    size_t frames_since_fft_ = 0; // Count new samples toward the next 512-sample FFT hop.
    bool fft_ready_ = false; // True only when this computeFFT call produced a complete new spectrum.
}; // End this block, declaration or argument list.
}  // namespace phono_app
