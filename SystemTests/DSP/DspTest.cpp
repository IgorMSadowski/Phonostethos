// Test the actual project's DSP class using known signals instead of a microphone.
#include "phonostethos/DspProcessor.h"
#include "phonostethos/protocol.h"
#include <cmath>
#include <cstdio>

using namespace phono_app;

// Measure output/input RMS after skipping the first half-second of settling.
static double measure(DspProcessor& dsp, FilterMode mode, double frequency) {
    float input[8000];
    float output[8000];
    for (int i = 0; i < 8000; i++) {
        input[i] = std::sin(2.0 * kPi * frequency * i / kSampleRate);
    }
    dsp.reset();
    dsp.applyBandpass(input, output, 8000, mode);
    double input_energy = 0;
    double output_energy = 0;
    for (int i = 4000; i < 8000; i++) {
        input_energy += input[i] * input[i];
        output_energy += output[i] * output[i];
    }
    return std::sqrt(output_energy / input_energy);
}

int main() {
    DspProcessor dsp;
    if (!dsp.initialize()) {
        std::fprintf(stderr, "FAIL: could not initialize the project's FFTW plan\n");
        return 1;
    }
    const char* names[3] = {"GENERAL", "HEART", "LUNG"};
    double frequencies[6] = {5, 50, 200, 500, 1500, 3000};
    std::puts("Measured filter response (dB relative to input):");
    for (int mode = 0; mode < 3; mode++) {
        std::printf("%s:", names[mode]);
        for (double frequency : frequencies) {
            double ratio = measure(dsp, static_cast<FilterMode>(mode), frequency);
            std::printf("  %.0fHz=%+.1fdB", frequency, 20.0 * std::log10(ratio));
        }
        std::puts("");
        if (measure(dsp, static_cast<FilterMode>(mode), 200) < 0.85) {
            std::fprintf(stderr, "FAIL: %s excessively reduced its 200 Hz passband\n", names[mode]);
            return 1;
        }
    }
    if (measure(dsp, FilterMode::General, 5) > 0.1 ||
        measure(dsp, FilterMode::General, 3000) > 0.1 ||
        measure(dsp, FilterMode::Heart, 1500) > 0.1 ||
        measure(dsp, FilterMode::Lung, 50) > 0.1) {
        std::fprintf(stderr, "FAIL: an out-of-band tone was not reduced by at least 20 dB\n");
        return 1;
    }
    std::puts("PASS: all three actual filter profiles pass/reject the expected tones");

    // Test the same gain codes sent by the GUI, including all fractional choices.
    unsigned codes[6] = {phono::protocol::GAIN_HALF, phono::protocol::GAIN_THREE_QUARTERS,
                        phono::protocol::GAIN_FOUR_FIFTHS, 1, 2, 20};
    int expected[6] = {500, 750, 800, 1000, 2000, 20000};
    float input[1] = {1000};
    int16_t output[1];
    for (int i = 0; i < 6; i++) {
        float gain = phono::protocol::gain_multiplier(codes[i]);
        dsp.applyGainWithLimiter(input, output, 1, gain);
        std::printf("Gain code %u -> %.2fx: 1000 -> %d\n", codes[i], gain, output[0]);
        if (output[0] != expected[i]) return 1;
    }
    float loud[2] = {30000, -30000};
    int16_t limited[2];
    dsp.applyGainWithLimiter(loud, limited, 2, 20);
    if (limited[0] != 32767 || limited[1] != -32768) {
        std::fprintf(stderr, "FAIL: limiter did not clamp to PCM16 limits\n");
        return 1;
    }
    std::puts("PASS: gain codes, attenuation, amplification and limiter");

    // The project uses a Hann window and a rolling 1024-point FFT with 512-sample hops.
    int16_t sine[1024];
    float magnitudes[512];
    for (int i = 0; i < 1024; i++) {
        sine[i] = static_cast<int16_t>(8192.0 * std::sin(2.0 * kPi * 500 * i / kSampleRate));
    }
    dsp.reset();
    dsp.computeFFT(sine, magnitudes, 512);
    if (dsp.fftReady()) return 1; // Only half a window is available.
    dsp.computeFFT(sine + 512, magnitudes, 512);
    if (!dsp.fftReady() || std::fabs(magnitudes[64] - 0.25F) > 0.003F) {
        std::fprintf(stderr, "FAIL: expected project FFT bin 64 at amplitude 0.25\n");
        return 1;
    }
    std::printf("PASS: project's windowed FFT, 500 Hz bin 64 = %.4f\n", magnitudes[64]);
    return 0;
}
