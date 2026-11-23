// Exercise the real FileIO worker, its mailbox and both real circular buffers.
#include "phonostethos/FileIoManager.h"
#include <sndfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace phono_app;

static bool wait_result(CommandQueue& queue, WorkerResultCode code, uint32_t operation) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        MainMessage message;
        if (!queue.dequeue(message) || message.result_code != code || message.operation_id != operation) continue;
        if (message.error_code != 0) {
            std::fprintf(stderr, "FAIL: worker returned error code %u\n", message.error_code);
            return false;
        }
        return true;
    }
    std::fprintf(stderr, "FAIL: worker did not report its result within five seconds\n");
    return false;
}

int main() {
    char directory[] = "/tmp/phono-fileio-test-XXXXXX";
    if (mkdtemp(directory) == nullptr) { std::perror("mkdtemp"); return 1; }
    Options options;
    options.storage = directory;
    options.require_mounted_storage = false; // This system test uses an isolated temporary directory.
    std::string path = std::string(directory) + "/GENERAL_20261123_120000_000001.wav";
    std::printf("Temporary test directory: %s\n", directory);
    CommandQueue messages;
    CircularBuffer recording_buffer;
    CircularBuffer playback_buffer;
    FileIoManager worker(options, messages, recording_buffer, playback_buffer);
    if (!worker.start() || !wait_result(messages, WorkerResultCode::FileIOReady, 0)) return 1;

    // Main -> FileIO mailbox: ask the worker to open a recording file.
    FileIoManager::Request request;
    request.kind = FileIoManager::RequestKind::OpenRecording;
    request.operation_id = 1;
    request.path = path;
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::RecordingOpened, 1)) return 1;
    short original[1041]; // Two full 512-sample blocks plus a final 17-sample tail.
    for (int i = 0; i < 1041; i++) original[i] = static_cast<short>(i - 500);
    if (!recording_buffer.begin_session() || !recording_buffer.push(original, 1041)) return 1;
    recording_buffer.stop_producing(); // No samples can enter after STOP.
    request.kind = FileIoManager::RequestKind::CloseRecording;
    request.operation_id = 2;
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::RecordingClosed, 2)) return 1;

    // Read the saved file independently to check the worker's complete output.
    SF_INFO info{};
    SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
    if (file == nullptr) return 1;
    short saved[1041];
    bool exact = info.frames == 1041 && info.samplerate == 8000 && info.channels == 1 &&
                 sf_read_short(file, saved, 1041) == 1041;
    if (sf_close(file) != 0) exact = false;
    if (exact) {
        for (int i = 0; i < 1041; i++) {
            if (saved[i] != original[i]) exact = false;
        }
    }
    if (!exact || !recording_buffer.isEmpty()) {
        std::fprintf(stderr, "FAIL: recording file or final partial block differed\n");
        return 1;
    }
    std::puts("PASS: recording mailbox -> buffer pop -> WAV write -> STOP drain/close, all 1041 samples saved");

    struct stat metadata{};
    if (lstat(path.c_str(), &metadata) != 0) return 1;
    request.kind = FileIoManager::RequestKind::PreparePlayback;
    request.operation_id = 3;
    request.identity.basename = "GENERAL_20261123_120000_000001.wav";
    request.identity.device = metadata.st_dev;
    request.identity.inode = metadata.st_ino;
    request.identity.size = metadata.st_size;
    request.identity.modified = metadata.st_mtim;
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::PlaybackReady, 3)) return 1;
    // FileIO pushes playback; this test consumes as AudioDSP would.
    std::vector<short> played;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        short block[512];
        size_t count = playback_buffer.pop(block, 512);
        for (size_t i = 0; i < count; i++) played.push_back(block[i]);
        if (playback_buffer.stopped() && playback_buffer.isEmpty()) break;
    }
    if (played.size() != 1041) return 1;
    for (int i = 0; i < 1041; i++) {
        if (played[i] != original[i]) return 1;
    }
    request.kind = FileIoManager::RequestKind::StopPlayback;
    request.operation_id = 4;
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::PlaybackClosed, 4)) return 1;
    std::puts("PASS: playback mailbox -> WAV read -> playback buffer push -> exact samples and EOF");
    request.kind = FileIoManager::RequestKind::Delete;
    request.operation_id = 5;
    if (!worker.request(request) || !wait_result(messages, WorkerResultCode::RecordingDeleted, 5)) return 1;
    worker.stop();
    if (access(path.c_str(), F_OK) == 0 || rmdir(directory) != 0) return 1;
    std::puts("PASS: validated delete, worker stop and temporary-file cleanup");
    return 0;
}
