/**
 * PthreadTest.cpp - checks Pthread Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - produce(): produce.
 * - main(): run this standalone test and return success only if its checks pass.
 */

// Exercise the actual project's CircularBuffer, CommandQueue and DataQueue.
#include "phonostethos/CircularBuffer.h" // Include the CircularBuffer class/types used here; implements the shared PCM16 circular sample buffer.
#include "phonostethos/CommandQueue.h" // Include the CommandQueue class/types used here; defines MainMessage and the FIFO consumed by th_Main.
#include "phonostethos/DataQueue.h" // Include the DataQueue class/types used here; implements the latest waveform/FFT snapshot channel.
#include <atomic> // Include shared flags and counters that must be read/written atomically.
#include <cstdio> // Include C-style file/console formatting used by this file.
#include <cstring> // Include byte copying/comparison and readable system-error strings.
#include <pthread.h> // Include POSIX thread creation, mutexes and condition-variable APIs.
#include <time.h> // Include timespec, monotonic clocks and timed sleeps.

using namespace phono_app; // Allow the names in this namespace without repeating its prefix.

struct Transfer { // Group the related fields in one plain data record.
    CircularBuffer buffer; // Declare buffer for this operation.
    CommandQueue results; // Declare results for this operation.
    std::atomic<bool> failed{false}; // Declare failed for this operation.
}; // End this block, declaration or argument list.

static void* produce(void* context) { // Finish the parameters/condition and enter the block below.
    Transfer* transfer = static_cast<Transfer*>(context); // Initialize transfer to static_cast<Transfer*>(context).
    short block[512]; // Declare block for this operation.
    for (int number = 0; number < 16; number++) { // Visit the specified items/indexes in order.
        for (int i = 0; i < 512; i++) block[i] = number * 512 + i; // Visit the specified items/indexes in order.
        while (!transfer->buffer.push(block, 512)) { // Repeat this block while its condition remains true; recheck shared state on each pass.
            if (transfer->failed.load()) return nullptr; // Test the condition transfer->failed.load(). If true, return nullptr to the caller.
            const timespec pause{0, 1000000}; // Declare pause for this operation.
            /* API: nanosleep
             * Sleep for the relative timespec interval instead of spinning while idle. A signal may interrupt the sleep before the whole interval has elapsed.
             */
            nanosleep(&pause, nullptr); // Let the consumer free buffer space.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    transfer->buffer.stop_producing(); // EOF: consumer drains already accepted samples.
    MainMessage result; // Declare result for this operation.
    result.kind = MessageKind::WORKER_RESULT; // Set result.kind to MessageKind::WORKER_RESULT.
    result.result_code = WorkerResultCode::RECORD_CLOSED; // Set result.result code to WorkerResultCode::RECORD_CLOSED.
    result.operation_id = 42; // Set result.operation id to 42.
    transfer->results.enqueue(result); // Send this record through the named protected queue; the receiver handles it later.
    return nullptr; // Return nullptr to the caller.
} // End this block, declaration or argument list.

int main() { // Run this standalone test and return success only if its checks pass.
    Transfer transfer; // Declare transfer for this operation.
    if (!transfer.buffer.begin_session()) return 1; // Test the condition !transfer.buffer.begin_session(). If true, return 1 to the caller.
    pthread_t worker; // Declare worker for this operation.
    /* API: pthread_create
     * Start produce() on a second thread and pass it the address of shared data.
     * The buffer's own pthread mutex/condition protect its push/pop operations.
     */
    int error = pthread_create(&worker, nullptr, produce, &transfer); // Initialize error to pthread_create(&worker, nullptr, produce, &transfer).
    if (error != 0) { // Test the condition error != 0.
        std::fprintf(stderr, "FAIL: pthread_create: %s\n", std::strerror(error)); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    size_t received = 0; // Initialize received to 0.
    short block[200]; // A different block size forces partial pops and wrap-around.
    while (received < 8192 && !transfer.failed.load()) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        size_t count = transfer.buffer.pop(block, 200, std::chrono::milliseconds(1000)); // Initialize count to transfer.buffer.pop(block, 200, std::chrono::milliseconds(1000)).
        if (count == 0) { // Test the condition count == 0.
            transfer.failed.store(true); // Publish the new shared transfer.failed value atomically.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        for (size_t i = 0; i < count; i++) { // Visit the specified items/indexes in order.
            if (block[i] != static_cast<short>(received + i)) transfer.failed.store(true); // Test the condition block[i] != static_cast<short>(received + i). If true, publish the new shared transfer.failed value atomically.
        } // End this block, declaration or argument list.
        received += count; // Update received using += count.
    } // End this block, declaration or argument list.
    // API: pthread_join waits until the worker returns before shared data is destroyed.
    error = pthread_join(worker, nullptr); // Set error to pthread_join(worker, nullptr).
    if (error != 0 || transfer.failed.load() || received != 8192) { // Test the condition error != 0 || transfer.failed.load() || received != 8192.
        std::fprintf(stderr, "FAIL: transfer lost/reordered samples, timed out or failed to join\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    MainMessage result; // Declare result for this operation.
    if (!transfer.results.dequeue(result) || result.operation_id != 42 || // Test the condition !transfer.results.dequeue(result) || result.operation_id != 42 ||.
        result.kind != MessageKind::WORKER_RESULT) { // Finish the parameters/condition and enter the block below.
        std::fprintf(stderr, "FAIL: worker result did not reach Main through CommandQueue\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    if (!transfer.buffer.isEmpty() || !transfer.buffer.stopped()) return 1; // Test the condition !transfer.buffer.isEmpty() || !transfer.buffer.stopped(). If true, return 1 to the caller.
    short later_sample = 1; // Initialize later sample to 1.
    if (transfer.buffer.push(&later_sample, 1)) return 1; // A closed gate rejects new samples.
    if (!transfer.buffer.begin_session()) return 1; // Test the condition !transfer.buffer.begin_session(). If true, return 1 to the caller.
    std::puts("PASS: 8192 ordered samples, buffer wrap-around, partial pops, EOF and reuse"); // Complete the call above with these remaining arguments.
    std::puts("PASS: worker result through CommandQueue, pthread creation and join"); // Complete the call above with these remaining arguments.

    DataQueue telemetry; // Declare telemetry for this operation.
    telemetry.set_connected(true); // Call telemetry.set_connected with the arguments shown here.
    DataPacket first, latest, received_packet; // Declare first for this operation.
    first.waveform[0] = 0.1F; // Set first.waveform[0] to 0.1F.
    latest.waveform[0] = 0.2F; // Set latest.waveform[0] to 0.2F.
    if (!telemetry.enqueue(first) || !telemetry.enqueue(latest) || // Test the condition !telemetry.enqueue(first) || !telemetry.enqueue(latest) ||.
        !telemetry.dequeue(received_packet) || received_packet.waveform[0] != 0.2F || // Take an available record through the named queue receive operation.
        telemetry.dequeue(received_packet)) { // Take an available record through the named queue receive operation.
        std::fprintf(stderr, "FAIL: DataQueue did not retain exactly the latest snapshot\n"); // Print the diagnostic or test result for the operator.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    std::puts("PASS: DataQueue replaces an old visualization snapshot"); // Complete the call above with these remaining arguments.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
