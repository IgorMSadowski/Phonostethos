#include <stdbool.h>
// Test the character-device mask interface and inspect the actual LEDs by eye.
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t interrupted = 0;
static void stop_test(int) { interrupted = 1; }

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--help") == 0)) {
        puts("Usage: LedTest [/dev/phonostethos_leds]\nObserve each LED; Ctrl+C turns all off and exits.");
        return 2;
    }
    const char* path = "/dev/phonostethos_leds";
    if (argc == 2) path = argv[1];
    // API: open obtains a descriptor for the existing kernel character device.
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("FAIL: open LED device");
        return 1;
    }
    signal(SIGINT, stop_test);
    signal(SIGTERM, stop_test);
    uint8_t masks[6] = {0x01, 0x02, 0x04, 0x08, 0x0f, 0x00};
    const char* expected[6] = {"HEART (GPIO17)", "LUNG (GPIO27)", "GENERAL (GPIO24)",
                               "RECORDING (GPIO22)", "ALL FOUR", "ALL OFF"};
    bool success = true;
    for (int i = 0; i < 6 && !interrupted; i++) {
        printf("Mask 0x%02x: expect %s\n", masks[i], expected[i]);
        fflush(stdout);
        /* API: write(fd, &mask, 1)
         * Send one BINARY mask byte. This is not the text "01" or "1".
         * Each write replaces the complete state of all four LEDs.
         */
        if (write(fd, &masks[i], 1) != 1) {
            perror("FAIL: LED write");
            success = false;
            break;
        }
        struct timespec pause = {1, 0};
        while (nanosleep(&pause, &pause) != 0 && !interrupted) {}
    }
    uint8_t off = 0;
    if (write(fd, &off, 1) != 1) success = false;
    if (close(fd) != 0) success = false;
    if (!success) return 1;
    if (interrupted) {
        puts("INCOMPLETE: interrupted; all-off mask submitted");
        return 3;
    }
    puts("PASS: mask write/close calls accepted; VISUAL RESULT still requires your observation.");
    puts("The driver has no electrical feedback and cannot detect a disconnected LED.");
    return 0;
}
