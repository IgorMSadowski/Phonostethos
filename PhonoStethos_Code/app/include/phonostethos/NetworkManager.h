/**
 * NetworkManager.h - implements th_Network and its protected outgoing ControlReplyQueue.
 *
 * CONNECTIONS: NetworkManager.h declares both classes; NetworkManager.cpp implements them. Only Network performs TCP I/O. It sends Main commands through CommandQueue and takes control replies before the latest DataQueue snapshot.
 *
 * FUNCTIONS:
 * - ControlReplyQueue(): Control Reply Queue.
 * - enqueue(): store an item using this channel's locking and delivery policy.
 * - dequeue(): take an available item using this channel's receive policy.
 * - clear(): discard waiting channel data and reset its state under its owner's rules.
 * - NetworkManager(): connect the owned resources/references used by this class.
 * - startListening(): create, bind and listen on the Raspberry Pi TCP server socket.
 * - receiveCommand(): retain partial TCP input until all five command bytes can be decoded.
 * - sendData(): serialize a waveform/FFT snapshot and send its complete frame.
 * - sendStatus(): serialize capability flags and enqueue a control reply for Network.
 * - sendFileList(): serialize one archive page and enqueue it for Network.
 * - queue_control(): put a complete serialized reply into the Main-to-Network ControlReplyQueue.
 * - start(): create this worker pthread once and retain its handle for joining.
 * - stop(): request teardown and join workers before destroying their shared resources.
 * - connected(): read whether Network currently has a GUI client.
 * - run(): execute this class's thread loop in the order shown in its flowchart.
 * - serve_client(): alternate one outgoing frame with command reception for one connected GUI.
 * - set_network_thread_policy(): request CPU 3 affinity and SCHED_OTHER priority 0 with nice 0.
 * - send_all(): retry partial TCP sends using an offset and a cumulative deadline.
 * - post_network_event(): send a connection or network error event to Main through CommandQueue.
 * - send_next_frame(): take one control reply first, otherwise take the latest visualization snapshot.
 * - threadEntry(): recover the object pointer passed to pthread_create and call its worker loop.
 */

// API DOCUMENTATION FOR THIS FILE:
// POSIX threads: creation, joining, mutexes and condition variables:
// https://man7.org/linux/man-pages/man7/pthreads.7.html
// pthread mutex lock, try-lock and unlock:
// https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
// Linux scheduling policies and real-time priorities:
// https://man7.org/linux/man-pages/man7/sched.7.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include "phonostethos/CommandQueue.h" // Include the CommandQueue class/types used here; defines MainMessage and the FIFO consumed by th_Main.
#include "phonostethos/DataQueue.h" // Include the DataQueue class/types used here; implements the latest waveform/FFT snapshot channel.
#include "phonostethos/RuntimeTypes.h" // Include the RuntimeTypes class/types used here; defines shared audio constants, runtime options, telemetry and archive identities.

#include <atomic> // Include shared flags and counters that must be read/written atomically.
#include <deque> // Include FIFO containers used for queued messages or replies.
#include "phonostethos/ThreadSupport.h" // Include the ThreadSupport class/types used here; provides the small pthread helpers shared by queues, buffers and managers.

#include <vector> // Include variable-size byte frames and archive entry lists.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.
// ================= QUEUE: Main -> Network ====================
// ControlReplyQueue stores already serialized StatusPacket, State, CommandResult and
// archive frames in a std::deque. Each enqueue adds at the BACK; Network takes
// from the FRONT, preserving FIFO order. The mutex protects that container.
// Network polls it in send_next_frame(); this queue has no condition variable.
// The queue stores bytes; only Network sends those bytes through the TCP socket.
class ControlReplyQueue { // Declare ControlReplyQueue and its related operations/state.
public: // The class operations callable by other code.
    ControlReplyQueue() { initializeMutex(mutex_); } // Call ControlReplyQueue with the arguments shown here.
    ~ControlReplyQueue() { pthread_mutex_destroy(&mutex_); } // Release this initialized mutex after its users have stopped.
    ControlReplyQueue(const ControlReplyQueue&) = delete; // Disallow copying this resource-owning object.
    ControlReplyQueue& operator=(const ControlReplyQueue&) = delete; // Disallow copying this resource-owning object.

