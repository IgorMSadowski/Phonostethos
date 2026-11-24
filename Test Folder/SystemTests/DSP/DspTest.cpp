/**
 * DspTest.cpp - checks Dsp Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - measure(): measure.
 * - main(): run this standalone test and return success only if its checks pass.
 */



// Test the actual project's DSP class using known signals instead of a microphone.
#include "phonostethos/DspProcessor.h" // Include the DspProcessor class/types used here; performs live band-pass filtering, gain/limiting and rolling FFT analysis.
#include "phonostethos/protocol.h" // Include the protocol class/types used here; defines and validates the client/server binary protocol.
#include <cmath> // Include trigonometry, square roots, logarithms and finite-value checks.
#include <cstdio> // Include C-style file/console formatting used by this file.

using namespace phono_app; // Allow the names in this namespace without repeating its prefix.

// Measure output/input RMS after skipping the first half-second of settling.
static double measure(DspProcessor& dsp, FilterMode mode, double frequency) { // Finish the parameters/condition and enter the block below.
    float input[8000]; // Declare input for this operation.
    float output[8000]; // Declare output for this operation.
    for (int i = 0; i < 8000; i++) { // Visit the specified items/indexes in order.
        input[i] = std::sin(2.0 * kPi * frequency * i / kSampleRate); // Set input[i] to std::sin(2.0 * kPi * frequency * i / kSampleRate).
    } // End this block, declaration or argument list.
    dsp.reset(); // Call dsp.reset with the arguments shown here.
    dsp.applyBandpass(input, output, 8000, mode); // Call dsp.applyBandpass with the arguments shown here.
    double input_energy = 0; // Initialize input energy to 0.
    double output_energy = 0; // Initialize output energy to 0.
    for (int i = 4000; i < 8000; i++) { // Visit the specified items/indexes in order.
        input_energy += input[i] * input[i]; // Update input energy using += input[i] * input[i].
        output_energy += output[i] * output[i]; // Update output energy using += output[i] * output[i].
    } // End this block, declaration or argument list.
    return std::sqrt(output_energy / input_energy); // Return std::sqrt(output_energy / input_energy) to the caller.
} // End this block, declaration or argument list.

