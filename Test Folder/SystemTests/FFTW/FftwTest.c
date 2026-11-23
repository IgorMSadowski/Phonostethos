#include <stdbool.h>
// Test the FFTW3f library directly, independently of the microphone and GUI.
// API reference: https://www.fftw.org/fftw3_doc/One_002dDimensional-DFTs-of-Real-Data.html
#include <fftw3.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    enum { count = 1024 };
    const int rate = 8000;
    const double pi = 3.14159265358979323846;
    float input[count];
    fftwf_complex output[count / 2 + 1]; // Real FFT: 1024 inputs -> 513 complex bins.

    for (int i = 0; i < count; i++) {
        input[i] = sin(2.0 * pi * 1000.0 * i / rate);
    }
    /* API: fftwf_plan_dft_r2c_1d
     * Prepare a real-to-complex single-precision transform. FFTW_ESTIMATE avoids
     * expensive planning. Planning is done once, before processing samples.
     */
    fftwf_plan plan = fftwf_plan_dft_r2c_1d(count, input, output, FFTW_ESTIMATE);
    if (plan == NULL) {
        fprintf(stderr, "FAIL: FFTW could not create a plan\n");
        return 1;
    }
    // API: fftwf_execute runs the prepared plan on its input/output arrays.
    fftwf_execute(plan);
    int peak_bin = 1; // Ignore DC when searching for the sine's frequency.
    float peak = 0;
    for (int bin = 1; bin < count / 2; bin++) {
        // FFTW's raw transform is unnormalized. A real sine needs 2/N scaling.
        float magnitude = 2.0F * hypot(output[bin][0], output[bin][1]) / count;
        if (magnitude > peak) {
            peak = magnitude;
            peak_bin = bin;
        }
    }
    float frequency = (float)(peak_bin * rate) / count;
    printf("Peak: bin %d, %.2f Hz, amplitude %.4f\n", peak_bin, frequency, peak);
    // API: fftwf_destroy_plan releases the plan after its last execution.
    fftwf_destroy_plan(plan);
    if (peak_bin != 128 || fabs(peak - 1.0F) > 0.001F) {
        fprintf(stderr, "FAIL: expected bin 128, 1000 Hz, amplitude 1.0\n");
        return 1;
    }
    puts("PASS: FFTW3f plan, execution, frequency and amplitude scaling");
    return 0;
}
