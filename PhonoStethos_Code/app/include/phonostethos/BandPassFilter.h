#pragma once

#include <cmath>

// Coefficient reference: Robert Bristow-Johnson's Audio EQ Cookbook (W3C).
// https://www.w3.org/TR/audio-eq-cookbook/
// We use its LPF (low-pass) and HPF (high-pass) equations, normalized by a0.
// BandPassFilter combines these sections to keep frequencies between two cutoffs.

using namespace std;

namespace phono_app {
// One biquad is a second-order IIR filter. It remembers two previous inputs
// and two previous outputs, so each new sample can be processed immediately.
class BiquadFilter {
public:
    // Reject low frequencies below cutoff. All frequencies are specified in Hz.
    void configure_high_pass(double cutoff, double sample_rate, double q) {
        configure(cutoff, sample_rate, true, q);
    }

    // Reject high frequencies above cutoff.
    void configure_low_pass(double cutoff, double sample_rate, double q) {
        configure(cutoff, sample_rate, false, q);
    }

    void reset() {
        // Forget the previous signal, for example after changing filter mode.
        x1_ = 0.0;
        x2_ = 0.0;
        y1_ = 0.0;
        y2_ = 0.0;
    }

    double process(double input) {
        // Cookbook Direct Form I difference equation:
        // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
        //                  - a1*y[n-1] - a2*y[n-2].
        // The coefficients already include division by a0 during configure().
        const double output = b0_ * input + b1_ * x1_ + b2_ * x2_ -
                              a1_ * y1_ - a2_ * y2_;
        // Shift the saved samples: the current sample becomes the previous one.
        x2_ = x1_;
        x1_ = input;
        y2_ = y1_;
        y1_ = output;
        return output;
    }

private:
    void configure(double cutoff, double sample_rate, bool high_pass, double q) {
        // cutoff = desired corner frequency; sample_rate = samples per second.
        // q controls the damping/resonance of this individual second-order section.
        // Convert the cutoff from Hz into radians per sample: omega = 2*pi*fc/Fs.
        const double omega = 2.0 * 3.14159265358979323846 * cutoff / sample_rate;
        const double cosine = cos(omega);
        // Cookbook intermediate value for the Q-based coefficient equations.
        const double alpha = sin(omega) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        // Divide every coefficient by a0 here, so process() needs no division.
        // Only the numerator coefficients differ between these HPF and LPF forms.
        if (high_pass) {
            // Cookbook HPF: b0 = (1+cos)/2; b1 = -(1+cos), before normalization.
            b0_ = (1.0 + cosine) / (2.0 * a0);
            b1_ = -(1.0 + cosine) / a0;
        } else {
            // Cookbook LPF: b0 = (1-cos)/2; b1 = 1-cos, before normalization.
            b0_ = (1.0 - cosine) / (2.0 * a0);
            b1_ = (1.0 - cosine) / a0;
        }
        // Both forms have b2 = b0 and the same denominator coefficients.
        b2_ = b0_;
        a1_ = -2.0 * cosine / a0;
        a2_ = (1.0 - alpha) / a0;
    }

    // Feedforward coefficients multiply the current and previous input samples.
    double b0_ = 1.0;
    double b1_ = 0.0;
    double b2_ = 0.0;
    // Feedback coefficients multiply previous outputs (subtracted in process()).
    double a1_ = 0.0;
    double a2_ = 0.0;
    // x1/x2: inputs from one/two samples ago; y1/y2: corresponding past outputs.
    double x1_ = 0.0;
    double x2_ = 0.0;
    double y1_ = 0.0;
    double y2_ = 0.0;
};

// Signal path: HPF section 1 -> HPF section 2 -> LPF section 1 -> LPF section 2.
// Each edge is fourth order; the complete cascade contains four biquads.
class BandPassFilter {
public:
    void configure(double low, double high, double sample_rate) {
        // low is the lower cutoff; high is the upper cutoff, both in Hz.
        // These Q values come from the fourth-order Butterworth pole positions:
        // Q1 = 1 / (2*cos(pi/8))   = 0.541196100146197
        // Q2 = 1 / (2*cos(3*pi/8)) = 1.306562964876377.
        // Each pair forms a Butterworth edge with a flat passband response.
        // The Cookbook supplies the biquad equations; Butterworth sets their Qs.
        high_pass_.configure_high_pass(low, sample_rate, 0.541196100146197);
        high_pass_second_.configure_high_pass(low, sample_rate, 1.306562964876377);
        low_pass_.configure_low_pass(high, sample_rate, 0.541196100146197);
        low_pass_second_.configure_low_pass(high, sample_rate, 1.306562964876377);
        // Start all four sections with an empty history after reconfiguration.
        reset();
    }

    void reset() {
        high_pass_.reset();
        low_pass_.reset();
        high_pass_second_.reset();
        low_pass_second_.reset();
    }

    double process(double input) {
        // Process one sample through all four sections, using the same order
        // as the signal path above. Each section maintains its own history.
        double sample = high_pass_.process(input);
        sample = high_pass_second_.process(sample);
        sample = low_pass_.process(sample);
        sample = low_pass_second_.process(sample);
        return sample;
    }

private:
    BiquadFilter high_pass_;
    BiquadFilter low_pass_;
    BiquadFilter high_pass_second_;
    BiquadFilter low_pass_second_;
};
}  // namespace phono_app
