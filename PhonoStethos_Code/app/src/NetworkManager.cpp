#include "phonostethos/NetworkManager.h"

#include "phonostethos/protocol.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace std;

namespace phono_app {
// ==================================================================
// WORKER LIFETIME AND SCHEDULING
// ==================================================================

NetworkManager::NetworkManager(const Options& options, CommandQueue& messages,
                               DataQueue& telemetry)
    : options_(options),
      messages_(messages),
      telemetry_(telemetry) {}

NetworkManager::~NetworkManager() { stop(); }

// pthread_create expects a C-style callback; recover this NetworkManager from void*.
void* NetworkManager::threadEntry(void* context) {
    static_cast<NetworkManager*>(context)->run();
    return nullptr;
}

void NetworkManager::start() {
    if (worker_started_) {
        return;
    }
    stop_requested_.store(false); // Update the shared stop requested value atomically.
    /* API: pthread_create
     * Start a POSIX worker. Pass its pthread_t address, attributes (NULL for defaults), entry callback and context pointer. Returns 0 or an error number.
     */
    const int result = pthread_create(&worker_, nullptr, &NetworkManager::threadEntry, this);
    if (result != 0) {
        cerr << "Cannot start Network thread: " << strerror(result) << '\n';
        post_network_event(WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return;
    }
    worker_started_ = true;
}

void NetworkManager::stop() {
    stop_requested_.store(true); // Update the shared stop requested value atomically.
    if (worker_started_) {
        /* API: pthread_join
         * Wait for this worker to return before releasing the objects it uses. Pass the pthread handle and NULL when its return value is unused.
         */
        pthread_join(worker_, nullptr);
        worker_started_ = false;
    }
}

bool NetworkManager::connected() const { return connected_.load(); }

void NetworkManager::set_network_thread_policy() {
    // Main and Network are peers: SCHED_OTHER priority 0, nice 0.
    sched_param normal_priority{};
    /* API: pthread_setschedparam
     * Request the scheduling policy and sched_priority stored in sched_param. FIFO 80 is AudioDSP and FIFO 50 is FileIO. SCHED_OTHER requires priority 0.
     */
    const int policy_error = pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority);
    if (policy_error != 0) cerr << "Cannot set normal scheduling: " << strerror(policy_error) << '\n';
    if (setpriority(PRIO_PROCESS, 0, 0) != 0) cerr << "Cannot set nice 0: " << strerror(errno) << '\n';

    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(3, &cpus);
    const int result =
        /* API: pthread_setaffinity_np
         * Ask Linux to run this pthread on the CPUs in cpu_set_t. pthread_self identifies the current worker. Check the returned error number.
         */
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (result != 0) {
        cerr << "Network thread CPU affinity not set: " << strerror(result)
                  << '\n';
    }

}

// ==================================================================
// CONTROLREPLYQUEUE: STORAGE, MAIN SEND AND NETWORK TAKE
// ==================================================================

void ControlReplyQueue::enqueue(vector<uint8_t> frame) {
    PthreadLock lock(mutex_); // Lock the reply FIFO against simultaneous Network reads.
    // QUEUE SEND (Main): add this serialized reply at the BACK of the FIFO.
    queue_.push_back(move(frame)); // Transfer this frame to the newest end; unlock on return.
}

bool ControlReplyQueue::try_dequeue(vector<uint8_t>& frame) {
    PthreadLock lock(mutex_); // Protect the empty check, front item and removal together.
    if (queue_.empty()) {
        return false; // No waiting reply; Network may check DataQueue instead.
    }
    // QUEUE TAKE (Network): take the oldest reply from the FRONT.
    // try_dequeue returns false when empty; its mutex uses an ordinary short lock.
    frame = move(queue_.front()); // Network now owns the oldest frame's byte vector.
    queue_.pop_front(); // Remove that one queue entry so it cannot be sent twice.
    return true; // Release the mutex before Network performs the TCP send.
}

void ControlReplyQueue::clear() {
    PthreadLock lock(mutex_); // Clear atomically relative to senders/receivers.
    queue_.clear(); // Drop waiting replies during a GUI connection change.
}

// CONTROL: Main -> ControlReplyQueue -> Network TCP send. This preserves reply order.
void NetworkManager::queue_control(vector<uint8_t> frame) {
    control_replies_.enqueue(move(frame));
}

// Main may call this method: queue the frame rather than writing from Main.
bool NetworkManager::sendStatus(const phono::protocol::Status& status) {
    queue_control(phono::protocol::make_status_packet(status));
    return true;
}

bool NetworkManager::sendFileList(uint32_t cursor,
                                 const vector<phono::protocol::FileEntry>& entries) {
    queue_control(phono::protocol::make_file_list_packet(cursor, entries));
    return true;
}

// ==================================================================
// COMMANDQUEUE: NETWORK REPORTS CONNECTION EVENTS TO MAIN
// ==================================================================

void NetworkManager::post_network_event(WorkerResultCode code, uint8_t error) {
    MainMessage message;
    message.kind = MessageKind::WorkerResult;
    if (error != 0) {
        message.kind = MessageKind::WorkerError;
    }
    message.result_code = code;
    message.error_code = error;
    // QUEUE SEND: connection/startup events also go to Main's CommandQueue.
    messages_.enqueue(move(message));
}

// ==================================================================
// OUTGOING FRAMES: CONTROLREPLYQUEUE FIRST, THEN DATAQUEUE
// ==================================================================

// FLOWCHART: select ControlReplyQueue first, otherwise the latest DataQueue frame.
// Complete this frame before receiving the next command or selecting another frame.
bool NetworkManager::send_next_frame(int socket_fd) {
    vector<uint8_t> control;
    // QUEUE RECEIVE: take a reliable control reply before ordinary telemetry.
    if (control_replies_.try_dequeue(control)) {
        return send_all(socket_fd, control);
    } else {
        Telemetry packet;
        // telemetry_ aliases SystemController::data_queue_: one latest snapshot.
        if (telemetry_.try_dequeue(packet)) {
            try {
                return sendData(packet);
            } catch (const exception& error) {
                cerr << "Cannot serialize telemetry: " << error.what() << '\n';
                post_network_event(
                    WorkerResultCode::NetworkFailed,
                    static_cast<uint8_t>(phono::protocol::Error::ProtocolError));
                return false;
            }
        }
    }
    return true; // Nothing waiting to send is also a normal iteration.
}

bool NetworkManager::sendData(const Telemetry& packet) {
    return send_all(client_fd_, phono::protocol::make_data_packet(packet.waveform, packet.spectrum));
}

// A TCP write can be partial: finish this frame before another frame is sent.
bool NetworkManager::send_all(int socket_fd,
                              const vector<uint8_t>& frame) {
    size_t bytes_sent = 0; // Retained across retries: never resend accepted bytes.
    const auto deadline =
        chrono::steady_clock::now() + chrono::milliseconds(100);
    while (bytes_sent < frame.size() && !stop_requested_.load()) {
        if (chrono::steady_clock::now() >= deadline) {
            return false;
        }
        /* API: ::send
         * Submit remaining frame bytes to TCP. A short positive result advances the offset. MSG_NOSIGNAL prevents a closed peer from raising SIGPIPE.
         */
        const auto count = ::send(socket_fd, frame.data() + bytes_sent,
                                  frame.size() - bytes_sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                const auto now = chrono::steady_clock::now();
                if (now >= deadline) {
                    return false;
                }
                const auto remaining =
                    chrono::duration_cast<chrono::milliseconds>(
                        deadline - now)
                        .count();
                pollfd writable{socket_fd, POLLOUT, 0};
                /* API: ::poll
                 * Wait for descriptor readiness for a bounded number of milliseconds. Positive means events, zero means timeout, -1 means error.
                 */
                const int ready = ::poll(
                    &writable, 1,
                    static_cast<int>(min<int64_t>(remaining, 10)));
                if (ready < 0 && errno != EINTR) {
                    return false;
                }
                if (ready > 0 &&
                    (writable.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        bytes_sent += static_cast<size_t>(count);
    }
    return bytes_sent == frame.size();
}

// ==================================================================
// TCP LISTENER AND INCOMING COMMANDS
// ==================================================================

bool NetworkManager::startListening() {
    /* API: ::socket
     * Create a TCP socket. AF_INET means IPv4, SOCK_STREAM means a reliable byte stream. Returns a descriptor or -1 with errno.
     */
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        cerr << "socket: " << strerror(errno) << '\n';
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) !=
        1) {
        cerr << "Invalid IPv4 bind address: " << options_.bind_address << '\n';
        /* API: ::close
         * Release a descriptor after its owner has finished using it. Set the stored descriptor to -1 so it cannot be reused accidentally.
         */
        ::close(server_fd_);
        server_fd_ = -1;
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return false;
    }
    /* API: ::bind
     * Assign a local IP address and port to the listening socket. The IP must exist on the Pi. Returns 0 or -1 with errno.
     */
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0 ||
        /* API: ::listen
         * Mark the socket as a listener. The backlog limits waiting connections. Accept creates a separate client descriptor.
         */
        ::listen(server_fd_, 1) < 0) {
        cerr << "Cannot listen on " << options_.bind_address << ':'
                  << options_.port << ": " << strerror(errno) << '\n';
        ::close(server_fd_);
        server_fd_ = -1;
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return false;
    }
    return true;
}

// TCP has no message boundaries. Keep partial command bytes until all five arrive.
bool NetworkManager::receiveCommand(phono::protocol::CommandPacket& command) {
    /* API: ::recv
     * Receive TCP bytes into the remaining command-buffer space. Positive is the number received, zero means peer closure, -1 requires checking errno. TCP may split a five-byte command.
     */
    const auto count = ::recv(client_fd_, command_bytes_.data() + bytes_received_,
                              command_bytes_.size() - bytes_received_, 0);
    if (count < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) client_alive_ = false;
        return false;
    }
    if (count == 0) { client_alive_ = false; return false; }
    bytes_received_ += static_cast<size_t>(count);
    if (bytes_received_ != command_bytes_.size()) {
        return false;
    }
    bytes_received_ = 0;
    if (!phono::protocol::decode_command(command_bytes_.data(), command_bytes_.size(), command)) {
        client_alive_ = false;
        return false;
    }
    return true;
}