int main() { // Run this standalone test and return success only if its checks pass.
    DspProcessor dsp; // Declare dsp for this operation.
    if (!dsp.initialize()) { // Test the condition !dsp.initialize().
        std::fprintf(stderr, "FAIL: could not initialize the project's FFTW plan\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    const char* names[3] = {"GENERAL", "HEART", "LUNG"}; // Initialize names to {"GENERAL", "HEART", "LUNG"}.
    double frequencies[6] = {5, 50, 200, 500, 1500, 3000}; // Initialize frequencies to {5, 50, 200, 500, 1500, 3000}.
    std::puts("Measured filter response (dB relative to input):"); // Complete the call above with these remaining arguments.
    for (int mode = 0; mode < 3; mode++) { // Visit the specified items/indexes in order.
        std::printf("%s:", names[mode]); // Print the diagnostic or test result for the operator.
        for (double frequency : frequencies) { // Visit the specified items/indexes in order.
            double ratio = measure(dsp, static_cast<FilterMode>(mode), frequency); // Initialize ratio to measure(dsp, static_cast<FilterMode>(mode), frequency).
            std::printf("  %.0fHz=%+.1fdB", frequency, 20.0 * std::log10(ratio)); // Print the diagnostic or test result for the operator.
        } // End this block, declaration or argument list.
        std::puts(""); // Complete the call above with these remaining arguments.
        if (measure(dsp, static_cast<FilterMode>(mode), 200) < 0.85) { // Test the condition measure(dsp, static_cast<FilterMode>(mode), 200) < 0.85.
            std::fprintf(stderr, "FAIL: %s excessively reduced its 200 Hz passband\n", names[mode]); // Print the diagnostic or test result for the operator.
            return 1; // Return 1 to the caller.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    if (measure(dsp, FilterMode::GENERAL, 5) > 0.1 || // Test the condition measure(dsp, FilterMode::GENERAL, 5) > 0.1 ||.
        measure(dsp, FilterMode::GENERAL, 3000) > 0.1 || // Call measure with the arguments shown here.
        measure(dsp, FilterMode::HEART, 1500) > 0.1 || // Call measure with the arguments shown here.
        measure(dsp, FilterMode::LUNG, 50) > 0.1) { // Call measure with the arguments shown here.
        std::fprintf(stderr, "FAIL: an out-of-band tone was not reduced by at least 20 dB\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    std::puts("PASS: all three actual filter profiles pass/reject the expected tones"); // Complete the call above with these remaining arguments.

    // Test the same gain codes sent by the GUI, including all fractional choices.
    unsigned codes[6] = {phono::protocol::GAIN_HALF, phono::protocol::GAIN_THREE_QUARTERS, // Initialize codes to {phono::protocol::GAIN_HALF, phono::protocol::GAIN_THREE_QUARTERS.
                        phono::protocol::GAIN_FOUR_FIFTHS, 1, 2, 20}; // Complete this expression or declaration.
    int expected[6] = {500, 750, 800, 1000, 2000, 20000}; // Initialize expected to {500, 750, 800, 1000, 2000, 20000}.
    float input[1] = {1000}; // Initialize input to {1000}.
    int16_t output[1]; // Declare output for this operation.
    for (int i = 0; i < 6; i++) { // Visit the specified items/indexes in order.
        float gain = phono::protocol::gain_multiplier(codes[i]); // Initialize gain to phono::protocol::gain_multiplier(codes[i]).
        dsp.applyGainWithLimiter(input, output, 1, gain); // Call dsp.applyGainWithLimiter with the arguments shown here.
        std::printf("Gain code %u -> %.2fx: 1000 -> %d\n", codes[i], gain, output[0]); // Print the diagnostic or test result for the operator.
        if (output[0] != expected[i]) return 1; // Test the condition output[0] != expected[i]. If true, return 1 to the caller.
    } // End this block, declaration or argument list.
    float loud[2] = {30000, -30000}; // Initialize loud to {30000, -30000}.
    int16_t limited[2]; // Declare limited for this operation.
    dsp.applyGainWithLimiter(loud, limited, 2, 20); // Call dsp.applyGainWithLimiter with the arguments shown here.
    if (limited[0] != 32767 || limited[1] != -32768) { // Test the condition limited[0] != 32767 || limited[1] != -32768.
        std::fprintf(stderr, "FAIL: limiter did not clamp to PCM16 limits\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    std::puts("PASS: gain codes, attenuation, amplification and limiter"); // Complete the call above with these remaining arguments.

    // The project uses a Hann window and a rolling 1024-point FFT with 512-sample hops.
    int16_t sine[1024]; // Declare sine for this operation.
    float magnitudes[512]; // Declare magnitudes for this operation.
    for (int i = 0; i < 1024; i++) { // Visit the specified items/indexes in order.
        sine[i] = static_cast<int16_t>(8192.0 * std::sin(2.0 * kPi * 500 * i / kSampleRate)); // Set sine[i] to static_cast<int16_t>(8192.0 * std::sin(2.0 * kPi * 500 * i / kSampleRate)).
    } // End this block, declaration or argument list.
    dsp.reset(); // Call dsp.reset with the arguments shown here.
    dsp.computeFFT(sine, magnitudes, 512); // Call dsp.computeFFT with the arguments shown here.
    if (dsp.fftReady()) return 1; // Only half a window is available.
    dsp.computeFFT(sine + 512, magnitudes, 512); // Call dsp.computeFFT with the arguments shown here.
    if (!dsp.fftReady() || std::fabs(magnitudes[64] - 0.25F) > 0.003F) { // Test the condition !dsp.fftReady() || std::fabs(magnitudes[64] - 0.25F) > 0.003F.
        std::fprintf(stderr, "FAIL: expected project FFT bin 64 at amplitude 0.25\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    std::printf("PASS: project's windowed FFT, 500 Hz bin 64 = %.4f\n", magnitudes[64]); // Print the diagnostic or test result for the operator.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
