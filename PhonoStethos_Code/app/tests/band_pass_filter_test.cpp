#include "phonostethos/BandPassFilter.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {
double output_rms(phono_app::BandPassFilter& filter, double frequency) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kRate = 8000.0;
    constexpr std::size_t kSamples = 8000;
    constexpr std::size_t kSkip = 1000;
    filter.reset();
    double sum_squares = 0.0;
    for (std::size_t i = 0; i < kSamples; ++i) {
        const auto input =
            std::sin(2.0 * kPi * frequency * i / kRate);
        const auto output = filter.process(input);
        if (i >= kSkip) sum_squares += output * output;
    }
    return std::sqrt(sum_squares / (kSamples - kSkip));
}
}  // namespace

int main() {
    phono_app::BandPassFilter general;
    phono_app::BandPassFilter heart;
    phono_app::BandPassFilter lung;
    general.configure(20.0, 2000.0, 8000.0);
    heart.configure(20.0, 600.0, 8000.0);
    lung.configure(100.0, 1000.0, 8000.0);

    const auto general_low = output_rms(general, 50.0);
    const auto lung_low = output_rms(lung, 50.0);
    if (lung_low >= general_low * 0.35) {
        std::cerr << "Lung filter did not attenuate frequencies below 100 Hz\n";
        return 1;
    }

    const auto general_high = output_rms(general, 1500.0);
    const auto heart_high = output_rms(heart, 1500.0);
    if (heart_high >= general_high * 0.35) {
        std::cerr << "Heart filter did not attenuate frequencies above 600 Hz\n";
        return 1;
    }

    if (output_rms(general, 500.0) <= 0.5 ||
        output_rms(heart, 500.0) <= 0.5 ||
        output_rms(lung, 500.0) <= 0.5) {
        std::cerr << "A filter attenuated a frequency inside its passband\n";
        return 1;
    }
    return 0;
}
