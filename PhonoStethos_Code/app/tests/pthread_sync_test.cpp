#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"

#include <array>
#include <cassert>
#include <pthread.h>
#include <time.h>

using namespace phono_app;

namespace {
struct Transfer {
    CircularBuffer<int, 31> buffer;
    size_t received = 0;
};

// Force wrap-around and full-buffer retries while the other thread consumes.
void* produce(void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    for (int block = 0; block < 1000; ++block) {
        std::array<int, 15> samples{};
        for (int i = 0; i < 15; ++i) samples[i] = block * 15 + i;
        while (!transfer.buffer.push(samples.data(), samples.size())) {
            const timespec pause{0, 100000};
            nanosleep(&pause, nullptr);
        }
    }
    transfer.buffer.stop_producing();
    return nullptr;
}

void* consume(void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    std::array<int, 7> samples{};
    while (true) {
        const auto count = transfer.buffer.pop(samples.data(), samples.size());
        for (size_t i = 0; i < count; ++i) {
            assert(samples[i] == static_cast<int>(transfer.received));
            ++transfer.received;
        }
        if (transfer.buffer.stopped() && transfer.buffer.isEmpty()) break;
    }
    return nullptr;
}

void* enqueue_results(void* context) {
    auto& queue = *static_cast<CommandQueue*>(context);
    for (uint32_t i = 0; i < 1000; ++i) {
        MainMessage message;
        message.kind = MessageKind::WorkerResult;
        message.operation_id = i;
        assert(queue.enqueue(message));
    }
    return nullptr;
}

void* wait_for_close(void* context) {
    auto& queue = *static_cast<CommandQueue*>(context);
    MainMessage message;
    assert(!queue.dequeue(message, std::chrono::seconds(10)));
    return nullptr;
}
}

int main() {
    Transfer transfer;
    assert(transfer.buffer.begin_session());
    pthread_t producer, consumer;
    assert(pthread_create(&consumer, nullptr, consume, &transfer) == 0);
    assert(pthread_create(&producer, nullptr, produce, &transfer) == 0);
    assert(pthread_join(producer, nullptr) == 0);
    assert(pthread_join(consumer, nullptr) == 0);
    assert(transfer.received == 15000);
    int sample = 42;
    assert(!transfer.buffer.push(&sample, 1)); // STOP forbids later pushes.
    assert(transfer.buffer.begin_session());
    assert(transfer.buffer.isEmpty()); // The next session inherits no samples.

    CommandQueue queue;
    pthread_t first, second;
    assert(pthread_create(&first, nullptr, enqueue_results, &queue) == 0);
    assert(pthread_create(&second, nullptr, enqueue_results, &queue) == 0);
    std::array<unsigned, 1000> seen{};
    for (unsigned i = 0; i < 2000; ++i) {
        MainMessage message;
        assert(queue.dequeue(message, std::chrono::seconds(5)));
        assert(message.operation_id < seen.size());
        ++seen[message.operation_id];
    }
    pthread_join(first, nullptr);
    pthread_join(second, nullptr);
    for (const auto count : seen) assert(count == 2);
    pthread_t waiter;
    assert(pthread_create(&waiter, nullptr, wait_for_close, &queue) == 0);
    queue.close();
    pthread_join(waiter, nullptr);
}
