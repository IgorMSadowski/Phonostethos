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
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace std;

namespace phono_app {
void ControlOutputQueue::enqueue(vector<uint8_t> frame) {
    lock_guard<mutex> lock(mutex_);
    queue_.push_back(move(frame));
}

bool ControlOutputQueue::try_dequeue(vector<uint8_t>& frame) {
    lock_guard<mutex> lock(mutex_);
    if (queue_.empty()) return false;
    frame = move(queue_.front());
    queue_.pop_front();
    return true;
}

void ControlOutputQueue::clear() {
    lock_guard<mutex> lock(mutex_);
    queue_.clear();
}

NetworkManager::NetworkManager(const Options& options, CommandQueue& messages,
                               DataQueue& telemetry,
                               ControlOutputQueue& output)
    : options_(options),
      messages_(messages),
      telemetry_(telemetry),
      output_(output) {}

NetworkManager::~NetworkManager() { stop(); }

void NetworkManager::start() {
    if (worker_.joinable()) return;
    stop_requested_.store(false);
    try {
        worker_ = thread(&NetworkManager::run, this);
    } catch (const system_error& error) {
        cerr << "Cannot start network thread: " << error.what() << '\n';
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
    }
}

void NetworkManager::stop() {
    stop_requested_.store(true);
    if (worker_.joinable()) worker_.join();
}

bool NetworkManager::connected() const { return connected_.load(); }

void NetworkManager::queue_control(vector<uint8_t> frame) {
    output_.enqueue(move(frame));
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
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(3, &cpus);
    const int result =
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (result != 0) {
        cerr << "Network thread CPU affinity not set: " << strerror(result)
                  << '\n';
    }
    sched_param priority{};
    const int scheduling_result =
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &priority);
    if (scheduling_result != 0) {
        cerr << "Network thread SCHED_OTHER unavailable: "
                  << strerror(scheduling_result) << '\n';
    }
}

bool NetworkManager::send_all(int socket_fd,
                              const vector<uint8_t>& frame) {
    size_t sent = 0;
    const auto deadline =
        chrono::steady_clock::now() + chrono::milliseconds(100);
    while (sent < frame.size() && !stop_requested_.load()) {
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

    vector<uint8_t> command_bytes;
    command_bytes.reserve(phono::protocol::kCommandBytes);
    bool connected = true;
    while (connected && !stop_requested_.load() && running.load()) {
        vector<uint8_t> control;
        if (output_.try_dequeue(control)) {
            if (!send_all(socket_fd, control)) break;
        } else if (const auto packet = telemetry_.try_dequeue();
                   packet.has_value()) {
            try {
                if (!send_all(socket_fd, phono::protocol::make_data_packet(
                                             packet->waveform,
                                             packet->spectrum))) {
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

        array<uint8_t, 128> incoming{};
        const auto count =
            ::recv(socket_fd, incoming.data(), incoming.size(), 0);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            cerr << "Client receive: " << strerror(errno) << '\n';
            break;
        }
        command_bytes.insert(command_bytes.end(), incoming.begin(),
                             incoming.begin() + count);
        while (command_bytes.size() >= phono::protocol::kCommandBytes) {
            phono::protocol::CommandPacket command;
            if (!phono::protocol::decode_command(
                    command_bytes.data(), phono::protocol::kCommandBytes,
                    command)) {
                cerr << "Closing client after invalid command frame\n";
                connected = false;
                break;
            }
            messages_.enqueue(command);
            command_bytes.erase(
                command_bytes.begin(),
                command_bytes.begin() + phono::protocol::kCommandBytes);
        }
    }
}

void NetworkManager::run() {
    set_network_thread_policy();
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "socket: " << strerror(errno) << '\n';
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return;
    }
    int reuse = 1;
    (void)::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) !=
        1) {
        cerr << "Invalid IPv4 bind address: " << options_.bind_address << '\n';
        ::close(server_fd);
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return;
    }
    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0 ||
        ::listen(server_fd, 1) < 0) {
        cerr << "Cannot listen on " << options_.bind_address << ':'
                  << options_.port << ": " << strerror(errno) << '\n';
        ::close(server_fd);
        post_network_event(
            WorkerResultCode::NetworkFailed,
            static_cast<uint8_t>(phono::protocol::Error::NetworkError));
        return;
    }
    post_network_event(WorkerResultCode::NetworkStarted);
    cout << "PhonoStethos server listening on " << options_.bind_address << ':'
              << options_.port << '\n';

    while (!stop_requested_.load() && running.load()) {
        pollfd listener{server_fd, POLLIN, 0};
        const auto ready = ::poll(&listener, 1, 10);
        if (ready < 0) {
            if (errno == EINTR) continue;
            cerr << "Listener poll: " << strerror(errno) << '\n';
            break;
        }
        if (ready == 0 || !(listener.revents & POLLIN)) continue;
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client_fd =
            ::accept(server_fd, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            cerr << "accept: " << strerror(errno) << '\n';
            continue;
        }
        output_.clear();
        telemetry_.clear();
        connected_.store(true);
        telemetry_.set_connected(true);
        post_network_event(WorkerResultCode::ClientConnected);
        serve_client(client_fd);
        ::close(client_fd);
        connected_.store(false);
        telemetry_.set_connected(false);
        output_.clear();
        post_network_event(WorkerResultCode::ClientDisconnected);
    }
    ::close(server_fd);
}
}  // namespace phono_app
