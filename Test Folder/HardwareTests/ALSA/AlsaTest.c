/**
 * AlsaTest.c - checks Alsa Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - list_devices(): list devices.
 * - open_pcm(): open pcm.
 * - record_file(): record file.
 * - play_file(): play file.
 * - main(): run this standalone test and return success only if its checks pass.
 */

// API DOCUMENTATION FOR THIS FILE:
// libsndfile: SNDFILE/SF_INFO, WAV open, sample I/O, errors and checked close:
// https://libsndfile.github.io/libsndfile/api.html
// ALSA PCM: stream setup, capture, output and recovery:
// https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html
// POSIX descriptor open/flags; related read/write/close system calls:
// https://man7.org/linux/man-pages/man2/open.2.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include <stdbool.h> // Include C bool, true and false.
// Standalone microphone/headphone check, based on the original DD_AlsaTest.
// Uses libsndfile for WAV headers instead of assuming every WAV has 44 header bytes.
// ALSA API: https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html
#include <alsa/asoundlib.h> // Include ALSA PCM handles, format constants and audio input/output APIs.
#include <sndfile.h> // Include libsndfile WAV handles, format descriptors and PCM read/write APIs.
#include <fcntl.h> // Include descriptor open flags such as O_CLOEXEC and exclusive creation.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.
#include <math.h> // Include trigonometric functions and floating-point magnitude calculations.
#include <stdio.h> // Include printf/fprintf/puts console diagnostics.
#include <stdlib.h> // Include process exit, allocation and numeric conversion helpers.
#include <string.h> // Include memory/string copying, comparisons and strerror diagnostics.

