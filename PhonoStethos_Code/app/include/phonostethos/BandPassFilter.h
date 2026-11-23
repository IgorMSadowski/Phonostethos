/**
 * BandPassFilter.h - implements the floating-point high-pass/low-pass biquad cascade.
 *
 * CONNECTIONS: DspProcessor owns this header-only filter. It uses the linked W3C Audio EQ Cookbook formulas, normalized by a0, with two biquads per fourth-order Butterworth edge. Filter state persists between samples and is reset at stream/profile transitions.
 *
 * FUNCTIONS:
 * - configure_high_pass(): select the Cookbook high-pass coefficients for one second-order section.
 * - configure_low_pass(): select the Cookbook low-pass coefficients for one second-order section.
 * - reset(): clear processing history so a new stream does not inherit old samples.
 * - process(): advance the persistent IIR state and return one filtered sample.
 * - configure(): calculate normalized biquad coefficients for the selected cutoff/rate/Q.
 */

#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include <cmath> // Include trigonometry, square roots, logarithms and finite-value checks.

// Coefficient reference: Robert Bristow-Johnson's Audio EQ Cookbook (W3C).
// https://www.w3.org/TR/audio-eq-cookbook/
// We use its LPF (low-pass) and HPF (high-pass) equations, normalized by a0.
// BandPassFilter combines these sections to keep frequencies between two cutoffs.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// One biquad is a second-order IIR filter. It remembers two previous inputs
// and two previous outputs, so each new sample can be processed immediately.
class BiquadFilter { // Declare BiquadFilter and its related operations/state.
public: // The class operations callable by other code.
    // Reject low frequencies below cutoff. All frequencies are specified in Hz.
    void configure_high_pass(double cutoff, double sample_rate, double q) { // Finish the parameters/condition and enter the block below.
        configure(cutoff, sample_rate, true, q); // Call configure with the arguments shown here.
    } // End this block, declaration or argument list.

    // Reject high frequencies above cutoff.
    void configure_low_pass(double cutoff, double sample_rate, double q) { // Finish the parameters/condition and enter the block below.
        configure(cutoff, sample_rate, false, q); // Call configure with the arguments shown here.
    } // End this block, declaration or argument list.

    void reset() { // Clear processing history so a new stream does not inherit old samples.
        // Forget the previous signal, for example after changing filter mode.
        x1_ = 0.0; // Set x1 to 0.0.
        x2_ = 0.0; // Set x2 to 0.0.
        y1_ = 0.0; // Set y1 to 0.0.
        y2_ = 0.0; // Set y2 to 0.0.
    } // End this block, declaration or argument list.

    double process(double input) { // Finish the parameters/condition and enter the block below.
        // Cookbook Direct Form I difference equation:
        // y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2]
        //                  - a1*y[n-1] - a2*y[n-2].
        // The coefficients already include division by a0 during configure().
        const double output = b0_ * input + b1_ * x1_ + b2_ * x2_ - // Initialize output to b0_ * input + b1_ * x1_ + b2_ * x2_ -.
                              a1_ * y1_ - a2_ * y2_; // Complete this expression or declaration.
        // Shift the saved samples: the current sample becomes the previous one.
        x2_ = x1_; // Set x2 to x1_.
        x1_ = input; // Set x1 to input.
        y2_ = y1_; // Set y2 to y1_.
        y1_ = output; // Set y1 to output.
        return output; // Return output to the caller.
    } // End this block, declaration or argument list.

private: // Internal helpers and state used by this class.
    void configure(double cutoff, double sample_rate, bool high_pass, double q) { // Finish the parameters/condition and enter the block below.
        // cutoff = desired corner frequency; sample_rate = samples per second.
        // q controls the damping/resonance of this individual second-order section.
        // Convert the cutoff from Hz into radians per sample: omega = 2*pi*fc/Fs.
        const double omega = 2.0 * 3.14159265358979323846 * cutoff / sample_rate; // Initialize omega to 2.0 * 3.14159265358979323846 * cutoff / sample_rate.
        const double cosine = cos(omega); // Initialize cosine to cos(omega).
        // Cookbook intermediate value for the Q-based coefficient equations.
        const double alpha = sin(omega) / (2.0 * q); // Initialize alpha to sin(omega) / (2.0 * q).
        const double a0 = 1.0 + alpha; // Initialize a0 to 1.0 + alpha.
        // Divide every coefficient by a0 here, so process() needs no division.
        // Only the numerator coefficients differ between these HPF and LPF forms.
        if (high_pass) { // Test the condition high_pass.
            // Cookbook HPF: b0 = (1+cos)/2; b1 = -(1+cos), before normalization.
            b0_ = (1.0 + cosine) / (2.0 * a0); // Set b0 to (1.0 + cosine) / (2.0 * a0).
            b1_ = -(1.0 + cosine) / a0; // Set b1 to -(1.0 + cosine) / a0.
        } else { // Follow the alternative path when the previous condition is false.
            // Cookbook LPF: b0 = (1-cos)/2; b1 = 1-cos, before normalization.
            b0_ = (1.0 - cosine) / (2.0 * a0); // Set b0 to (1.0 - cosine) / (2.0 * a0).
            b1_ = (1.0 - cosine) / a0; // Set b1 to (1.0 - cosine) / a0.
        } // End this block, declaration or argument list.
        // Both forms have b2 = b0 and the same denominator coefficients.
        b2_ = b0_; // Set b2 to b0_.
        a1_ = -2.0 * cosine / a0; // Set a1 to -2.0 * cosine / a0.
        a2_ = (1.0 - alpha) / a0; // Set a2 to (1.0 - alpha) / a0.
    } // End this block, declaration or argument list.