// One GUI client: send one control/telemetry frame, then check for incoming commands.
void NetworkManager::serve_client(int socket_fd) {
    timeval timeout{};
    timeout.tv_usec = 10000;
    if (::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) != 0 ||
        ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) != 0) {
        cerr << "Cannot configure client socket timeouts: "
                  << strerror(errno) << '\n';
        return;
    }

    bytes_received_ = 0;
    client_alive_ = true;
    while (client_alive_ && !stop_requested_.load() && running.load()) {
        if (!send_next_frame(socket_fd)) {
            break;
        }

        pollfd event{socket_fd, POLLIN, 0};
        const auto ready = ::poll(&event, 1, 10);
        if (ready < 0) {
            if (errno == EINTR) continue;
            cerr << "Client poll: " << strerror(errno) << '\n';
            break;
        }
        if (ready == 0) continue;
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(event.revents & POLLIN)) continue;

        phono::protocol::CommandPacket command;
        // QUEUE SEND: received TCP bytes have become a complete CommandPacket.
        // enqueue wraps it as MainMessage::GuiCommand and signals Main's queue.
        if (receiveCommand(command)) messages_.enqueue(command);
    }
}

// ==================================================================
// WORKER LOOP: ACCEPT, SERVE AND CLEAN UP EACH CLIENT
// ==================================================================