static int list_devices(void) { // Finish the parameters/condition and enter the block below.
    void** hints = NULL; // Initialize hints to NULL.
    int result = snd_device_name_hint(-1, "pcm", &hints); // Initialize result to snd_device_name_hint(-1, "pcm", &hints).
    if (result < 0) { // Test the condition result < 0.
        fprintf(stderr, "FAIL: PCM hints: %s\n", snd_strerror(result)); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    // API: snd_device_name_* lists configured PCM names, including plugins.
    // A name being listed does not prove its microphone/headphones work.
    for (void** item = hints; *item != NULL; item++) { // Visit the specified items/indexes in order.
        char* name = snd_device_name_get_hint(*item, "NAME"); // Initialize name to snd_device_name_get_hint(*item, "NAME").
        char* direction = snd_device_name_get_hint(*item, "IOID"); // Initialize direction to snd_device_name_get_hint(*item, "IOID").
        if (name != NULL) printf("%s (%s)\n", name, direction == NULL ? "both/unspecified" : direction); // Test the condition name != NULL. If true, print the diagnostic or test result for the operator.
        free(name); // Call free with the arguments shown here.
        free(direction); // Call free with the arguments shown here.
    } // End this block, declaration or argument list.
    snd_device_name_free_hint(hints); // Call snd_device_name_free_hint with the arguments shown here.
    puts("PASS: ALSA device enumeration; use rec/play to test a chosen device"); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

static bool open_pcm(snd_pcm_t** pcm, const char* name, snd_pcm_stream_t direction) { // Finish the parameters/condition and enter the block below.
    // API: snd_pcm_open opens either a capture or playback PCM stream.
    int result = snd_pcm_open(pcm, name, direction, 0); // Initialize result to snd_pcm_open(pcm, name, direction, 0).
    if (result < 0) { // Test the condition result < 0.
        fprintf(stderr, "FAIL: open %s: %s\n", name, snd_strerror(result)); // Print the diagnostic or test result for the operator.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    /* API: snd_pcm_set_params
     * Documentation: https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html
     * Configure S16_LE, interleaved access, mono, 8000 Hz and allow conversion
     * by the selected plugin. 128000 us requests the diagnostic stream latency;
     * it is not a measurement of the complete project's latency.
     */
    result = snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE, // Set result to snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE,.
                               SND_PCM_ACCESS_RW_INTERLEAVED, 1, 8000, 1, 128000); // Complete the call above with these remaining arguments.
    if (result < 0) { // Test the condition result < 0.
        fprintf(stderr, "FAIL: configure %s: %s\n", name, snd_strerror(result)); // Print the diagnostic or test result for the operator.
        snd_pcm_close(*pcm); // Call snd_pcm_close with the arguments shown here.
        *pcm = NULL; // Complete this expression or declaration.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

static int record_file(const char* path, int seconds, const char* name) { // Finish the parameters/condition and enter the block below.
    snd_pcm_t* pcm = NULL; // Initialize pcm to NULL.
    if (!open_pcm(&pcm, name, SND_PCM_STREAM_CAPTURE)) return 1; // Test the condition !open_pcm(&pcm, name, SND_PCM_STREAM_CAPTURE). If true, return 1 to the caller.
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600); // Initialize fd to open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600).
    if (fd < 0) { // Test the condition fd < 0.
        perror("FAIL: create new WAV (existing files are not replaced)"); // Call perror with the arguments shown here.
        snd_pcm_close(pcm); // Call snd_pcm_close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    SF_INFO info = {0}; // Initialize info to {0}.
    info.samplerate = 8000; // Set info.samplerate to 8000.
    info.channels = 1; // Set info.channels to 1.
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16; // Set info.format to SF_FORMAT_WAV | SF_FORMAT_PCM_16.
    SNDFILE* file = sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE); // Initialize file to sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE).
    if (file == NULL) { // Test the condition file == NULL.
        fprintf(stderr, "FAIL: open WAV: %s\n", sf_strerror(NULL)); // Print the diagnostic or test result for the operator.
        close(fd); // Call close with the arguments shown here.
        snd_pcm_close(pcm); // Call snd_pcm_close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    printf("Recording %d seconds from %s into %s\n", seconds, name, path); // Print the diagnostic or test result for the operator.
    long total = seconds * 8000; // Initialize total to seconds * 8000.
    long done = 0; // Initialize done to 0.
    int recoveries = 0; // Initialize recoveries to 0.
    int peak = 0; // Initialize peak to 0.
    double energy = 0; // Initialize energy to 0.
    bool success = true; // Initialize success to true.
    short samples[512] = {0}; // Initialize samples to {0}.
    while (done < total) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        int requested = 512; // Initialize requested to 512.
        if (total - done < requested) requested = (int)(total - done); // Test the condition total - done < requested. If true, set requested to (int)(total - done).
        // API: snd_pcm_readi returns frames read, which may be a partial block.
        long count = snd_pcm_readi(pcm, samples, requested); // Initialize count to snd_pcm_readi(pcm, samples, requested).
        if (count < 0) { // Test the condition count < 0.
            // API: snd_pcm_recover handles XRUN/suspend/interruption errors.
            if (++recoveries <= 10 && snd_pcm_recover(pcm, (int)(count), 1) >= 0) continue; // Test the condition ++recoveries <= 10 && snd_pcm_recover(pcm, (int)(count), 1) >= 0. If true, skip the rest of this pass and check the next loop iteration.
            fprintf(stderr, "FAIL: capture: %s\n", snd_strerror((int)(count))); // Print the diagnostic or test result for the operator.
            success = false; // Set success to false.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        /* API: sf_write_short
         * Documentation: https://libsndfile.github.io/libsndfile/api.html
         * Write the requested PCM16 sample count from the supplied array. Returns the actual count; compare it with the requested count to detect a short write.
         */
        if (count == 0 || sf_write_short(file, samples, count) != count) { // Test the condition count == 0 || sf_write_short(file, samples, count) != count.
            fprintf(stderr, "FAIL: stalled capture or short WAV write\n"); // Print the diagnostic or test result for the operator.
            success = false; // Set success to false.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        for (int i = 0; i < count; i++) { // Visit the specified items/indexes in order.
            int magnitude = abs((int)(samples[i])); // Initialize magnitude to abs((int)(samples[i])).
            if (magnitude > peak) peak = magnitude; // Test the condition magnitude > peak. If true, set peak to magnitude.
            energy += (double)(samples[i]) * samples[i]; // Update energy using += (double)(samples[i]) * samples[i].
        } // End this block, declaration or argument list.
        done += count; // Update done using += count.
    } // End this block, declaration or argument list.
    /* API: sf_write_sync
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Ask libsndfile to flush pending writes. This function returns void, so check sf_error and the subsequent checked close/synchronization results too.
     */
    sf_write_sync(file); // Call sf_write_sync with the arguments shown here.
    /* API: sf_error
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Read the libsndfile error associated with a handle. Compare with SF_ERR_NO_ERROR before claiming a write/finalization succeeded.
     */
    if (sf_error(file) != SF_ERR_NO_ERROR) success = false; // Test the condition sf_error(file) != SF_ERR_NO_ERROR. If true, set success to false.
    /* API: sf_close
     * Documentation: https://libsndfile.github.io/libsndfile/api.html
     * Finalize/release a libsndfile handle. A nonzero result indicates a close/finalization error. This project closes retained POSIX descriptors separately.
     */
    if (sf_close(file) != 0) success = false; // Test the condition sf_close(file) != 0. If true, set success to false.
    /* API: fsync
     * Documentation: https://man7.org/linux/man-pages/man2/fsync.2.html
     * Synchronize this open file descriptor and report 0 or -1 with errno. FileIO checks it before reporting successful recording finalization.
     */
    if (fsync(fd) != 0) success = false; // Test the condition fsync(fd) != 0. If true, set success to false.
    if (close(fd) != 0) success = false; // Test the condition close(fd) != 0. If true, set success to false.
    if (snd_pcm_close(pcm) < 0) success = false; // Test the condition snd_pcm_close(pcm) < 0. If true, set success to false.
    double rms = 0; // Initialize rms to 0.
    if (done > 0) rms = sqrt(energy / done); // Test the condition done > 0. If true, set rms to sqrt(energy / done).
    printf("Samples: %ld/%ld; peak: %d; RMS: %.1f; recoveries: %d\n", done, total, peak, rms, recoveries); // Print the diagnostic or test result for the operator.
    if (!success || done != total) return 1; // Test the condition !success || done != total. If true, return 1 to the caller.
    puts("PASS: capture/write/close calls succeeded. Play the WAV to assess the actual sound."); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

static int play_file(const char* path, const char* name) { // Finish the parameters/condition and enter the block below.
    SF_INFO info = {0}; // Initialize info to {0}.
    SNDFILE* file = sf_open(path, SFM_READ, &info); // Initialize file to sf_open(path, SFM_READ, &info).
    if (file == NULL) { // Test the condition file == NULL.
        fprintf(stderr, "FAIL: open WAV: %s\n", sf_strerror(NULL)); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    if (info.channels != 1 || info.samplerate != 8000 || // Test the condition info.channels != 1 || info.samplerate != 8000 ||.
        (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV || // Continue this calculation/condition with the next line.
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) { // Finish the parameters/condition and enter the block below.
        fprintf(stderr, "FAIL: use an 8000 Hz mono PCM16 WAV\n"); // Print the diagnostic or test result for the operator.
        sf_close(file); // Call sf_close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    snd_pcm_t* pcm = NULL; // Initialize pcm to NULL.
    if (!open_pcm(&pcm, name, SND_PCM_STREAM_PLAYBACK)) { // Test the condition !open_pcm(&pcm, name, SND_PCM_STREAM_PLAYBACK).
        sf_close(file); // Call sf_close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    printf("Playing %s through %s\n", path, name); // Print the diagnostic or test result for the operator.
    bool success = true; // Initialize success to true.
    int recoveries = 0; // Initialize recoveries to 0.
    long total = 0; // Initialize total to 0.
    short samples[512] = {0}; // Initialize samples to {0}.
    while (success) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        /* API: sf_read_short
         * Documentation: https://libsndfile.github.io/libsndfile/api.html
         * Read at most the requested count of PCM16 samples into the supplied array. Returns the actual count; a short read needs EOF/error classification. Mono samples equal frames.
         */
        long count = sf_read_short(file, samples, 512); // Initialize count to sf_read_short(file, samples, 512).
        if (count == 0) break; // Test the condition count == 0. If true, leave the current loop or switch branch.
        long offset = 0; // Initialize offset to 0.
        while (offset < count) { // Repeat this block while its condition remains true; recheck shared state on each pass.
            // API: snd_pcm_writei may accept only part of this block.
            long written = snd_pcm_writei(pcm, samples + offset, count - offset); // Initialize written to snd_pcm_writei(pcm, samples + offset, count - offset).
            if (written < 0) { // Test the condition written < 0.
                if (++recoveries <= 10 && snd_pcm_recover(pcm, (int)(written), 1) >= 0) continue; // Test the condition ++recoveries <= 10 && snd_pcm_recover(pcm, (int)(written), 1) >= 0. If true, skip the rest of this pass and check the next loop iteration.
                fprintf(stderr, "FAIL: playback: %s\n", snd_strerror((int)(written))); // Print the diagnostic or test result for the operator.
                success = false; // Set success to false.
                break; // Leave the current loop or switch branch.
            } // End this block, declaration or argument list.
            if (written == 0) { // Test the condition written == 0.
                success = false; // Set success to false.
                break; // Leave the current loop or switch branch.
            } // End this block, declaration or argument list.
            offset += written; // Update offset using += written.
        } // End this block, declaration or argument list.
        total += offset; // Update total using += offset.
    } // End this block, declaration or argument list.
    if (sf_error(file) != SF_ERR_NO_ERROR || total != info.frames) success = false; // Test the condition sf_error(file) != SF_ERR_NO_ERROR || total != info.frames. If true, set success to false.
    // API: snd_pcm_drain waits for the final queued samples before closing output.
    if (success && snd_pcm_drain(pcm) < 0) success = false; // Test the condition success && snd_pcm_drain(pcm) < 0. If true, set success to false.
    if (snd_pcm_close(pcm) < 0) success = false; // Test the condition snd_pcm_close(pcm) < 0. If true, set success to false.
    if (sf_close(file) != 0) success = false; // Test the condition sf_close(file) != 0. If true, set success to false.
    if (!success) { // Test the condition !success.
        fprintf(stderr, "FAIL: playback/read/close was incomplete\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    printf("PASS: %ld samples submitted. Confirm you heard the expected sound.\n", total); // Print the diagnostic or test result for the operator.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

int main(int argc, char** argv) { // Run this standalone test and return success only if its checks pass.
    if (argc == 2 && strcmp(argv[1], "list") == 0) return list_devices(); // Test the condition argc == 2 && strcmp(argv[1], "list") == 0. If true, return list_devices() to the caller.
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "rec") == 0) { // Test the condition (argc == 4 || argc == 5) && strcmp(argv[1], "rec") == 0.
        char* end = NULL; // Initialize end to NULL.
        long seconds = strtol(argv[3], &end, 10); // Initialize seconds to strtol(argv[3], &end, 10).
        if (*argv[3] == '\0' || *end != '\0' || seconds < 1 || seconds > 60) { // Test the condition *argv[3] == '\0' || *end != '\0' || seconds < 1 || seconds > 60.
            fprintf(stderr, "Use a duration from 1 to 60 seconds\n"); // Print the diagnostic or test result for the operator.
            return 2; // Return 2 to the caller.
        } // End this block, declaration or argument list.
        const char* name = "plughw:CARD=HiFiAudio,DEV=0"; // Initialize name to "plughw:CARD=HiFiAudio,DEV=0".
        if (argc == 5) name = argv[4]; // Test the condition argc == 5. If true, set name to argv[4].
        return record_file(argv[2], (int)(seconds), name); // Return record_file(argv[2], (int)(seconds), name) to the caller.
    } // End this block, declaration or argument list.
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "play") == 0) { // Test the condition (argc == 3 || argc == 4) && strcmp(argv[1], "play") == 0.
        const char* name = "plughw:CARD=Headphones,DEV=0"; // Initialize name to "plughw:CARD=Headphones,DEV=0".
        if (argc == 4) name = argv[3]; // Test the condition argc == 4. If true, set name to argv[3].
        return play_file(argv[2], name); // Return play_file(argv[2], name) to the caller.
    } // End this block, declaration or argument list.
    puts("Usage: AlsaTest list\n       AlsaTest rec NEW.wav SECONDS [PCM]\n       AlsaTest play INPUT.wav [PCM]"); // Call puts with the arguments shown here.
    return 2; // Return 2 to the caller.
} // End this block, declaration or argument list.
