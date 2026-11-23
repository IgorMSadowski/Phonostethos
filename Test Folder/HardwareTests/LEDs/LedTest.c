/**
 * LedTest.c - checks Led Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - stop_test(): stop test.
 * - main(): run this standalone test and return success only if its checks pass.
 */

#include <stdbool.h> // Include C bool, true and false.
// Test the character-device mask interface and inspect the actual LEDs by eye.
#include <fcntl.h> // Include descriptor open flags such as O_CLOEXEC and exclusive creation.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.
#include <signal.h> // Include the signal.h declarations used by this source file.
#include <stdint.h> // Include fixed-width PCM and protocol integer types.
#include <stdio.h> // Include printf/fprintf/puts console diagnostics.
#include <string.h> // Include memory/string copying, comparisons and strerror diagnostics.
#include <time.h> // Include timespec, monotonic clocks and timed sleeps.

static volatile sig_atomic_t interrupted = 0; // Complete this expression or declaration.
static void stop_test(int) { interrupted = 1; } // Continue the expression or parameter list from the preceding line.

int main(int argc, char** argv) { // Run this standalone test and return success only if its checks pass.
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--help") == 0)) { // Test the condition argc > 2 || (argc == 2 && strcmp(argv[1], "--help") == 0).
        puts("Usage: LedTest [/dev/phonostethos_leds]\nObserve each LED; Ctrl+C turns all off and exits."); // Call puts with the arguments shown here.
        return 2; // Return 2 to the caller.
    } // End this block, declaration or argument list.
    const char* path = "/dev/phonostethos_leds"; // Initialize path to "/dev/phonostethos_leds".
    if (argc == 2) path = argv[1]; // Test the condition argc == 2. If true, set path to argv[1].
    // API: open obtains a descriptor for the existing kernel character device.
    int fd = open(path, O_WRONLY | O_CLOEXEC); // Initialize fd to open(path, O_WRONLY | O_CLOEXEC).
    if (fd < 0) { // Test the condition fd < 0.
        perror("FAIL: open LED device"); // Call perror with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    /* API: signal
     * Register the SIGINT/SIGTERM service-stop callback. The callback only requests stop; ordinary code performs worker/resource teardown.
     */
    signal(SIGINT, stop_test); // Call signal with the arguments shown here.
    signal(SIGTERM, stop_test); // Call signal with the arguments shown here.
    uint8_t masks[6] = {0x01, 0x02, 0x04, 0x08, 0x0f, 0x00}; // Initialize masks to {0x01, 0x02, 0x04, 0x08, 0x0f, 0x00}.
    const char* expected[6] = {"HEART (GPIO17)", "LUNG (GPIO27)", "GENERAL (GPIO24)", // Initialize expected to {"HEART (GPIO17)", "LUNG (GPIO27)", "GENERAL (GPIO24)".
                               "RECORDING (GPIO22)", "ALL FOUR", "ALL OFF"}; // Complete this expression or declaration.
    bool success = true; // Initialize success to true.
    for (int i = 0; i < 6 && !interrupted; i++) { // Visit the specified items/indexes in order.
        printf("Mask 0x%02x: expect %s\n", masks[i], expected[i]); // Print the diagnostic or test result for the operator.
        fflush(stdout); // Call fflush with the arguments shown here.
        /* API: write(fd, &mask, 1)
         * Send one BINARY mask byte. This is not the text "01" or "1".
         * Each write replaces the complete state of all four LEDs.
         */
        if (write(fd, &masks[i], 1) != 1) { // Test the condition write(fd, &masks[i], 1) != 1.
            perror("FAIL: LED write"); // Call perror with the arguments shown here.
            success = false; // Set success to false.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        struct timespec pause = {1, 0}; // Group the related fields in one plain data record.
        /* API: nanosleep
         * Sleep for the relative timespec interval instead of spinning while idle. A signal may interrupt the sleep before the whole interval has elapsed.
         */
        while (nanosleep(&pause, &pause) != 0 && !interrupted) {} // Repeat this block while its condition remains true; recheck shared state on each pass.
    } // End this block, declaration or argument list.
    uint8_t off = 0; // Initialize off to 0.
    if (write(fd, &off, 1) != 1) success = false; // Test the condition write(fd, &off, 1) != 1. If true, set success to false.
    if (close(fd) != 0) success = false; // Test the condition close(fd) != 0. If true, set success to false.
    if (!success) return 1; // Test the condition !success. If true, return 1 to the caller.
    if (interrupted) { // Test the condition interrupted.
        puts("INCOMPLETE: interrupted; all-off mask submitted"); // Call puts with the arguments shown here.
        return 3; // Return 3 to the caller.
    } // End this block, declaration or argument list.
    puts("PASS: mask write/close calls accepted; VISUAL RESULT still requires your observation."); // Call puts with the arguments shown here.
    puts("The driver has no electrical feedback and cannot detect a disconnected LED."); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
