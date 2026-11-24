/**
 * DspProcessor.cpp - performs live band-pass filtering, gain/limiting and rolling FFT analysis.
 *
 * CONNECTIONS: DspProcessor.h declares state and operations; DspProcessor.cpp implements them. AudioEngine calls them from th_AudioDSP. BandPassFilter.h implements the biquad cascade; RuntimeTypes.h supplies 8 kHz/512/1024 constants. FFTW planning occurs before workers start.
 *
 * FUNCTIONS:
 * Main methods are listed first; private helpers are implemented at the end.
 * - ~DspProcessor(): release this object's resources after its users have stopped.
 * - applyBandpass(): select the report's mode profile and run the persistent biquad filter cascade.
 * - applyGainWithLimiter(): multiply floating-point samples and clamp them before conversion to PCM16.
 * - computeFFT(): update the rolling 1024-sample history and compute Hann-normalized spectrum magnitudes.
 * - initialize(): prepare resources before the audio worker starts using them.
 * - reset(): clear processing history so a new stream does not inherit old samples.
 * - filter_limits(): select GENERAL, HEART or LUNG high-pass and low-pass cutoff frequencies.
 */

// API DOCUMENTATION FOR THIS FILE:
// FFTW: real-to-complex planning/execution and single-precision fftwf APIs:
// https://www.fftw.org/fftw3_doc/
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include "phonostethos/DspProcessor.h" // Include the DspProcessor class/types used here; performs live band-pass filtering, gain/limiting and rolling FFT analysis.

#include <algorithm> // Include min/max, copying, sorting and other standard algorithms used in this file.
#include <cmath> // Include trigonometry, square roots, logarithms and finite-value checks.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.

// ==================== OBJECT SETUP ====================

DspProcessor::~DspProcessor() { // Finish the parameters/condition and enter the block below.
    if (fft_plan_ != nullptr) { // Test the condition fft_plan_ != nullptr.
        /* API: fftwf_destroy_plan
         * Documentation: https://www.fftw.org/fftw3_doc/
         * Release the FFT plan after the audio worker has stopped using it.
         */
        fftwf_destroy_plan(fft_plan_); // Call fftwf_destroy_plan with the arguments shown here.
    } // End this block, declaration or argument list.
} // End this function or local scope.

// ==================== MAIN METHODS - REPORT ORDER ====================

// The existing tested filter has a high-pass stage followed by a low-pass stage.
void DspProcessor::applyBandpass(const float* input, float* output, // Pass this next argument/value to the call or initializer above.
                                 int count, FilterMode mode) { // Declare count for this operation.
    const auto selected_mode = static_cast<unsigned>(mode); // Initialize selected mode to static_cast<unsigned>(mode).
    if (selected_mode != configured_mode_) { // Test the condition selected_mode != configured_mode_.
        double low; // Declare low for this operation.
        double high; // Declare high for this operation.
        filter_limits(selected_mode, low, high); // Select GENERAL, HEART or LUNG high-pass and low-pass cutoff frequencies.
        filter_.configure(low, high, kSampleRate); // Call filter_.configure with the arguments shown here.
        configured_mode_ = selected_mode; // Set configured mode to selected_mode.
    } // End this block, declaration or argument list.
    for (int i = 0; i < count; ++i) output[i] = static_cast<float>(filter_.process(input[i])); // Visit the specified items/indexes in order.
} // End this function or local scope.

void DspProcessor::applyGainWithLimiter(const float* input, int16_t* output, // Pass this next argument/value to the call or initializer above.
                                       int count, float gain) { // Declare count for this operation.
    for (int i = 0; i < count; ++i) { // Visit the specified items/indexes in order.
        float sample = input[i] * gain; // Initialize sample to input[i] * gain.
        // PCM16 cannot store a value outside this range. Limit before conversion.
        if (sample < -32768.0F) sample = -32768.0F; // Test the condition sample < -32768.0F. If true, set sample to -32768.0f.
        if (sample > 32767.0F) sample = 32767.0F; // Test the condition sample > 32767.0F. If true, set sample to 32767.0f.
        output[i] = static_cast<int16_t>(sample); // Set output[i] to static_cast<int16_t>(sample).
    } // End this block, declaration or argument list.
} // End this function or local scope.

