#include <stdbool.h>
// Create, sync, reopen and verify one WAV. Existing paths are never overwritten.
// API reference: https://libsndfile.github.io/libsndfile/api.html
#include <sndfile.h>
#include <sys/vfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--local") != 0)) {
        puts("Usage: UsbTest NEW_FILE.wav [--local]");
        puts("Default: require a mounted FAT filesystem. --local skips that hardware check.");
        return 2;
    }
    if (argc == 2) {
        char parent[PATH_MAX] = ".";
        const char* separator = strrchr(argv[1], '/');
        if (separator != NULL) {
            size_t length = (size_t)(separator - argv[1]) + 1;
            if (length >= sizeof(parent)) return 2;
            memcpy(parent, argv[1], length);
            parent[length] = '\0';
        }
        struct statfs filesystem = {0};
        if (statfs(parent, &filesystem) != 0) {
            perror("FAIL: statfs");
            return 1;
        }
        if (filesystem.f_type != 0x4d44) { // Linux MSDOS_SUPER_MAGIC: FAT family.
            fprintf(stderr, "FAIL: %s is not on a FAT filesystem; check the USB mount\n", parent);
            return 1;
        }
        puts("FAT mount detected (this value alone does not distinguish FAT32 from FAT16).");
    }
    if (argc == 3) puts("LOCAL MODE: WAV library check only; USB hardware is not tested.");
    // API: open + O_EXCL creates a new file and refuses to replace an existing one.
    int fd = open(argv[1], O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("FAIL: create WAV");
        return 1;
    }
    SF_INFO info = {0};
    info.samplerate = 8000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    /* API: sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE)
     * libsndfile writes a valid WAV header. SF_FALSE keeps our POSIX descriptor
     * open after sf_close so we can explicitly check fsync after finalization.
     */
    SNDFILE* file = sf_open_fd(fd, SFM_WRITE, &info, SF_FALSE);
    if (file == NULL) {
        fprintf(stderr, "FAIL: sf_open_fd: %s\n", sf_strerror(NULL));
        close(fd);
        return 1;
    }
    short samples[8000];
    for (int i = 0; i < 8000; i++) {
        samples[i] = (short)(4096 * sin(2 * 3.14159265358979323846 * 500 * i / 8000));
    }
    // API: sf_write_short returns the number of samples actually written.
    bool success = sf_write_short(file, samples, 8000) == 8000;
    sf_write_sync(file); // Force writes and then check the library's error state.
    if (sf_error(file) != SF_ERR_NO_ERROR) success = false;
    if (sf_close(file) != 0) success = false; // Also finalize the WAV header.
    if (fsync(fd) != 0) success = false; // Include the finalized header in the sync.
    if (close(fd) != 0) success = false;
    if (!success) {
        fprintf(stderr, "FAIL: writing/syncing/closing WAV\n");
        return 1;
    }
    info = (SF_INFO){0};
    file = sf_open(argv[1], SFM_READ, &info);
    if (file == NULL) {
        fprintf(stderr, "FAIL: reopen WAV: %s\n", sf_strerror(NULL));
        return 1;
    }
    short readback[8000];
    if (info.frames != 8000 || info.samplerate != 8000 || info.channels != 1 ||
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) success = false;
    // API: sf_read_short reads PCM samples; compare them with the originals.
    if (sf_read_short(file, readback, 8000) != 8000) success = false;
    if (success) {
        for (int i = 0; i < 8000; i++) {
            if (readback[i] != samples[i]) success = false;
        }
    }
    if (sf_close(file) != 0) success = false;
    if (!success) {
        fprintf(stderr, "FAIL: WAV metadata, samples or read-close result differ\n");
        return 1;
    }
    printf("PASS: 8000 samples written/read exactly; 8 kHz mono PCM16, 1 second\nKept test file: %s\n", argv[1]);
    return 0;
}
