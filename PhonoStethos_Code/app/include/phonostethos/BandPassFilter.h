#pragma once

#include <cmath>

using namespace std;

namespace phono_app {
class BiquadFilter {
public:
    void configure_high_pass(double cutoff, double sample_rate, double q) {
        configure(cutoff, sample_rate, true, q);
    }

    void configure_low_pass(double cutoff, double sample_rate, double q) {
        configure(cutoff, sample_rate, false, q);
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
    void configure(double cutoff, double sample_rate, bool high_pass, double q) {
        const double omega = 2.0 * 3.14159265358979323846 * cutoff /
                             sample_rate;
        const double cosine = cos(omega);
        const double alpha = sin(omega) / (2.0 * q);
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
        // Two sections with different Q values form a fourth-order Butterworth edge.
        high_pass_.configure_high_pass(low, sample_rate, 0.541196100146197);
        high_pass_second_.configure_high_pass(low, sample_rate, 1.306562964876377);
        low_pass_.configure_low_pass(high, sample_rate, 0.541196100146197);
        low_pass_second_.configure_low_pass(high, sample_rate, 1.306562964876377);
        reset();
    }

    void reset() {
        high_pass_.reset();
        low_pass_.reset();
        high_pass_second_.reset();
        low_pass_second_.reset();
    }

    double process(double input) {
        return low_pass_second_.process(low_pass_.process(
            high_pass_second_.process(high_pass_.process(input))));
    }

private:
    BiquadFilter high_pass_;
    BiquadFilter low_pass_;
    BiquadFilter high_pass_second_;
    BiquadFilter low_pass_second_;
};
}  // namespace phono_app
