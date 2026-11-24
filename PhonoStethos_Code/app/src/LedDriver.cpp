/**
 * LedDriver.cpp - wraps the LED character-device descriptor in user space.
 *
 * CONNECTIONS: LedDriver.h declares the wrapper; LedDriver.cpp checks open/write/close. SystemController writes a complete mask. kernel-driver/phonostethos_leds.c maps its four bits to the four fixed GPIO outputs; there is no physical LED feedback.
 *
 * FUNCTIONS:
 * Main methods are listed first; private helpers are implemented at the end.
 * - LedDriver(): connect the owned resources/references used by this class.
 * - ~LedDriver(): release this object's resources after its users have stopped.
 * - openDriver(): open the LED character device and report whether access succeeded.
 * - closeDriver(): turn the LEDs off and release the user-space device descriptor.
 * - writeByte(): send one complete four-bit LED mask and check the returned byte count.
 */

// API DOCUMENTATION FOR THIS FILE:
// POSIX descriptor open/flags; related read/write/close system calls:
// https://man7.org/linux/man-pages/man2/open.2.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include "phonostethos/LedDriver.h" // Include the LedDriver class/types used here; wraps the LED character-device descriptor in user space.

#include <cerrno> // Include errno and named system-call error codes.
#include <cstring> // Include byte copying/comparison and readable system-error strings.
#include <fcntl.h> // Include descriptor open flags such as O_CLOEXEC and exclusive creation.
#include <iostream> // Include console diagnostics and test results.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.

// ==================== OBJECT SETUP ====================

LedDriver::LedDriver(const std::string& path) : path_(path) {} // Store the configured character-device path; openDriver opens it later.

LedDriver::~LedDriver() { closeDriver(); } // Stop the worker and release owned resources during destruction.

// ==================== MAIN METHODS - REPORT ORDER ====================

bool LedDriver::openDriver() { // Open the LED character device and report whether access succeeded.
    /* API: ::open
     * Documentation: https://man7.org/linux/man-pages/man2/open.2.html
     * Open the LED character device for writing. Returns a descriptor or -1.
     * O_CLOEXEC prevents a child program from inheriting the descriptor.
     */
    fd_ = ::open(path_.c_str(), O_WRONLY | O_CLOEXEC); // Set fd to ::open(path_.c_str(), O_WRONLY | O_CLOEXEC).
    if (fd_ >= 0) { // Test the condition fd_ >= 0.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.
    cerr << "LED driver " << path_ << ": " << strerror(errno) // Print the diagnostic or test result for the operator.
              << '\n'; // Append this part of the diagnostic output.
    return false; // Report failure or no available item to the caller.
} // End this function or local scope.

void LedDriver::closeDriver() { // Turn the LEDs off and release the user-space device descriptor.
    if (fd_ >= 0) { // Test the condition fd_ >= 0.
        (void)writeByte(0); // Call  with the arguments shown here.
        /* API: ::close
         * Documentation: https://man7.org/linux/man-pages/man2/close.2.html
         * Release a descriptor after its owner has finished using it. Set the stored descriptor to -1 so it cannot be reused accidentally.
         */
        ::close(fd_); // Release this POSIX descriptor; the owner resets its stored descriptor to -1.
        fd_ = -1; // Set fd to -1.
    } // End this block, declaration or argument list.
} // End this function or local scope.

bool LedDriver::writeByte(std::uint8_t mask) { // Send one complete four-bit LED mask and check the returned byte count.
    if (fd_ < 0) { // Test the condition fd_ < 0.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    /* API: ::write
     * Documentation: https://man7.org/linux/man-pages/man2/write.2.html
     * Copy the requested bytes to the open descriptor. Check the returned count. For the LED device, write exactly one mask byte.
     */
    const auto count = ::write(fd_, &mask, sizeof(mask)); // Initialize count to ::write(fd_, &mask, sizeof(mask)).
    if (count != static_cast<ssize_t>(sizeof(mask))) { // Test the condition count != static_cast<ssize_t>(sizeof(mask)).
        cerr << "write " << path_ << ": "; // Print the diagnostic or test result for the operator.
        if (count < 0) { // Test the condition count < 0.
            cerr << strerror(errno); // Print the diagnostic or test result for the operator.
        } else { // Follow the alternative path when the previous condition is false.
            cerr << "short write"; // Print the diagnostic or test result for the operator.
        } // End this block, declaration or argument list.
        cerr << '\n'; // Print the diagnostic or test result for the operator.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.


}  // namespace phono_app