// This pthread is the sole owner of server_fd_ and client_fd_.
// FLOWCHART: th_Network part 1 -> listen/accept -> serve_client (parts 2-3).
void NetworkManager::run() {
    set_network_thread_policy();
    if (!startListening()) {
        return;
    }
    post_network_event(WorkerResultCode::NetworkStarted);
    cout << "PhonoStethos server listening on " << options_.bind_address << ':'
              << options_.port << '\n';

    while (!stop_requested_.load() && running.load()) {
        pollfd listener{server_fd_, POLLIN, 0};
        const auto ready = ::poll(&listener, 1, 10);
        if (ready < 0) {
            if (errno == EINTR) continue;
            cerr << "Listener poll: " << strerror(errno) << '\n';
            break;
        }
        if (ready == 0 || !(listener.revents & POLLIN)) continue;
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        client_fd_ =
            /* API: ::accept
             * Accept a waiting GUI connection. Returns a new descriptor. Keep the listening descriptor for later reconnects.
             */
            ::accept(server_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client_fd_ < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            cerr << "accept: " << strerror(errno) << '\n';
            continue;
        }
        control_replies_.clear();
        telemetry_.clear();
        connected_.store(true); // Update the shared connected value atomically.
        telemetry_.set_connected(true);
        post_network_event(WorkerResultCode::ClientConnected);
        serve_client(client_fd_);
        ::close(client_fd_);
        client_fd_ = -1;
        connected_.store(false); // Update the shared connected value atomically.
        telemetry_.set_connected(false);
        control_replies_.clear();
        post_network_event(WorkerResultCode::ClientDisconnected);
    }
    ::close(server_fd_);
    server_fd_ = -1;
}
}  // namespace phono_app
