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
void ControlReplyQueue::enqueue(vector<uint8_t> frame) {
    PthreadLock lock(mutex_);
    queue_.push_back(move(frame));
}

bool ControlReplyQueue::try_dequeue(vector<uint8_t>& frame) {
    PthreadLock lock(mutex_);
    if (queue_.empty()) return false;
    frame = move(queue_.front());
    queue_.pop_front();
    return true;
}

void ControlReplyQueue::clear() {
    PthreadLock lock(mutex_);
    queue_.clear();
}

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
    if (worker_started_) return;
    stop_requested_.store(false);
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
    stop_requested_.store(true);
    if (worker_started_) {
        pthread_join(worker_, nullptr);
        worker_started_ = false;
    }
}

bool NetworkManager::connected() const { return connected_.load(); }

void NetworkManager::queue_control(vector<uint8_t> frame) {
    control_replies_.enqueue(move(frame));
}

void NetworkManager::post_network_event(WorkerResultCode code, uint8_t error) {
    MainMessage message;
    message.kind = error == 0 ? MessageKind::WorkerResult
                              : MessageKind::WorkerError;
    message.result_code = code;
    message.error_code = error;
    messages_.enqueue(move(message));
}

void NetworkManager::set_network_thread_policy() {
    // Main and Network are peers: SCHED_OTHER priority 0, nice 0.
    sched_param normal_priority{};
    const int policy_error = pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority);
    if (policy_error != 0) cerr << "Cannot set normal scheduling: " << strerror(policy_error) << '\n';
    if (setpriority(PRIO_PROCESS, 0, 0) != 0) cerr << "Cannot set nice 0: " << strerror(errno) << '\n';

    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(3, &cpus);
    const int result =
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (result != 0) {
        cerr << "Network thread CPU affinity not set: " << strerror(result)
                  << '\n';
    }

}

// A TCP write can be partial: finish this frame before another frame is sent.
bool NetworkManager::send_all(int socket_fd,
                              const vector<uint8_t>& frame) {
    size_t sent = 0;
    const auto deadline =
        chrono::steady_clock::now() + chrono::milliseconds(100);
    while (sent < frame.size() && !stop_requested_.load()) {
        if (chrono::steady_clock::now() >= deadline) return false;
        const auto count = ::send(socket_fd, frame.data() + sent,
                                  frame.size() - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                const auto now = chrono::steady_clock::now();
                if (now >= deadline) return false;
                const auto remaining =
                    chrono::duration_cast<chrono::milliseconds>(
                        deadline - now)
                        .count();
                pollfd writable{socket_fd, POLLOUT, 0};
                const int ready = ::poll(
                    &writable, 1,
                    static_cast<int>(min<int64_t>(remaining, 10)));
                if (ready < 0 && errno != EINTR) return false;
                if (ready > 0 &&
                    (writable.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (count == 0) return false;
        sent += static_cast<size_t>(count);
    }
    return sent == frame.size();
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
        vector<uint8_t> control;
        if (control_replies_.try_dequeue(control)) {
            if (!send_all(socket_fd, control)) break;
        } else if (const auto packet = telemetry_.try_dequeue();
                   packet.has_value()) {
            try {
                if (!sendData(*packet)) {
                    break;
                }
            } catch (const exception& error) {
                cerr << "Cannot serialize telemetry: " << error.what() << '\n';
                post_network_event(
                    WorkerResultCode::NetworkFailed,
                    static_cast<uint8_t>(phono::protocol::Error::ProtocolError));
                break;
            }
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
        if (receiveCommand(command)) messages_.enqueue(command);
    }
}

bool NetworkManager::startListening() {
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
        ::close(server_fd_);
        server_fd_ = -1;
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return false;
    }
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0 ||
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
    const auto count = ::recv(client_fd_, command_bytes_.data() + bytes_received_,
                              command_bytes_.size() - bytes_received_, 0);
    if (count < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) client_alive_ = false;
        return false;
    }
    if (count == 0) { client_alive_ = false; return false; }
    bytes_received_ += static_cast<size_t>(count);
    if (bytes_received_ != command_bytes_.size()) return false;
    bytes_received_ = 0;
    if (!phono::protocol::decode_command(command_bytes_.data(), command_bytes_.size(), command)) {
        client_alive_ = false;
        return false;
    }
    return true;
}

bool NetworkManager::sendData(const Telemetry& packet) {
    return send_all(client_fd_, phono::protocol::make_data_packet(packet.waveform, packet.spectrum));
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

// This pthread is the sole owner of server_fd_ and client_fd_.
void NetworkManager::run() {
    set_network_thread_policy();
    if (!startListening()) return;
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
        connected_.store(true);
        telemetry_.set_connected(true);
        post_network_event(WorkerResultCode::ClientConnected);
        serve_client(client_fd_);
        ::close(client_fd_);
        client_fd_ = -1;
        connected_.store(false);
        telemetry_.set_connected(false);
        control_replies_.clear();
        post_network_event(WorkerResultCode::ClientDisconnected);
    }
    ::close(server_fd_);
    server_fd_ = -1;
}
}  // namespace phono_app
