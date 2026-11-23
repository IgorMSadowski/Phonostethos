// Exercise the actual project's CircularBuffer, CommandQueue and DataQueue.
#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <time.h>

using namespace phono_app;

struct Transfer {
    CircularBuffer buffer;
    CommandQueue results;
    std::atomic<bool> failed{false};
};

static void* produce(void* context) {
    Transfer* transfer = static_cast<Transfer*>(context);
    short block[512];
    for (int number = 0; number < 16; number++) {
        for (int i = 0; i < 512; i++) block[i] = number * 512 + i;
        while (!transfer->buffer.push(block, 512)) {
            if (transfer->failed.load()) return nullptr;
            const timespec pause{0, 1000000};
            nanosleep(&pause, nullptr); // Let the consumer free buffer space.
        }
    }
    transfer->buffer.stop_producing(); // EOF: consumer drains already accepted samples.
    MainMessage result;
    result.kind = MessageKind::WorkerResult;
    result.result_code = WorkerResultCode::RecordingClosed;
    result.operation_id = 42;
    transfer->results.enqueue(result);
    return nullptr;
}

int main() {
    Transfer transfer;
    if (!transfer.buffer.begin_session()) return 1;
    pthread_t worker;
    /* API: pthread_create
     * Start produce() on a second thread and pass it the address of shared data.
     * The buffer's own pthread mutex/condition protect its push/pop operations.
     */
    int error = pthread_create(&worker, nullptr, produce, &transfer);
    if (error != 0) {
        std::fprintf(stderr, "FAIL: pthread_create: %s\n", std::strerror(error));
        return 1;
    }
    size_t received = 0;
    short block[200]; // A different block size forces partial pops and wrap-around.
    while (received < 8192 && !transfer.failed.load()) {
        size_t count = transfer.buffer.pop(block, 200, std::chrono::milliseconds(1000));
        if (count == 0) {
            transfer.failed.store(true);
            break;
        }
        for (size_t i = 0; i < count; i++) {
            if (block[i] != static_cast<short>(received + i)) transfer.failed.store(true);
        }
        received += count;
    }
    // API: pthread_join waits until the worker returns before shared data is destroyed.
    error = pthread_join(worker, nullptr);
    if (error != 0 || transfer.failed.load() || received != 8192) {
        std::fprintf(stderr, "FAIL: transfer lost/reordered samples, timed out or failed to join\n");
        return 1;
    }
    MainMessage result;
    if (!transfer.results.dequeue(result) || result.operation_id != 42 ||
        result.kind != MessageKind::WorkerResult) {
        std::fprintf(stderr, "FAIL: worker result did not reach Main through CommandQueue\n");
        return 1;
    }
    if (!transfer.buffer.isEmpty() || !transfer.buffer.stopped()) return 1;
    short later_sample = 1;
    if (transfer.buffer.push(&later_sample, 1)) return 1; // A closed gate rejects new samples.
    if (!transfer.buffer.begin_session()) return 1;
    std::puts("PASS: 8192 ordered samples, buffer wrap-around, partial pops, EOF and reuse");
    std::puts("PASS: worker result through CommandQueue, pthread creation and join");

    DataQueue telemetry;
    telemetry.set_connected(true);
    Telemetry first, latest, received_packet;
    first.waveform[0] = 0.1F;
    latest.waveform[0] = 0.2F;
    if (!telemetry.try_enqueue(first) || !telemetry.try_enqueue(latest) ||
        !telemetry.try_dequeue(received_packet) || received_packet.waveform[0] != 0.2F ||
        telemetry.try_dequeue(received_packet)) {
        std::fprintf(stderr, "FAIL: DataQueue did not retain exactly the latest snapshot\n");
        return 1;
    }
    std::puts("PASS: DataQueue replaces an old visualization snapshot");
    return 0;
}
