#pragma once

#include <cmath>

using namespace std;

namespace phono_app {
class BiquadFilter {
public:
    void configure_high_pass(double cutoff, double sample_rate) {
        configure(cutoff, sample_rate, true);
    }

    void configure_low_pass(double cutoff, double sample_rate) {
        configure(cutoff, sample_rate, false);
    }

    void reset() {
        x1_ = 0.0;
        x2_ = 0.0;
        y1_ = 0.0;
        y2_ = 0.0;
    }

    double process(double input) {
        const double output = b0_ * input + b1_ * x1_ + b2_ * x2_ -
                              a1_ * y1_ - a2_ * y2_;
        x2_ = x1_;
        x1_ = input;
        y2_ = y1_;
        y1_ = output;
        return output;
    }

private:
    void configure(double cutoff, double sample_rate, bool high_pass) {
        constexpr double kButterworthQ = 0.7071067811865475244;
        const double omega = 2.0 * 3.14159265358979323846 * cutoff /
                             sample_rate;
        const double cosine = cos(omega);
        const double alpha = sin(omega) / (2.0 * kButterworthQ);
        const double a0 = 1.0 + alpha;
        if (high_pass) {
            b0_ = (1.0 + cosine) / (2.0 * a0);
            b1_ = -(1.0 + cosine) / a0;
        } else {
            b0_ = (1.0 - cosine) / (2.0 * a0);
            b1_ = (1.0 - cosine) / a0;
        }
        b2_ = b0_;
        a1_ = -2.0 * cosine / a0;
        a2_ = (1.0 - alpha) / a0;
    }

    double b0_ = 1.0;
    double b1_ = 0.0;
    double b2_ = 0.0;
    double a1_ = 0.0;
    double a2_ = 0.0;
    double x1_ = 0.0;
    double x2_ = 0.0;
    double y1_ = 0.0;
    double y2_ = 0.0;
};

class BandPassFilter {
public:
    void configure(double low, double high, double sample_rate) {
        high_pass_.configure_high_pass(low, sample_rate);
        low_pass_.configure_low_pass(high, sample_rate);
        reset();
    }

    void reset() {
        high_pass_.reset();
        low_pass_.reset();
    }

    double process(double input) {
        return low_pass_.process(high_pass_.process(input));
    }

private:
    BiquadFilter high_pass_;
    BiquadFilter low_pass_;
};
}  // namespace phono_app
