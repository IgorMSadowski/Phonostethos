/**
 * UsbTest.c - checks Usb Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - main(): run this standalone test and return success only if its checks pass.
 */

// API DOCUMENTATION FOR THIS FILE:
// libsndfile: SNDFILE/SF_INFO, WAV open, sample I/O, errors and checked close:
// https://libsndfile.github.io/libsndfile/api.html
// POSIX descriptor open/flags; related read/write/close system calls:
// https://man7.org/linux/man-pages/man2/open.2.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include <stdbool.h> // Include C bool, true and false.
// Create, sync, reopen and verify one WAV. Existing paths are never overwritten.
// API reference: https://libsndfile.github.io/libsndfile/api.html
#include <sndfile.h> // Include libsndfile WAV handles, format descriptors and PCM read/write APIs.
#include <sys/vfs.h> // Include the sys/vfs.h declarations used by this source file.
#include <fcntl.h> // Include descriptor open flags such as O_CLOEXEC and exclusive creation.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.
#include <math.h> // Include trigonometric functions and floating-point magnitude calculations.
#include <stdio.h> // Include printf/fprintf/puts console diagnostics.
#include <string.h> // Include memory/string copying, comparisons and strerror diagnostics.
#include <limits.h> // Include numeric limits used when validating inputs.

int main(int argc, char** argv) { // Run this standalone test and return success only if its checks pass.
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--local") != 0)) { // Test the condition argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--local") != 0).
        puts("Usage: UsbTest NEW_FILE.wav [--local]"); // Call puts with the arguments shown here.
        puts("Default: require a mounted FAT filesystem. --local skips that hardware check."); // Call puts with the arguments shown here.
        return 2; // Return 2 to the caller.
    } // End this block, declaration or argument list.
    if (argc == 2) { // Test the condition argc == 2.
        char parent[PATH_MAX] = "."; // Initialize parent to ".".
        const char* separator = strrchr(argv[1], '/'); // Initialize separator to strrchr(argv[1], '/').
        if (separator != NULL) { // Test the condition separator != NULL.
            size_t length = (size_t)(separator - argv[1]) + 1; // Initialize length to (size_t)(separator - argv[1]) + 1.
            if (length >= sizeof(parent)) return 2; // Test the condition length >= sizeof(parent). If true, return 2 to the caller.
            memcpy(parent, argv[1], length); // Call memcpy with the arguments shown here.
            parent[length] = '\0'; // Set parent[length] to '\0'.
        } // End this block, declaration or argument list.
        struct statfs filesystem = {0}; // Group the related fields in one plain data record.
        if (statfs(parent, &filesystem) != 0) { // Test the condition statfs(parent, &filesystem) != 0.
            perror("FAIL: statfs"); // Call perror with the arguments shown here.
            return 1; // Return 1 to the caller.
        } // End this block, declaration or argument list.
        if (filesystem.f_type != 0x4d44) { // Linux MSDOS_SUPER_MAGIC: FAT family.
            fprintf(stderr, "FAIL: %s is not on a FAT filesystem; check the USB mount\n", parent); // Print the diagnostic or test result for the operator.
            return 1; // Return 1 to the caller.
        } // End this block, declaration or argument list.
        puts("FAT mount detected (this value alone does not distinguish FAT32 from FAT16)."); // Call puts with the arguments shown here.
    } // End this block, declaration or argument list.
    if (argc == 3) puts("LOCAL MODE: WAV library check only; USB hardware is not tested."); // Test the condition argc == 3. If true, call puts with the arguments shown here.
    // API: open + O_EXCL creates a new file and refuses to replace an existing one.
    int fd = open(argv[1], O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600); // Initialize fd to open(argv[1], O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600).
    if (fd < 0) { // Test the condition fd < 0.
        perror("FAIL: create WAV"); // Call perror with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    SF_INFO info = {0}; // Initialize info to {0}.
    info.samplerate = 8000; // Set info.samplerate to 8000.
    info.channels = 1; // Set info.channels to 1.
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16; // Set info.format to SF_FORMAT_WAV | SF_FORMAT_PCM_16.
    /* API: sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE)
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * libsndfile writes a valid WAV header. SF_FALSE keeps our POSIX descriptor
     * open after sf_close so we can explicitly check fsync after finalization.
     */
    SNDFILE* file = sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE); // Initialize file to sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE).
    if (file == NULL) { // Test the condition file == NULL.
        fprintf(stderr, "FAIL: sf_open_fd: %s\n", sf_strerror(NULL)); // Print the diagnostic or test result for the operator.
        close(fd); // Call close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    short samples[8000]; // Declare samples for this operation.
    for (int i = 0; i < 8000; i++) { // Visit the specified items/indexes in order.
        samples[i] = (short)(4096 * sin(2 * 3.14159265358979323846 * 500 * i / 8000)); // Set samples[i] to (short)(4096 * sin(2 * 3.14159265358979323846 * 500 * i / 8000)).
    } // End this block, declaration or argument list.
    // API: sf_write_short returns the number of samples actually written.
    bool success = sf_write_short(file, samples, 8000) == 8000; // Initialize success to sf_write_short(file, samples, 8000) == 8000.
    /* API: sf_write_sync
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Ask libsndfile to flush pending writes. This function returns void, so check sf_error and the subsequent checked close/synchronization results too.
     */
    sf_write_sync(file); // Force writes and then check the library's error state.
    /* API: sf_error
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Read the libsndfile error associated with a handle. Compare with SF_ERR_NO_ERROR before claiming a write/finalization succeeded.
     */
    if (sf_error(file) != SF_ERR_NO_ERROR) success = false; // Test the condition sf_error(file) != SF_ERR_NO_ERROR. If true, set success to false.
    /* API: sf_close
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Finalize/release a libsndfile handle. A nonzero result indicates a close/finalization error. This project closes retained POSIX descriptors separately.
     */
    if (sf_close(file) != 0) success = false; // Also finalize the WAV header.
    /* API: fsync
     * Documentation: https://man7.org/linux/man-pages/man2/fsync.2.html
     * Synchronize this open file descriptor and report 0 or -1 with errno. FileIO checks it before reporting successful recording finalization.
     */
    if (fsync(fd) != 0) success = false; // Include the finalized header in the sync.
    if (close(fd) != 0) success = false; // Test the condition close(fd) != 0. If true, set success to false.
    if (!success) { // Test the condition !success.
        fprintf(stderr, "FAIL: writing/syncing/closing WAV\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    info = (SF_INFO){0}; // Set info to (SF_INFO){0}.
    file = sf_open(argv[1], SFM_READ, &info); // Set file to sf_open(argv[1], SFM_READ, &info).
    if (file == NULL) { // Test the condition file == NULL.
        fprintf(stderr, "FAIL: reopen WAV: %s\n", sf_strerror(NULL)); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    short readback[8000]; // Declare readback for this operation.
    if (info.frames != 8000 || info.samplerate != 8000 || info.channels != 1 || // Test the condition info.frames != 8000 || info.samplerate != 8000 || info.channels != 1 ||.
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) success = false; // Complete this expression or declaration.
    // API: sf_read_short reads PCM samples; compare them with the originals.
    if (sf_read_short(file, readback, 8000) != 8000) success = false; // Test the condition sf_read_short(file, readback, 8000) != 8000. If true, set success to false.
    if (success) { // Test the condition success.
        for (int i = 0; i < 8000; i++) { // Visit the specified items/indexes in order.
            if (readback[i] != samples[i]) success = false; // Test the condition readback[i] != samples[i]. If true, set success to false.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    if (sf_close(file) != 0) success = false; // Test the condition sf_close(file) != 0. If true, set success to false.
    if (!success) { // Test the condition !success.
        fprintf(stderr, "FAIL: WAV metadata, samples or read-close result differ\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    printf("PASS: 8000 samples written/read exactly; 8 kHz mono PCM16, 1 second\nKept test file: %s\n", argv[1]); // Print the diagnostic or test result for the operator.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
