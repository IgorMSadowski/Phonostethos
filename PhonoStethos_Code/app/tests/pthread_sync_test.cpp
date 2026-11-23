/**
 * pthread_sync_test.cpp - checks pthread sync with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - produce(): produce.
 * - consume(): consume.
 * - enqueue_results(): enqueue results.
 * - wait_for_close(): wait for close.
 * - main(): run this standalone test and return success only if its checks pass.
 */

#include "phonostethos/CircularBuffer.h" // Include the CircularBuffer class/types used here; implements the shared PCM16 circular sample buffer.
#include "phonostethos/CommandQueue.h" // Include the CommandQueue class/types used here; defines MainMessage and the FIFO consumed by th_Main.

#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <cassert> // Include assertions that stop a test when an expected result is false.
#include <pthread.h> // Include POSIX thread creation, mutexes and condition-variable APIs.
#include <time.h> // Include timespec, monotonic clocks and timed sleeps.

using namespace phono_app; // Allow the names in this namespace without repeating its prefix.

namespace { // Keep these declarations in the project namespace and avoid global name collisions.
struct Transfer { // Group the related fields in one plain data record.
    CircularBuffer buffer{31}; // Declare buffer for this operation.
    size_t received = 0; // Initialize received to 0.
}; // End this block, declaration or argument list.

// Force wrap-around and full-buffer retries while the other thread consumes.
void* produce(void* context) { // Finish the parameters/condition and enter the block below.
    auto& transfer = *static_cast<Transfer*>(context); // Initialize transfer to *static_cast<Transfer*>(context).
    for (int block = 0; block < 1000; ++block) { // Visit the specified items/indexes in order.
        std::array<int16_t, 15> samples{}; // Complete this expression or declaration.
        for (int i = 0; i < 15; ++i) samples[i] = block * 15 + i; // Visit the specified items/indexes in order.
        while (!transfer.buffer.push(samples.data(), samples.size())) { // Repeat this block while its condition remains true; recheck shared state on each pass.
            const timespec pause{0, 100000}; // Declare pause for this operation.
            /* API: nanosleep
             * Sleep for the relative timespec interval instead of spinning while idle. A signal may interrupt the sleep before the whole interval has elapsed.
             */
            nanosleep(&pause, nullptr); // Call nanosleep with the arguments shown here.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    transfer.buffer.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    return nullptr; // Return nullptr to the caller.
} // End this block, declaration or argument list.

void* consume(void* context) { // Finish the parameters/condition and enter the block below.
    auto& transfer = *static_cast<Transfer*>(context); // Initialize transfer to *static_cast<Transfer*>(context).
    std::array<int16_t, 7> samples{}; // Complete this expression or declaration.
    while (true) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        const auto count = transfer.buffer.pop(samples.data(), samples.size()); // Initialize count to transfer.buffer.pop(samples.data(), samples.size()).
        for (size_t i = 0; i < count; ++i) { // Visit the specified items/indexes in order.
            assert(samples[i] == static_cast<int>(transfer.received)); // Verify this expected result; fail the test if it is not satisfied.
            ++transfer.received; // Advance this index/counter for the next item.
        } // End this block, declaration or argument list.
        if (transfer.buffer.stopped() && transfer.buffer.isEmpty()) break; // Test the condition transfer.buffer.stopped() && transfer.buffer.isEmpty(). If true, leave the current loop or switch branch.
    } // End this block, declaration or argument list.
    return nullptr; // Return nullptr to the caller.
} // End this block, declaration or argument list.

void* enqueue_results(void* context) { // Finish the parameters/condition and enter the block below.
    auto& queue = *static_cast<CommandQueue*>(context); // Initialize queue to *static_cast<CommandQueue*>(context).
    for (uint32_t i = 0; i < 1000; ++i) { // Visit the specified items/indexes in order.
        MainMessage message; // Declare message for this operation.
        message.kind = MessageKind::WORKER_RESULT; // Set message.kind to MessageKind::WORKER_RESULT.
        message.operation_id = i; // Set message.operation id to i.
        assert(queue.enqueue(message)); // Verify this expected result; fail the test if it is not satisfied.
    } // End this block, declaration or argument list.
    return nullptr; // Return nullptr to the caller.
} // End this block, declaration or argument list.

void* wait_for_close(void* context) { // Finish the parameters/condition and enter the block below.
    auto& queue = *static_cast<CommandQueue*>(context); // Initialize queue to *static_cast<CommandQueue*>(context).
    MainMessage message; // Declare message for this operation.
    assert(!queue.dequeue(message, std::chrono::seconds(10))); // Verify this expected result; fail the test if it is not satisfied.
    return nullptr; // Return nullptr to the caller.
} // End this block, declaration or argument list.
} // End this block, declaration or argument list.

int main() { // Run this standalone test and return success only if its checks pass.
    Transfer transfer; // Declare transfer for this operation.
    assert(transfer.buffer.begin_session()); // Verify this expected result; fail the test if it is not satisfied.
    pthread_t producer, consumer; // Declare producer for this operation.
    assert(pthread_create(&consumer, nullptr, consume, &transfer) == 0); // Verify this expected result; fail the test if it is not satisfied.
    assert(pthread_create(&producer, nullptr, produce, &transfer) == 0); // Verify this expected result; fail the test if it is not satisfied.
    assert(pthread_join(producer, nullptr) == 0); // Verify this expected result; fail the test if it is not satisfied.
    assert(pthread_join(consumer, nullptr) == 0); // Verify this expected result; fail the test if it is not satisfied.
    assert(transfer.received == 15000); // Verify this expected result; fail the test if it is not satisfied.
    int16_t sample = 42; // Initialize sample to 42.
    assert(!transfer.buffer.push(&sample, 1)); // STOP forbids later pushes.
    assert(transfer.buffer.begin_session()); // Verify this expected result; fail the test if it is not satisfied.
    assert(transfer.buffer.isEmpty()); // The next session inherits no samples.

    CommandQueue queue; // Declare queue for this operation.
    pthread_t first, second; // Declare first for this operation.
    assert(pthread_create(&first, nullptr, enqueue_results, &queue) == 0); // Verify this expected result; fail the test if it is not satisfied.
    assert(pthread_create(&second, nullptr, enqueue_results, &queue) == 0); // Verify this expected result; fail the test if it is not satisfied.
    std::array<unsigned, 1000> seen{}; // Complete this expression or declaration.
    for (unsigned i = 0; i < 2000; ++i) { // Visit the specified items/indexes in order.
        MainMessage message; // Declare message for this operation.
        assert(queue.dequeue(message, std::chrono::seconds(5))); // Verify this expected result; fail the test if it is not satisfied.
        assert(message.operation_id < seen.size()); // Verify this expected result; fail the test if it is not satisfied.
        ++seen[message.operation_id]; // Advance this index/counter for the next item.
    } // End this block, declaration or argument list.
    pthread_join(first, nullptr); // Call pthread_join with the arguments shown here.
    pthread_join(second, nullptr); // Call pthread_join with the arguments shown here.
    for (const auto count : seen) assert(count == 2); // Visit the specified items/indexes in order.
    pthread_t waiter; // Declare waiter for this operation.
    assert(pthread_create(&waiter, nullptr, wait_for_close, &queue) == 0); // Verify this expected result; fail the test if it is not satisfied.
    queue.close(); // Call queue.close with the arguments shown here.
    pthread_join(waiter, nullptr); // Call pthread_join with the arguments shown here.
} // End this block, declaration or argument list.