void DspProcessor::computeFFT(const int16_t* samples, float* magnitudes, int count) { // Update the rolling 1024-sample history and compute Hann-normalized spectrum magnitudes.
    const auto frames = static_cast<size_t>(count); // Initialize count to static_cast<size_t>(sample_count).
    fft_ready_ = false; // Set fft ready to false.
    for (size_t i = 0; i < frames; ++i) { // Visit the specified items/indexes in order.
        history_[history_next_] = static_cast<float>(samples[i]) / 32768.0F; // Set history [history next ] to static_cast<float>(samples[i]) / 32768.0F.
        history_next_ = (history_next_ + 1) % kFftFrames; // Set history next to (history_next_ + 1) % kFftFrames.
        history_size_ = min(history_size_ + 1, kFftFrames); // Set history size to min(history_size_ + 1, kFftFrames).
    } // End this block, declaration or argument list.
    frames_since_fft_ += frames; // Update frames since fft using += count.
    if (frames_since_fft_ < kPeriodFrames || history_size_ < kFftFrames) { // Test the condition frames_since_fft_ < kPeriodFrames || history_size_ < kFftFrames.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    frames_since_fft_ %= kPeriodFrames; // Complete this expression or declaration.
    for (size_t i = 0; i < kFftFrames; ++i) { // Visit the specified items/indexes in order.
        waveform_[i] = history_[(history_next_ + i) % kFftFrames]; // Set waveform [i] to history_[(history_next_ + i) % kFftFrames].
    } // End this block, declaration or argument list.

    float window_sum = 0.0F; // Initialize window sum to 0.0F.
    for (size_t i = 0; i < kFftFrames; ++i) { // Visit the specified items/indexes in order.
        const float window = // Start initializing this local value from the expression on the next line.
            0.5F - 0.5F * cos( // Begin this function/widget call; its arguments follow below.
                       static_cast<float>(2.0 * 3.14159265358979323846 * i) / // Continue this calculation/condition with the next line.
                       static_cast<float>(kFftFrames - 1)); // Complete the call above with these remaining arguments.
        fft_input_[i] = waveform_[i] * window; // Set fft input [i] to waveform_[i] * window.
        window_sum += window; // Update window sum using += window.
    } // End this block, declaration or argument list.
    /* API: fftwf_execute
     * Documentation: https://www.fftw.org/fftw3_doc/
     * Run the previously prepared FFT plan using the arrays supplied during planning. Window and fill input before calling it.
     */
    fftwf_execute(fft_plan_); // Call fftwf_execute with the arguments shown here.

    for (size_t bin = 0; bin < kFftBins; ++bin) { // Visit the specified items/indexes in order.
        float real = fft_output_[bin][0]; // Initialize real to fft_output_[bin][0].
        float imaginary = fft_output_[bin][1]; // Initialize imaginary to fft_output_[bin][1].
        float magnitude = hypot(real, imaginary) / window_sum; // Initialize magnitude to hypot(real, imaginary) / window_sum.
        // Positive-frequency bins represent both halves of a real signal's FFT.
        // DC (bin 0) has no negative-frequency partner, so it is not doubled.
        if (bin != 0) magnitude *= 2.0F; // Test the condition bin != 0. If true, update magnitude using *= 2.0f.
        magnitudes[bin] = magnitude; // Set magnitudes[bin] to magnitude.
    } // End this block, declaration or argument list.
    fft_ready_ = true; // Set fft ready to true.
} // End this function or local scope.

bool DspProcessor::initialize() { // Prepare resources before the audio worker starts using them.
    if (fft_plan_ != nullptr) { // Test the condition fft_plan_ != nullptr.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    /* API: fftwf_plan_dft_r2c_1d
     * Documentation: https://www.fftw.org/fftw3_doc/
     * Create a single-precision real-to-complex FFT plan using a length, input array, output array and planning flags. A 1024-point transform needs 513 complex outputs. Plan before starting workers.
     */
    fft_plan_ = fftwf_plan_dft_r2c_1d( // Set fft plan to fftwf_plan_dft_r2c_1d(.
        static_cast<int>(kFftFrames), fft_input_.data(), fft_output_.data(), // Pass this next argument/value to the call or initializer above.
        FFTW_ESTIMATE); // Complete the call above with these remaining arguments.
    return fft_plan_ != nullptr; // Prepared single-precision FFTW plan; nullptr means no plan has been created.
} // End this function or local scope.

void DspProcessor::reset() { // Clear processing history so a new stream does not inherit old samples.
    history_.fill(0.0F); // Call history_.fill with the arguments shown here.
    history_next_ = 0; // Set history next to 0.
    history_size_ = 0; // Set history size to 0.
    filter_.reset(); // Call filter_.reset with the arguments shown here.
    configured_mode_ = 99; // Set configured mode to 99.
    frames_since_fft_ = 0; // Set frames since fft to 0.
    fft_ready_ = false; // Set fft ready to false.
} // End this function or local scope.

// ==================== PRIVATE HELPERS ====================

// These are the three report profiles. Each edge is fourth-order Butterworth.
void DspProcessor::filter_limits(unsigned mode, double& low, double& high) { // Select GENERAL, HEART or LUNG high-pass and low-pass cutoff frequencies.
    low = 20.0; // Set low to 20.0.
    high = 2000.0; // Set high to 2000.0.
    if (mode == static_cast<unsigned>(FilterMode::HEART)) { // Test the condition mode == static_cast<unsigned>(FilterMode::HEART).
        high = 600.0; // Set high to 600.0.
    } else if (mode == static_cast<unsigned>(FilterMode::LUNG)) { // Follow the alternative path when the previous condition is false.
        low = 100.0; // Set low to 100.0.
        high = 1000.0; // Set high to 1000.0.
    } // End this block, declaration or argument list.
} // End this function or local scope.


}  // namespace phono_app
