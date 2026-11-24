/**
 * LedDriver.h - wraps the LED character-device descriptor in user space.
 *
 * CONNECTIONS: LedDriver.h declares the wrapper; LedDriver.cpp checks open/write/close. SystemController writes a complete mask. kernel-driver/phonostethos_leds.c maps its four bits to the four fixed GPIO outputs; there is no physical LED feedback.
 *
 * FUNCTIONS:
 * - LedDriver(): connect the owned resources/references used by this class.
 * - openDriver(): open the LED character device and report whether access succeeded.
 * - closeDriver(): turn the LEDs off and release the user-space device descriptor.
 * - writeByte(): send one complete four-bit LED mask and check the returned byte count.
 */



#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include <cstdint> // Include integer types with exact bit widths used by PCM and the binary protocol.
#include <string> // Include owned filenames, device paths and diagnostic text.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
class LedDriver { // Declare LedDriver and its related operations/state.
public: // The class operations callable by other code.
    explicit LedDriver(const std::string& path); // Connect the owned resources/references used by this class.

    bool openDriver(); // Open the LED character device and report whether access succeeded.
    void closeDriver(); // Turn the LEDs off and release the user-space device descriptor.
    bool writeByte(std::uint8_t mask); // Send one complete four-bit LED mask and check the returned byte count.

    ~LedDriver(); // Complete the call above with these remaining arguments.

private: // Internal helpers and state used by this class.
    std::string path_; // Declare path for this class instance.
    int fd_ = -1; // Initialize fd to -1.
}; // End this block, declaration or argument list.
}  // namespace phono_app