    // Feedforward coefficients multiply the current and previous input samples.
    double b0_ = 1.0; // Initialize b0 to 1.0.
    double b1_ = 0.0; // Initialize b1 to 0.0.
    double b2_ = 0.0; // Initialize b2 to 0.0.
    // Feedback coefficients multiply previous outputs (subtracted in process()).
    double a1_ = 0.0; // Initialize a1 to 0.0.
    double a2_ = 0.0; // Initialize a2 to 0.0.
    // x1/x2: inputs from one/two samples ago; y1/y2: corresponding past outputs.
    double x1_ = 0.0; // Initialize x1 to 0.0.
    double x2_ = 0.0; // Initialize x2 to 0.0.
    double y1_ = 0.0; // Initialize y1 to 0.0.
    double y2_ = 0.0; // Initialize y2 to 0.0.
}; // End this block, declaration or argument list.

// Signal path: HPF section 1 -> HPF section 2 -> LPF section 1 -> LPF section 2.
// Each edge is fourth order; the complete cascade contains four biquads.
class BandPassFilter { // Declare BandPassFilter and its related operations/state.
public: // The class operations callable by other code.
    void configure(double low, double high, double sample_rate) { // Finish the parameters/condition and enter the block below.
        // low is the lower cutoff; high is the upper cutoff, both in Hz.
        // These Q values come from the fourth-order Butterworth pole positions:
        // Q1 = 1 / (2*cos(pi/8))   = 0.541196100146197
        // Q2 = 1 / (2*cos(3*pi/8)) = 1.306562964876377.
        // Each pair forms a Butterworth edge with a flat passband response.
        // The Cookbook supplies the biquad equations; Butterworth sets their Qs.
        high_pass_.configure_high_pass(low, sample_rate, 0.541196100146197); // Call high_pass_.configure_high_pass with the arguments shown here.
        high_pass_second_.configure_high_pass(low, sample_rate, 1.306562964876377); // Call high_pass_second_.configure_high_pass with the arguments shown here.
        low_pass_.configure_low_pass(high, sample_rate, 0.541196100146197); // Call low_pass_.configure_low_pass with the arguments shown here.
        low_pass_second_.configure_low_pass(high, sample_rate, 1.306562964876377); // Call low_pass_second_.configure_low_pass with the arguments shown here.
        // Start all four sections with an empty history after reconfiguration.
        reset(); // Clear processing history so a new stream does not inherit old samples.
    } // End this block, declaration or argument list.

    void reset() { // Clear processing history so a new stream does not inherit old samples.
        high_pass_.reset(); // Call high_pass_.reset with the arguments shown here.
        low_pass_.reset(); // Call low_pass_.reset with the arguments shown here.
        high_pass_second_.reset(); // Call high_pass_second_.reset with the arguments shown here.
        low_pass_second_.reset(); // Call low_pass_second_.reset with the arguments shown here.
    } // End this block, declaration or argument list.

    double process(double input) { // Finish the parameters/condition and enter the block below.
        // Process one sample through all four sections, using the same order
        // as the signal path above. Each section maintains its own history.
        double sample = high_pass_.process(input); // Initialize sample to high_pass_.process(input).
        sample = high_pass_second_.process(sample); // Set sample to high_pass_second_.process(sample).
        sample = low_pass_.process(sample); // Set sample to low_pass_.process(sample).
        sample = low_pass_second_.process(sample); // Set sample to low_pass_second_.process(sample).
        return sample; // Return sample to the caller.
    } // End this block, declaration or argument list.

private: // Internal helpers and state used by this class.
    BiquadFilter high_pass_; // Declare high pass for this class instance.
    BiquadFilter low_pass_; // Declare low pass for this class instance.
    BiquadFilter high_pass_second_; // Declare high pass second for this class instance.
    BiquadFilter low_pass_second_; // Declare low pass second for this class instance.
}; // End this block, declaration or argument list.
}  // namespace phono_app
