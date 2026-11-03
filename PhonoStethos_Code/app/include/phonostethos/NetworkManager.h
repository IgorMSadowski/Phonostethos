#pragma once

#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"
#include "phonostethos/RuntimeTypes.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace std;

namespace phono_app {
class ControlOutputQueue {
public:
    void enqueue(vector<uint8_t> frame);
    bool try_dequeue(vector<uint8_t>& frame);
    void clear();

private:
    mutex mutex_;
    deque<vector<uint8_t>> queue_;
};

class NetworkManager {
public:
    NetworkManager(const Options& options, CommandQueue& messages,
                   DataQueue& telemetry, ControlOutputQueue& output);
    ~NetworkManager();

    void start();
    void stop();
    bool connected() const;
    void queue_control(vector<uint8_t> frame);

private:
    void run();
    void serve_client(int socket_fd);
    void set_network_thread_policy();
    bool send_all(int socket_fd, const vector<uint8_t>& frame);
    void post_network_event(WorkerResultCode code, uint8_t error = 0);

    const Options& options_;
    CommandQueue& messages_;
    DataQueue& telemetry_;
    ControlOutputQueue& output_;
    atomic<bool> stop_requested_{false};
    atomic<bool> connected_{false};
    thread worker_;
};
}  // namespace phono_app
