/**
 * FftwTest.c - checks Fftw Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - main(): run this standalone test and return success only if its checks pass.
 */

#include <stdbool.h> // Include C bool, true and false.
// Test the FFTW3f library directly, independently of the microphone and GUI.
// API reference: https://www.fftw.org/fftw3_doc/One_002dDimensional-DFTs-of-Real-Data.html
#include <fftw3.h> // Include single-precision FFTW plans, complex output arrays and FFT execution APIs.
#include <math.h> // Include trigonometric functions and floating-point magnitude calculations.
#include <stdio.h> // Include printf/fprintf/puts console diagnostics.

int main(void) { // Run this standalone test and return success only if its checks pass.
    enum { count = 1024 }; // Give the following named choices distinct typed values.
    const int rate = 8000; // Initialize rate to 8000.
    const double pi = 3.14159265358979323846; // Initialize pi to 3.14159265358979323846.
    float input[count]; // Declare input for this operation.
    fftwf_complex output[count / 2 + 1]; // Real FFT: 1024 inputs -> 513 complex bins.

    for (int i = 0; i < count; i++) { // Visit the specified items/indexes in order.
        input[i] = sin(2.0 * pi * 1000.0 * i / rate); // Set input[i] to sin(2.0 * pi * 1000.0 * i / rate).
    } // End this block, declaration or argument list.
    /* API: fftwf_plan_dft_r2c_1d
     * Prepare a real-to-complex single-precision transform. FFTW_ESTIMATE avoids
     * expensive planning. Planning is done once, before processing samples.
     */
    fftwf_plan plan = fftwf_plan_dft_r2c_1d(count, input, output, FFTW_ESTIMATE); // Initialize plan to fftwf_plan_dft_r2c_1d(count, input, output, FFTW_ESTIMATE).
    if (plan == NULL) { // Test the condition plan == NULL.
        fprintf(stderr, "FAIL: FFTW could not create a plan\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    // API: fftwf_execute runs the prepared plan on its input/output arrays.
    fftwf_execute(plan); // Call fftwf_execute with the arguments shown here.
    int peak_bin = 1; // Ignore DC when searching for the sine's frequency.
    float peak = 0; // Initialize peak to 0.
    for (int bin = 1; bin < count / 2; bin++) { // Visit the specified items/indexes in order.
        // FFTW's raw transform is unnormalized. A real sine needs 2/N scaling.
        float magnitude = 2.0F * hypot(output[bin][0], output[bin][1]) / count; // Initialize magnitude to 2.0F * hypot(output[bin][0], output[bin][1]) / count.
        if (magnitude > peak) { // Test the condition magnitude > peak.
            peak = magnitude; // Set peak to magnitude.
            peak_bin = bin; // Set peak bin to bin.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    float frequency = (float)(peak_bin * rate) / count; // Initialize frequency to (float)(peak_bin * rate) / count.
    printf("Peak: bin %d, %.2f Hz, amplitude %.4f\n", peak_bin, frequency, peak); // Print the diagnostic or test result for the operator.
    // API: fftwf_destroy_plan releases the plan after its last execution.
    fftwf_destroy_plan(plan); // Call fftwf_destroy_plan with the arguments shown here.
    if (peak_bin != 128 || fabs(peak - 1.0F) > 0.001F) { // Test the condition peak_bin != 128 || fabs(peak - 1.0F) > 0.001F.
        fprintf(stderr, "FAIL: expected bin 128, 1000 Hz, amplitude 1.0\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    puts("PASS: FFTW3f plan, execution, frequency and amplitude scaling"); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
