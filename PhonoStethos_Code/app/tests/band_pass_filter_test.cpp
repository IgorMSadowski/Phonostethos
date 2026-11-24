/**
 * band_pass_filter_test.cpp - checks band pass filter with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - output_rms(): output rms.
 * - main(): run this standalone test and return success only if its checks pass.
 */



#include "phonostethos/BandPassFilter.h" // Include the BandPassFilter class/types used here; implements the floating-point high-pass/low-pass biquad cascade.

#include <cmath> // Include trigonometry, square roots, logarithms and finite-value checks.
#include <cstddef> // Include size_t for sizes and array indexes.
#include <cstdint> // Include integer types with exact bit widths used by PCM and the binary protocol.
#include <iostream> // Include console diagnostics and test results.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace { // Keep these declarations in the project namespace and avoid global name collisions.
double output_rms(phono_app::BandPassFilter& filter, double frequency) { // Finish the parameters/condition and enter the block below.
    constexpr double kPi = 3.14159265358979323846; // Pi used when converting filter frequency to radians per sample.
    constexpr double kRate = 8000.0; // Initialize k Rate to 8000.0.
    constexpr size_t kSamples = 8000; // Initialize k Samples to 8000.
    constexpr size_t kSkip = 1000; // Initialize k Skip to 1000.
    filter.reset(); // Call filter.reset with the arguments shown here.
    double sum_squares = 0.0; // Initialize sum squares to 0.0.
    for (size_t i = 0; i < kSamples; ++i) { // Visit the specified items/indexes in order.
        const auto input = // Start initializing this local value from the expression on the next line.
            sin(2.0 * kPi * frequency * i / kRate); // Call sin with the arguments shown here.
        const auto output = filter.process(input); // Initialize output to filter.process(input).
        if (i >= kSkip) sum_squares += output * output; // Test the condition i >= kSkip. If true, update sum squares using += output * output.
    } // End this block, declaration or argument list.
    return sqrt(sum_squares / (kSamples - kSkip)); // Return sqrt(sum_squares / (kSamples - kSkip)) to the caller.
} // End this block, declaration or argument list.
}  // namespace

int main() { // Run this standalone test and return success only if its checks pass.
    phono_app::BandPassFilter general; // Declare general for this operation.
    phono_app::BandPassFilter heart; // Declare heart for this operation.
    phono_app::BandPassFilter lung; // Declare lung for this operation.
    general.configure(20.0, 2000.0, 8000.0); // Call general.configure with the arguments shown here.
    heart.configure(20.0, 600.0, 8000.0); // Call heart.configure with the arguments shown here.
    lung.configure(100.0, 1000.0, 8000.0); // Call lung.configure with the arguments shown here.

    const auto general_low = output_rms(general, 50.0); // Initialize general low to output_rms(general, 50.0).
    const auto lung_low = output_rms(lung, 50.0); // Initialize lung low to output_rms(lung, 50.0).
    if (lung_low >= general_low * 0.35) { // Test the condition lung_low >= general_low * 0.35.
        cerr << "Lung filter did not attenuate frequencies below 100 Hz\n"; // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.

    const auto general_high = output_rms(general, 1500.0); // Initialize general high to output_rms(general, 1500.0).
    const auto heart_high = output_rms(heart, 1500.0); // Initialize heart high to output_rms(heart, 1500.0).
    if (heart_high >= general_high * 0.35) { // Test the condition heart_high >= general_high * 0.35.
        cerr << "Heart filter did not attenuate frequencies above 600 Hz\n"; // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.

    if (output_rms(general, 500.0) <= 0.5 || // Test the condition output_rms(general, 500.0) <= 0.5 ||.
        output_rms(heart, 500.0) <= 0.5 || // Call output_rms with the arguments shown here.
        output_rms(lung, 500.0) <= 0.5) { // Call output_rms with the arguments shown here.
        cerr << "A filter attenuated a frequency inside its passband\n"; // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
