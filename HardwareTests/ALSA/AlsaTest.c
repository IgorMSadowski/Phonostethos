#include <stdbool.h>
// Standalone microphone/headphone check, based on the original DD_AlsaTest.
// Uses libsndfile for WAV headers instead of assuming every WAV has 44 header bytes.
// ALSA API: https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html
#include <alsa/asoundlib.h>
#include <sndfile.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int list_devices(void) {
    void** hints = NULL;
    int result = snd_device_name_hint(-1, "pcm", &hints);
    if (result < 0) {
        fprintf(stderr, "FAIL: PCM hints: %s\n", snd_strerror(result));
        return 1;
    }
    // API: snd_device_name_* lists configured PCM names, including plugins.
    // A name being listed does not prove its microphone/headphones work.
    for (void** item = hints; *item != NULL; item++) {
        char* name = snd_device_name_get_hint(*item, "NAME");
        char* direction = snd_device_name_get_hint(*item, "IOID");
        if (name != NULL) printf("%s (%s)\n", name, direction == NULL ? "both/unspecified" : direction);
        free(name);
        free(direction);
    }
    snd_device_name_free_hint(hints);
    puts("PASS: ALSA device enumeration; use rec/play to test a chosen device");
    return 0;
}

static bool open_pcm(snd_pcm_t** pcm, const char* name, snd_pcm_stream_t direction) {
    // API: snd_pcm_open opens either a capture or playback PCM stream.
    int result = snd_pcm_open(pcm, name, direction, 0);
    if (result < 0) {
        fprintf(stderr, "FAIL: open %s: %s\n", name, snd_strerror(result));
        return false;
    }
    /* API: snd_pcm_set_params
     * Configure S16_LE, interleaved access, mono, 8000 Hz and allow conversion
     * by the selected plugin. 128000 us requests the diagnostic stream latency;
     * it is not a measurement of the complete project's latency.
     */
    result = snd_pcm_set_params(*pcm, SND_PCM_FORMAT_S16_LE,
                               SND_PCM_ACCESS_RW_INTERLEAVED, 1, 8000, 1, 128000);
    if (result < 0) {
        fprintf(stderr, "FAIL: configure %s: %s\n", name, snd_strerror(result));
        snd_pcm_close(*pcm);
        *pcm = NULL;
        return false;
    }
    return true;
}

static int record_file(const char* path, int seconds, const char* name) {
    snd_pcm_t* pcm = NULL;
    if (!open_pcm(&pcm, name, SND_PCM_STREAM_CAPTURE)) return 1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("FAIL: create new WAV (existing files are not replaced)");
        snd_pcm_close(pcm);
        return 1;
    }
    SF_INFO info = {0};
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    SNDFILE* file = sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE);
    if (file == NULL) {
        fprintf(stderr, "FAIL: open WAV: %s\n", sf_strerror(NULL));
        close(fd);
        snd_pcm_close(pcm);
        return 1;
    }
    printf("Recording %d seconds from %s into %s\n", seconds, name, path);
    long total = seconds * 8000;
    long done = 0;
    int recoveries = 0;
    int peak = 0;
    double energy = 0;
    bool success = true;
    short samples[512];
    while (done < total) {
        int requested = 512;
        if (total - done < requested) requested = (int)(total - done);
        // API: snd_pcm_readi returns frames read, which may be a partial block.
        long count = snd_pcm_readi(pcm, samples, requested);
        if (count < 0) {
            // API: snd_pcm_recover handles XRUN/suspend/interruption errors.
            if (++recoveries <= 10 && snd_pcm_recover(pcm, (int)(count), 1) >= 0) continue;
            fprintf(stderr, "FAIL: capture: %s\n", snd_strerror((int)(count)));
            success = false;
            break;
        }
        if (count == 0 || sf_write_short(file, samples, count) != count) {
            fprintf(stderr, "FAIL: stalled capture or short WAV write\n");
            success = false;
            break;
        }
        for (int i = 0; i < count; i++) {
            int magnitude = abs((int)(samples[i]));
            if (magnitude > peak) peak = magnitude;
            energy += (double)(samples[i]) * samples[i];
        }
        done += count;
    }
    sf_write_sync(file);
    if (sf_error(file) != SF_ERR_NO_ERROR) success = false;
    if (sf_close(file) != 0) success = false;
    if (fsync(fd) != 0) success = false;
    if (close(fd) != 0) success = false;
    if (snd_pcm_close(pcm) < 0) success = false;
    double rms = 0;
    if (done > 0) rms = sqrt(energy / done);
    printf("Samples: %ld/%ld; peak: %d; RMS: %.1f; recoveries: %d\n", done, total, peak, rms, recoveries);
    if (!success || done != total) return 1;
    puts("PASS: capture/write/close calls succeeded. Play the WAV to assess the actual sound.");
    return 0;
}

