#pragma once

#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"
#include "phonostethos/RuntimeTypes.h"

#include <atomic>
#include <deque>
#include "phonostethos/ThreadSupport.h"

#include <vector>

using namespace std;

namespace phono_app {
// Reliable outgoing control FIFO, owned by NetworkManager; separate from lossy telemetry.
class ControlReplyQueue {
public:
    ControlReplyQueue() { initializeMutex(mutex_); }
    ~ControlReplyQueue() { pthread_mutex_destroy(&mutex_); }
    ControlReplyQueue(const ControlReplyQueue&) = delete;
    ControlReplyQueue& operator=(const ControlReplyQueue&) = delete;

    void enqueue(vector<uint8_t> frame);
    bool try_dequeue(vector<uint8_t>& frame);
    void clear();

private:
    pthread_mutex_t mutex_{};
    deque<vector<uint8_t>> queue_;
};

class NetworkManager {
public:
    NetworkManager(const Options& options, CommandQueue& messages,
                   DataQueue& telemetry);
    ~NetworkManager();

    void start();
    void stop();
    bool connected() const;
    bool startListening();
    bool receiveCommand(phono::protocol::CommandPacket& command);
    bool sendData(const Telemetry& packet);
    // Control methods queue frames; th_Network performs the actual socket writes.
    bool sendStatus(const phono::protocol::Status& status);
    bool sendFileList(uint32_t cursor, const vector<phono::protocol::FileEntry>& entries);
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
    ControlReplyQueue control_replies_;
    int server_fd_ = -1;
    int client_fd_ = -1;
    array<uint8_t, phono::protocol::kCommandBytes> command_bytes_{};
    size_t bytes_received_ = 0;
    bool client_alive_ = false;
    atomic<bool> stop_requested_{false};
    atomic<bool> connected_{false};
    static void* threadEntry(void* context);
    pthread_t worker_{};
    bool worker_started_ = false;
};
}  // namespace phono_app
