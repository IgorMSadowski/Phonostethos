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
// ================= QUEUE: Main -> Network ====================
// ControlReplyQueue stores already serialized Status, State, CommandResult and
// archive frames in a std::deque. Each enqueue adds at the BACK; Network takes
// from the FRONT, preserving FIFO order. The mutex protects that container.
// Network polls it in send_next_frame(); this queue has no condition variable.
// The queue stores bytes; only Network sends those bytes through the TCP socket.
class ControlReplyQueue {
public:
    ControlReplyQueue() { initializeMutex(mutex_); }
    ~ControlReplyQueue() { pthread_mutex_destroy(&mutex_); }
    ControlReplyQueue(const ControlReplyQueue&) = delete;
    ControlReplyQueue& operator=(const ControlReplyQueue&) = delete;

    void enqueue(vector<uint8_t> frame); // Main adds at the back under mutex_.
    bool try_dequeue(vector<uint8_t>& frame); // Network takes the front; false if empty.
    void clear(); // Remove waiting frames when the GUI connection changes.

private:
    pthread_mutex_t mutex_{};       // Main and Network must lock before access.
    deque<vector<uint8_t>> queue_; // Actual FIFO storage: one byte vector per frame.
};

class NetworkManager {
public:
    NetworkManager(const Options& options, CommandQueue& messages,
                   DataQueue& telemetry);
    ~NetworkManager();

    void start();
    void stop();
    bool connected() const;
    // CONTROLREPLYQUEUE SEND API: these methods are called by Main.
    // They store complete frames; Network later sends them through its socket.
    bool sendStatus(const phono::protocol::Status& status);
    bool sendFileList(uint32_t cursor, const vector<phono::protocol::FileEntry>& entries);
    void queue_control(vector<uint8_t> frame);

    // SOCKET API: these methods run inside th_Network.
    bool startListening();
    bool receiveCommand(phono::protocol::CommandPacket& command);
    bool sendData(const Telemetry& packet);

private:
    void run();
    void serve_client(int socket_fd);
    void set_network_thread_policy();
    bool send_all(int socket_fd, const vector<uint8_t>& frame);

    const Options& options_;
    // SAME SHARED OBJECTS as SystemController::command_queue_ and data_queue_.
    void post_network_event(WorkerResultCode code, uint8_t error = 0); // Send a MainMessage.
    CommandQueue& messages_; // Network sends GUI commands/connection events -> Main.
    DataQueue& telemetry_;   // Network takes the latest AudioDSP visualization.
    // ACTUAL OUTGOING QUEUE OBJECT: Main calls queue_control() to fill it;
    // Network calls send_next_frame() to consume it before ordinary telemetry.
    bool send_next_frame(int socket_fd); // Take a reply first, otherwise the latest telemetry.
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