static int play_file(const char* path, const char* name) {
    SF_INFO info = {0};
    SNDFILE* file = sf_open(path, SFM_READ, &info);
    if (file == NULL) {
        fprintf(stderr, "FAIL: open WAV: %s\n", sf_strerror(NULL));
        return 1;
    }
    if (info.channels != 1 || info.samplerate != 8000 ||
        (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV ||
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) {
        fprintf(stderr, "FAIL: use an 8000 Hz mono PCM16 WAV\n");
        sf_close(file);
        return 1;
    }
    snd_pcm_t* pcm = NULL;
    if (!open_pcm(&pcm, name, SND_PCM_STREAM_PLAYBACK)) {
        sf_close(file);
        return 1;
    }
    printf("Playing %s through %s\n", path, name);
    bool success = true;
    int recoveries = 0;
    long total = 0;
    short samples[512];
    while (success) {
        long count = sf_read_short(file, samples, 512);
        if (count == 0) break;
        long offset = 0;
        while (offset < count) {
            // API: snd_pcm_writei may accept only part of this block.
            long written = snd_pcm_writei(pcm, samples + offset, count - offset);
            if (written < 0) {
                if (++recoveries <= 10 && snd_pcm_recover(pcm, (int)(written), 1) >= 0) continue;
                fprintf(stderr, "FAIL: playback: %s\n", snd_strerror((int)(written)));
                success = false;
                break;
            }
            if (written == 0) {
                success = false;
                break;
            }
            offset += written;
        }
        total += offset;
    }
    if (sf_error(file) != SF_ERR_NO_ERROR || total != info.frames) success = false;
    // API: snd_pcm_drain waits for the final queued samples before closing output.
    if (success && snd_pcm_drain(pcm) < 0) success = false;
    if (snd_pcm_close(pcm) < 0) success = false;
    if (sf_close(file) != 0) success = false;
    if (!success) {
        fprintf(stderr, "FAIL: playback/read/close was incomplete\n");
        return 1;
    }
    printf("PASS: %ld samples submitted. Confirm you heard the expected sound.\n", total);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "list") == 0) return list_devices();
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "rec") == 0) {
        char* end = NULL;
        long seconds = strtol(argv[3], &end, 10);
        if (*argv[3] == '\0' || *end != '\0' || seconds < 1 || seconds > 60) {
            fprintf(stderr, "Use a duration from 1 to 60 seconds\n");
            return 2;
        }
        const char* name = "plughw:CARD=HiFiAudio,DEV=0";
        if (argc == 5) name = argv[4];
        return record_file(argv[2], (int)(seconds), name);
    }
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "play") == 0) {
        const char* name = "plughw:CARD=Headphones,DEV=0";
        if (argc == 4) name = argv[3];
        return play_file(argv[2], name);
    }
    puts("Usage: AlsaTest list\n       AlsaTest rec NEW.wav SECONDS [PCM]\n       AlsaTest play INPUT.wav [PCM]");
    return 2;
}