    void enqueue(vector<uint8_t> frame); // Main adds at the back under mutex_.
    bool dequeue(vector<uint8_t>& frame); // Network takes the front; false if empty.
    void clear(); // Remove waiting frames when the GUI connection changes.

private: // Internal helpers and state used by this class.
    pthread_mutex_t mutex_{};       // Main and Network must lock before access.
    deque<vector<uint8_t>> queue_; // Actual FIFO storage: one byte vector per frame.
}; // End this block, declaration or argument list.

class NetworkManager { // Declare NetworkManager and its related operations/state.
public: // The class operations callable by other code.
    NetworkManager(const Options& options, CommandQueue& messages, DataQueue& telemetry); // Connect the owned resources/references used by this class.
    ~NetworkManager(); // Complete the call above with these remaining arguments.

    // REPORT 3.4.2: TCP operations followed by queued reply operations.
    bool startListening(); // Create, bind and listen on the Raspberry Pi TCP server socket.
    bool receiveCommand(phono::protocol::CommandPacket& command); // Retain partial TCP input until all five command bytes can be decoded.
    bool sendData(const DataPacket& packet); // Serialize a waveform/FFT snapshot and send its complete frame.
    bool sendStatus(const phono::protocol::StatusPacket& status); // Serialize capability flags and enqueue a control reply for Network.
    bool sendFileList(uint32_t cursor, const vector<phono::protocol::FileEntry>& entries); // Serialize one archive page and enqueue it for Network.

    // Main queues control frames; only th_Network writes the socket.
    void queue_control(vector<uint8_t> frame); // Put a complete serialized reply into the Main-to-Network ControlReplyQueue.
    void start(); // Create this worker pthread once and retain its handle for joining.
    void stop(); // Request teardown and join workers before destroying their shared resources.
    bool connected() const; // Read whether Network currently has a GUI client.

private: // Internal helpers and state used by this class.
    // Worker loop follows the thread flowchart.
    void run(); // Execute this class's thread loop in the order shown in its flowchart.

    const Options& options_; // Declare options for this class instance.
    // SAME SHARED OBJECTS as SystemController::command_queue_ and data_queue_.
    CommandQueue& messages_; // Network sends GUI commands/connection events -> Main.
    DataQueue& telemetry_;   // Network takes the latest AudioDSP visualization.
    // ACTUAL OUTGOING QUEUE OBJECT: Main calls queue_control() to fill it;
    // Network calls send_next_frame() to consume it before ordinary telemetry.
    ControlReplyQueue control_replies_; // Declare control replies for this class instance.
    int server_fd_ = -1; // Initialize server fd to -1.
    int client_fd_ = -1; // Initialize client fd to -1.
    array<uint8_t, phono::protocol::kCommandBytes> command_bytes_{}; // Complete this expression or declaration.
    size_t bytes_received_ = 0; // Initialize bytes received to 0.
    bool client_alive_ = false; // Initialize client alive to false.
    atomic<bool> stop_requested_{false}; // Declare stop requested for this class instance.
    atomic<bool> connected_{false}; // Declare connected for this class instance.
    pthread_t worker_{}; // POSIX pthread handle created by start and later passed to pthread_join.
    bool worker_started_ = false; // Track whether a pthread was actually created and must be joined.
    // ==================== PRIVATE HELPER METHODS ====================
    void serve_client(int socket_fd); // Alternate one outgoing frame with command reception for one connected GUI.
    void set_network_thread_policy(); // Request CPU 3 affinity and SCHED_OTHER priority 0 with nice 0.
    bool send_all(int socket_fd, const vector<uint8_t>& frame); // Retry partial TCP sends using an offset and a cumulative deadline.
    void post_network_event(WorkerResultCode code, uint8_t error = 0); // Send a MainMessage.
    bool send_next_frame(int socket_fd); // Take a reply first, otherwise the latest telemetry.
    static void* threadEntry(void* context); // Recover the object pointer passed to pthread_create and call its worker loop.

}; // End this block, declaration or argument list.
}  // namespace phono_app
