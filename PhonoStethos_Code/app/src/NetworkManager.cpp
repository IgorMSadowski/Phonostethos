/**
 * NetworkManager.cpp - implements th_Network and its protected outgoing ControlReplyQueue.
 *
 * CONNECTIONS: NetworkManager.h declares both classes; NetworkManager.cpp implements them. Only Network performs TCP I/O. It sends Main commands through CommandQueue and takes control replies before the latest DataQueue snapshot.
 *
 * FUNCTIONS:
 * Main methods are listed first; private helpers are implemented at the end.
 * - NetworkManager(): connect the owned resources/references used by this class.
 * - ~NetworkManager(): release this object's resources after its users have stopped.
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
 * - threadEntry(): recover the object pointer passed to pthread_create and call its worker loop.
 * - set_network_thread_policy(): request CPU 3 affinity and SCHED_OTHER priority 0 with nice 0.
 * - post_network_event(): send a connection or network error event to Main through CommandQueue.
 * - send_next_frame(): take one control reply first, otherwise take the latest visualization snapshot.
 * - send_all(): retry partial TCP sends using an offset and a cumulative deadline.
 * - serve_client(): alternate one outgoing frame with command reception for one connected GUI.
 */

// API DOCUMENTATION FOR THIS FILE:
// POSIX threads: creation, joining, mutexes and condition variables:
// https://man7.org/linux/man-pages/man7/pthreads.7.html
// pthread mutex lock, try-lock and unlock:
// https://man7.org/linux/man-pages/man3/pthread_mutex_lock.3p.html
// Linux pthread CPU affinity:
// https://man7.org/linux/man-pages/man3/pthread_setaffinity_np.3.html
// Linux scheduling policies and real-time priorities:
// https://man7.org/linux/man-pages/man7/sched.7.html
// POSIX TCP socket API; follow the related bind/listen/accept/send/recv pages:
// https://man7.org/linux/man-pages/man2/socket.2.html
// POSIX descriptor open/flags; related read/write/close system calls:
// https://man7.org/linux/man-pages/man2/open.2.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


#include "phonostethos/NetworkManager.h" // Include the NetworkManager class/types used here; implements th_Network and its protected outgoing ControlReplyQueue.

#include "phonostethos/protocol.h" // Include the protocol class/types used here; defines and validates the client/server binary protocol.

#include <algorithm> // Include min/max, copying, sorting and other standard algorithms used in this file.
#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <arpa/inet.h> // Include IPv4 text/binary conversion and network byte order.
#include <cerrno> // Include errno and named system-call error codes.
#include <chrono> // Include typed durations and monotonic deadlines.
#include <cstring> // Include byte copying/comparison and readable system-error strings.
#include <iostream> // Include console diagnostics and test results.
#include <netinet/in.h> // Include IPv4 socket-address structures and TCP/IP constants.
#include <poll.h> // Include bounded descriptor-readiness checks for sockets and udev.
#include <pthread.h> // Include POSIX thread creation, mutexes and condition-variable APIs.
#include <sched.h> // Include CPU affinity masks and Linux scheduling policies/priorities.
#include <sys/resource.h> // Include Linux nice-value control, distinct from real-time scheduler priority.
#include <sys/socket.h> // Include TCP socket creation, connection, sending, receiving and options.
#include <sys/time.h> // Include timeval values for socket call timeouts.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono_app { // Keep these declarations in the project namespace and avoid global name collisions.

// ==================== OBJECT SETUP ====================

NetworkManager::NetworkManager(const Options& options, CommandQueue& messages, // Pass this next argument/value to the call or initializer above.
                               DataQueue& telemetry) // Declare telemetry for this operation.
    : options_(options), // Supply the next constructor initializer or argument.
      messages_(messages), // Call messages_ with the arguments shown here.
      telemetry_(telemetry) {} // Share the existing DataQueue and finish construction.

NetworkManager::~NetworkManager() { stop(); } // Stop the worker and release owned resources during destruction.

// ==================== MAIN METHODS - REPORT ORDER ====================

bool NetworkManager::startListening() { // Create, bind and listen on the Raspberry Pi TCP server socket.
    /* API: ::socket
     * Documentation: https://man7.org/linux/man-pages/man2/socket.2.html
     * Create a TCP socket. AF_INET means IPv4, SOCK_STREAM means a reliable byte stream. Returns a descriptor or -1 with errno.
     */
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0); // Set server fd to ::socket(AF_INET, SOCK_STREAM, 0).
    if (server_fd_ < 0) { // Test the condition server_fd_ < 0.
        cerr << "socket: " << strerror(errno) << '\n'; // Print the diagnostic or test result for the operator.
        post_network_event( // Call post_network_event with the arguments shown here.
            WorkerResultCode::NETWORK_FAILED, // Pass this next argument/value to the call or initializer above.
            static_cast<uint8_t>(phono::protocol::Error::NetworkError)); // Complete the call above with these remaining arguments.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    int reuse = 1; // Initialize reuse to 1.
    /* API: setsockopt
     * Documentation: https://man7.org/linux/man-pages/man2/setsockopt.2.html
     * Set the named socket option using a pointer and its exact byte size. Check the return; options here configure reuse and per-call send/receive timeouts.
     */
    (void)::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, // Pass this next argument/value to the call or initializer above.
                       sizeof(reuse)); // Call sizeof with the arguments shown here.
    sockaddr_in address{}; // Declare address for this operation.
    address.sin_family = AF_INET; // Set address.sin family to AF_INET.
    address.sin_port = htons(options_.port); // Set address.sin port to htons(options_.port).
    /* API: inet_pton
     * Documentation: https://man7.org/linux/man-pages/man3/inet_pton.3.html
     * Convert the textual bind address into binary IPv4 form. AF_INET success returns 1; invalid text returns 0 and a call error returns -1.
     */
    if (::inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) != // Test the condition ::inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) !=.
        1) { // Finish the parameters/condition and enter the block below.
        cerr << "Invalid IPv4 bind address: " << options_.bind_address << '\n'; // Print the diagnostic or test result for the operator.
        /* API: ::close
         * Documentation: https://man7.org/linux/man-pages/man2/close.2.html
         * Release a descriptor after its owner has finished using it. Set the stored descriptor to -1 so it cannot be reused accidentally.
         */
        ::close(server_fd_); // Release this POSIX descriptor; the owner resets its stored descriptor to -1.
        server_fd_ = -1; // Set server fd to -1.
        post_network_event( // Call post_network_event with the arguments shown here.
            WorkerResultCode::NETWORK_FAILED, // Pass this next argument/value to the call or initializer above.
            static_cast<uint8_t>(phono::protocol::Error::NetworkError)); // Complete the call above with these remaining arguments.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    /* API: ::bind
     * Documentation: https://man7.org/linux/man-pages/man2/bind.2.html
     * Assign a local IP address and port to the listening socket. The IP must exist on the Pi. Returns 0 or -1 with errno.
     */
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&address), // Test the condition ::bind(server_fd_, reinterpret_cast<sockaddr*>(&address),.
               sizeof(address)) < 0 || // Call sizeof with the arguments shown here.
        /* API: ::listen
         * Documentation: https://man7.org/linux/man-pages/man2/listen.2.html
         * Mark the socket as a listener. The backlog limits waiting connections. Accept creates a separate client descriptor.
         */
        ::listen(server_fd_, 1) < 0) { // Supply the next constructor initializer or argument.
        cerr << "Cannot listen on " << options_.bind_address << ':' // Print the diagnostic or test result for the operator.
                  << options_.port << ": " << strerror(errno) << '\n'; // Append this part of the diagnostic output.
        ::close(server_fd_); // Release this POSIX descriptor; the owner resets its stored descriptor to -1.
        server_fd_ = -1; // Set server fd to -1.
        post_network_event( // Call post_network_event with the arguments shown here.
            WorkerResultCode::NETWORK_FAILED, // Pass this next argument/value to the call or initializer above.
            static_cast<uint8_t>(phono::protocol::Error::NetworkError)); // Complete the call above with these remaining arguments.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

// TCP has no message boundaries. Keep partial command bytes until all five arrive.
bool NetworkManager::receiveCommand(phono::protocol::CommandPacket& command) { // Retain partial TCP input until all five command bytes can be decoded.
    /* API: ::recv
     * Documentation: https://man7.org/linux/man-pages/man2/recv.2.html
     * Receive TCP bytes into the remaining command-buffer space. Positive is the number received, zero means peer closure, -1 requires checking errno. TCP may split a five-byte command.
     */
    const auto count = ::recv(client_fd_, command_bytes_.data() + bytes_received_, // Initialize count to ::recv(client_fd_, command_bytes_.data() + bytes_received_.
                              command_bytes_.size() - bytes_received_, 0); // Call command_bytes_.size with the arguments shown here.
    if (count < 0) { // Test the condition count < 0.
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) client_alive_ = false; // Test the condition errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK. If true, set client alive to false.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    if (count == 0) { client_alive_ = false; return false; } // Test the condition count == 0. If true, continue the expression or parameter list from the preceding line.
    bytes_received_ += static_cast<size_t>(count); // Update bytes received using += static_cast<size_t>(count).
    if (bytes_received_ != command_bytes_.size()) { // Test the condition bytes_received_ != command_bytes_.size().
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    bytes_received_ = 0; // Set bytes received to 0.
    if (!phono::protocol::decode_command(command_bytes_.data(), command_bytes_.size(), command)) { // Test the condition !phono::protocol::decode_command(command_bytes_.data(), command_bytes_.size(), command).
        client_alive_ = false; // Set client alive to false.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

bool NetworkManager::sendData(const DataPacket& packet) { // Serialize a waveform/FFT snapshot and send its complete frame.
    return send_all(client_fd_, phono::protocol::make_data_packet(packet.waveform, packet.fft_magnitudes)); // Return send_all(client_fd_, phono::protocol::make_data_packet(packet.waveform, packet.fft_magnitudes)) to the caller.
} // End this function or local scope.

// Main may call this method: queue the frame rather than writing from Main.
bool NetworkManager::sendStatus(const phono::protocol::StatusPacket& status) { // Serialize capability flags and enqueue a control reply for Network.
    queue_control(phono::protocol::make_status_packet(status)); // Put a complete serialized reply into the Main-to-Network ControlReplyQueue.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

bool NetworkManager::sendFileList(uint32_t cursor, // Pass this next argument/value to the call or initializer above.
                                 const vector<phono::protocol::FileEntry>& entries) { // Declare entries for this operation.
    queue_control(phono::protocol::make_file_list_packet(cursor, entries)); // Put a complete serialized reply into the Main-to-Network ControlReplyQueue.
    return true; // Report success, readiness or an available result to the caller.
} // End this function or local scope.

// CONTROL: Main -> ControlReplyQueue -> Network TCP send. This preserves reply order.
void NetworkManager::queue_control(vector<uint8_t> frame) { // Put a complete serialized reply into the Main-to-Network ControlReplyQueue.
    control_replies_.enqueue(move(frame)); // Send this record through the named protected queue; the receiver handles it later.
} // End this function or local scope.

void NetworkManager::start() { // Create this worker pthread once and retain its handle for joining.
    if (worker_started_) { // Test the condition worker_started_.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    stop_requested_.store(false); // Update the shared stop requested value atomically.
    /* API: pthread_create
     * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
     * Start a POSIX worker. Pass its pthread_t address, attributes (NULL for defaults), entry callback and context pointer. Returns 0 or an error number.
     */
    const int result = pthread_create(&worker_, nullptr, &NetworkManager::threadEntry, this); // Initialize result to pthread_create(&worker_, nullptr, &NetworkManager::threadEntry, this).
    if (result != 0) { // Test the condition result != 0.
        cerr << "Cannot start Network thread: " << strerror(result) << '\n'; // Print the diagnostic or test result for the operator.
        post_network_event(WorkerResultCode::NETWORK_FAILED, // Call post_network_event with the arguments shown here.
            static_cast<uint8_t>(phono::protocol::Error::NetworkError)); // Complete the call above with these remaining arguments.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    worker_started_ = true; // Set worker started to true.
} // End this function or local scope.

void NetworkManager::stop() { // Request teardown and join workers before destroying their shared resources.
    stop_requested_.store(true); // Update the shared stop requested value atomically.
    if (worker_started_) { // Test the condition worker_started_.
        /* API: pthread_join
         * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
         * Wait for this worker to return before releasing the objects it uses. Pass the pthread handle and NULL when its return value is unused.
         */
        pthread_join(worker_, nullptr); // Call pthread_join with the arguments shown here.
        worker_started_ = false; // Set worker started to false.
    } // End this block, declaration or argument list.
} // End this function or local scope.

bool NetworkManager::connected() const { return connected_.load(); } // Read whether the Network worker currently has a GUI client.

// ==================== THREAD FLOW ====================

// This pthread is the sole owner of server_fd_ and client_fd_.
// FLOWCHART: th_Network part 1 -> listen/accept -> serve_client (parts 2-3).
void NetworkManager::run() { // Execute this class's thread loop in the order shown in its flowchart.
    set_network_thread_policy(); // Request CPU 3 affinity and SCHED_OTHER priority 0 with nice 0.
    if (!startListening()) { // Test the condition !startListening().
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.
    post_network_event(WorkerResultCode::NETWORK_STARTED); // Send a connection or network error event to Main through CommandQueue.
    cout << "PhonoStethos server listening on " << options_.bind_address << ':' // Print the diagnostic or test result for the operator.
              << options_.port << '\n'; // Append this part of the diagnostic output.

    while (!stop_requested_.load() && running.load()) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        pollfd listener{server_fd_, POLLIN, 0}; // Declare listener for this operation.
        const auto ready = ::poll(&listener, 1, 10); // Initialize ready to ::poll(&listener, 1, 10).
        if (ready < 0) { // Test the condition ready < 0.
            if (errno == EINTR) continue; // Test the condition errno == EINTR. If true, skip the rest of this pass and check the next loop iteration.
            cerr << "Listener poll: " << strerror(errno) << '\n'; // Print the diagnostic or test result for the operator.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        if (ready == 0 || !(listener.revents & POLLIN)) continue; // Test the condition ready == 0 || !(listener.revents & POLLIN). If true, skip the rest of this pass and check the next loop iteration.
        sockaddr_in peer{}; // Declare peer for this operation.
        socklen_t peer_size = sizeof(peer); // Initialize peer size to sizeof(peer).
        client_fd_ = // Continue the expression or parameter list from the preceding line.
            /* API: ::accept
             * Documentation: https://man7.org/linux/man-pages/man2/accept.2.html
             * Accept a waiting GUI connection. Returns a new descriptor. Keep the listening descriptor for later reconnects.
             */
            ::accept(server_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_size); // Supply the next constructor initializer or argument.
        if (client_fd_ < 0) { // Test the condition client_fd_ < 0.
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) { // Test the condition errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK.
                continue; // Skip the rest of this pass and check the next loop iteration.
            } // End this block, declaration or argument list.
            cerr << "accept: " << strerror(errno) << '\n'; // Print the diagnostic or test result for the operator.
            continue; // Skip the rest of this pass and check the next loop iteration.
        } // End this block, declaration or argument list.
        control_replies_.clear(); // Discard the previous contents/state of this object.
        telemetry_.clear(); // Discard the previous contents/state of this object.
        connected_.store(true); // Update the shared connected value atomically.
        telemetry_.set_connected(true); // Call telemetry_.set_connected with the arguments shown here.
        post_network_event(WorkerResultCode::CLIENT_CONNECTED); // Send a connection or network error event to Main through CommandQueue.
        serve_client(client_fd_); // Alternate one outgoing frame with command reception for one connected GUI.
        ::close(client_fd_); // Release this POSIX descriptor; the owner resets its stored descriptor to -1.
        client_fd_ = -1; // Set client fd to -1.
        connected_.store(false); // Update the shared connected value atomically.
        telemetry_.set_connected(false); // Call telemetry_.set_connected with the arguments shown here.
        control_replies_.clear(); // Discard the previous contents/state of this object.
        post_network_event(WorkerResultCode::CLIENT_DISCONNECTED); // Send a connection or network error event to Main through CommandQueue.
    } // End this block, declaration or argument list.
    ::close(server_fd_); // Release this POSIX descriptor; the owner resets its stored descriptor to -1.
    server_fd_ = -1; // Set server fd to -1.
} // End this function or local scope.

// ==================== CONTROL REPLY QUEUE ====================

void ControlReplyQueue::enqueue(vector<uint8_t> frame) { // Store an item using this channel's locking and delivery policy.
    PthreadLock lock(mutex_); // Lock the reply FIFO against simultaneous Network reads.
    // QUEUE SEND (Main): add this serialized reply at the BACK of the FIFO.
    queue_.push_back(move(frame)); // Transfer this frame to the newest end; unlock on return.
} // End this block, declaration or argument list.

bool ControlReplyQueue::dequeue(vector<uint8_t>& frame) { // Take an available item using this channel's receive policy.
    PthreadLock lock(mutex_); // Protect the empty check, front item and removal together.
    if (queue_.empty()) { // Test the condition queue_.empty().
        return false; // No waiting reply; Network may check DataQueue instead.
    } // End this block, declaration or argument list.
    // QUEUE TAKE (Network): take the oldest reply from the FRONT.
    // dequeue returns false when empty; its mutex uses an ordinary short lock.
    frame = move(queue_.front()); // Network now owns the oldest frame's byte vector.
    queue_.pop_front(); // Remove that one queue entry so it cannot be sent twice.
    return true; // Release the mutex before Network performs the TCP send.
} // End this block, declaration or argument list.

void ControlReplyQueue::clear() { // Discard waiting channel data and reset its state under its owner's rules.
    PthreadLock lock(mutex_); // Clear atomically relative to senders/receivers.
    queue_.clear(); // Drop waiting replies during a GUI connection change.
} // End this block, declaration or argument list.

// ==================== PRIVATE HELPERS ====================

// pthread_create expects a C-style callback; recover this NetworkManager from void*.
void* NetworkManager::threadEntry(void* context) { // Recover the object pointer passed to pthread_create and call its worker loop.
    static_cast<NetworkManager*>(context)->run(); // Complete the call above with these remaining arguments.
    return nullptr; // Return nullptr to the caller.
} // End this function or local scope.

void NetworkManager::set_network_thread_policy() { // Request CPU 3 affinity and SCHED_OTHER priority 0 with nice 0.
    // Main and Network are peers: SCHED_OTHER priority 0, nice 0.
    sched_param normal_priority{}; // Declare normal priority for this operation.
    /* API: pthread_setschedparam
     * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
     * Request the scheduling policy and sched_priority stored in sched_param. FIFO 80 is AudioDSP and FIFO 50 is FileIO. SCHED_OTHER requires priority 0.
     */
    /* API: pthread_self
     * Documentation: https://man7.org/linux/man-pages/man7/pthreads.7.html
     * Return the calling pthread handle, used when setting this worker's affinity or scheduling policy.
     */
    const int policy_error = pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority); // Initialize policy error to pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority).
    if (policy_error != 0) cerr << "Cannot set normal scheduling: " << strerror(policy_error) << '\n'; // Test the condition policy_error != 0. If true, print the diagnostic or test result for the operator.
    /* API: setpriority
     * Documentation: https://man7.org/linux/man-pages/man2/setpriority.2.html
     * Set the calling Linux thread's nice value with PRIO_PROCESS and ID 0. Nice 0 is normal SCHED_OTHER weight; it is distinct from FIFO priority 50/80. Returns 0 or -1 with errno.
     */
    if (setpriority(PRIO_PROCESS, 0, 0) != 0) cerr << "Cannot set nice 0: " << strerror(errno) << '\n'; // Test the condition setpriority(PRIO_PROCESS, 0, 0) != 0. If true, print the diagnostic or test result for the operator.

    cpu_set_t cpus; // Declare cpus for this operation.
    CPU_ZERO(&cpus); // Call CPU_ZERO with the arguments shown here.
    CPU_SET(3, &cpus); // Call CPU_SET with the arguments shown here.
    const int result = // Start initializing this local value from the expression on the next line.
        /* API: pthread_setaffinity_np
         * Documentation: https://man7.org/linux/man-pages/man3/pthread_setaffinity_np.3.html
         * Ask Linux to run this pthread on the CPUs in cpu_set_t. pthread_self identifies the current worker. Check the returned error number.
         */
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus); // Call pthread_setaffinity_np with the arguments shown here.
    if (result != 0) { // Test the condition result != 0.
        cerr << "Network thread CPU affinity not set: " << strerror(result) // Print the diagnostic or test result for the operator.
                  << '\n'; // Append this part of the diagnostic output.
    } // End this block, declaration or argument list.

} // End this function or local scope.

void NetworkManager::post_network_event(WorkerResultCode code, uint8_t error) { // Send a connection or network error event to Main through CommandQueue.
    MainMessage message; // Declare message for this operation.
    message.kind = MessageKind::WORKER_RESULT; // Set message.kind to MessageKind::WORKER_RESULT.
    if (error != 0) { // Test the condition error != 0.
        message.kind = MessageKind::WORKER_ERROR; // Set message.kind to MessageKind::WORKER_ERROR.
    } // End this block, declaration or argument list.
    message.result_code = code; // Set message.result code to code.
    message.error_code = error; // Set message.error code to error.
    // QUEUE SEND: connection/startup events also go to Main's CommandQueue.
    messages_.enqueue(move(message)); // Send this record through the named protected queue; the receiver handles it later.
} // End this function or local scope.

// FLOWCHART: select ControlReplyQueue first, otherwise the latest DataQueue frame.
// Complete this frame before receiving the next command or selecting another frame.
bool NetworkManager::send_next_frame(int socket_fd) { // Take one control reply first, otherwise take the latest visualization snapshot.
    vector<uint8_t> control; // Declare control for this operation.
    // QUEUE RECEIVE: take a reliable control reply before ordinary telemetry.
    if (control_replies_.dequeue(control)) { // Test the condition control_replies_.dequeue(control).
        return send_all(socket_fd, control); // Return send_all(socket_fd, control) to the caller.
    } else { // Follow the alternative path when the previous condition is false.
        DataPacket packet; // Declare packet for this operation.
        // telemetry_ aliases SystemController::data_queue_: one latest snapshot.
        if (telemetry_.dequeue(packet)) { // Test the condition telemetry_.dequeue(packet).
            try { // Catch failures from the operations in this block.
                return sendData(packet); // Return sendData(packet) to the caller.
            } catch (const exception& error) { // Handle the stated exception instead of continuing with an invalid result.
                cerr << "Cannot serialize telemetry: " << error.what() << '\n'; // Print the diagnostic or test result for the operator.
                post_network_event( // Call post_network_event with the arguments shown here.
                    WorkerResultCode::NETWORK_FAILED, // Pass this next argument/value to the call or initializer above.
                    static_cast<uint8_t>(phono::protocol::Error::ProtocolError)); // Complete the call above with these remaining arguments.
                return false; // Report failure or no available item to the caller.
            } // End this block, declaration or argument list.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    return true; // Nothing waiting to send is also a normal iteration.
} // End this function or local scope.

// A TCP write can be partial: finish this frame before another frame is sent.
bool NetworkManager::send_all(int socket_fd, // Pass this next argument/value to the call or initializer above.
                              const vector<uint8_t>& frame) { // Declare frame for this operation.
    size_t bytes_sent = 0; // Retained across retries: never resend accepted bytes.
    const auto deadline = // Start initializing this local value from the expression on the next line.
        chrono::steady_clock::now() + chrono::milliseconds(100); // Complete the call above with these remaining arguments.
    while (bytes_sent < frame.size() && !stop_requested_.load()) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        if (chrono::steady_clock::now() >= deadline) { // Test the condition chrono::steady_clock::now() >= deadline.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        /* API: ::send
         * Documentation: https://man7.org/linux/man-pages/man2/send.2.html
         * Submit remaining frame bytes to TCP. A short positive result advances the offset. MSG_NOSIGNAL prevents a closed peer from raising SIGPIPE.
         */
        const auto count = ::send(socket_fd, frame.data() + bytes_sent, // Initialize count to ::send(socket_fd, frame.data() + bytes_sent.
                                  frame.size() - bytes_sent, MSG_NOSIGNAL); // Call frame.size with the arguments shown here.
        if (count < 0) { // Test the condition count < 0.
            if (errno == EINTR) continue; // Test the condition errno == EINTR. If true, skip the rest of this pass and check the next loop iteration.
            if (errno == EAGAIN || errno == EWOULDBLOCK) { // Test the condition errno == EAGAIN || errno == EWOULDBLOCK.
                const auto now = chrono::steady_clock::now(); // Initialize now to chrono::steady_clock::now().
                if (now >= deadline) { // Test the condition now >= deadline.
                    return false; // Report failure or no available item to the caller.
                } // End this block, declaration or argument list.
                const auto remaining = // Start initializing this local value from the expression on the next line.
                    chrono::duration_cast<chrono::milliseconds>( // Begin this function/widget call; its arguments follow below.
                        deadline - now) // Continue the expression or parameter list from the preceding line.
                        .count(); // Complete the call above with these remaining arguments.
                pollfd writable{socket_fd, POLLOUT, 0}; // Declare writable for this operation.
                /* API: ::poll
                 * Documentation: https://man7.org/linux/man-pages/man2/poll.2.html
                 * Wait for descriptor readiness for a bounded number of milliseconds. Positive means events, zero means timeout, -1 means error.
                 */
                const int ready = ::poll( // Initialize ready to ::poll(.
                    &writable, 1, // Pass this next argument/value to the call or initializer above.
                    static_cast<int>(min<int64_t>(remaining, 10))); // Complete the call above with these remaining arguments.
                if (ready < 0 && errno != EINTR) { // Test the condition ready < 0 && errno != EINTR.
                    return false; // Report failure or no available item to the caller.
                } // End this block, declaration or argument list.
                if (ready > 0 && // Test the condition ready > 0 &&.
                    (writable.revents & (POLLERR | POLLHUP | POLLNVAL))) { // Finish the parameters/condition and enter the block below.
                    return false; // Report failure or no available item to the caller.
                } // End this block, declaration or argument list.
                continue; // Skip the rest of this pass and check the next loop iteration.
            } // End this block, declaration or argument list.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        if (count == 0) { // Test the condition count == 0.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        bytes_sent += static_cast<size_t>(count); // Update bytes sent using += static_cast<size_t>(count).
    } // End this block, declaration or argument list.
    return bytes_sent == frame.size(); // Return bytes_sent == frame.size() to the caller.
} // End this function or local scope.

// One GUI client: send one control/telemetry frame, then check for incoming commands.
void NetworkManager::serve_client(int socket_fd) { // Alternate one outgoing frame with command reception for one connected GUI.
    timeval timeout{}; // Declare timeout for this operation.
    timeout.tv_usec = 10000; // Set timeout.tv usec to 10000.
    if (::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, // Test the condition ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,.
                     sizeof(timeout)) != 0 || // Call sizeof with the arguments shown here.
        ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, // Supply the next constructor initializer or argument.
                     sizeof(timeout)) != 0) { // Call sizeof with the arguments shown here.
        cerr << "Cannot configure client socket timeouts: " // Print the diagnostic or test result for the operator.
                  << strerror(errno) << '\n'; // Append this part of the diagnostic output.
        return; // Leave this function; any local scoped locks are released.
    } // End this block, declaration or argument list.

    bytes_received_ = 0; // Set bytes received to 0.
    client_alive_ = true; // Set client alive to true.
    while (client_alive_ && !stop_requested_.load() && running.load()) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        if (!send_next_frame(socket_fd)) { // Test the condition !send_next_frame(socket_fd).
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.

        pollfd event{socket_fd, POLLIN, 0}; // Declare event for this operation.
        const auto ready = ::poll(&event, 1, 10); // Initialize ready to ::poll(&event, 1, 10).
        if (ready < 0) { // Test the condition ready < 0.
            if (errno == EINTR) continue; // Test the condition errno == EINTR. If true, skip the rest of this pass and check the next loop iteration.
            cerr << "Client poll: " << strerror(errno) << '\n'; // Print the diagnostic or test result for the operator.
            break; // Leave the current loop or switch branch.
        } // End this block, declaration or argument list.
        if (ready == 0) continue; // Test the condition ready == 0. If true, skip the rest of this pass and check the next loop iteration.
        if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) break; // Test the condition event.revents & (POLLERR | POLLHUP | POLLNVAL). If true, leave the current loop or switch branch.
        if (!(event.revents & POLLIN)) continue; // Test the condition !(event.revents & POLLIN). If true, skip the rest of this pass and check the next loop iteration.

        phono::protocol::CommandPacket command; // Declare command for this operation.
        // QUEUE SEND: received TCP bytes have become a complete CommandPacket.
        // enqueue wraps it as MessageKind::GUI_COMMAND and signals Main's queue.
        if (receiveCommand(command)) messages_.enqueue(command); // Test the condition receiveCommand(command). If true, send this record through the named protected queue; the receiver handles it later.
    } // End this block, declaration or argument list.
} // End this function or local scope.

}  // namespace phono_app
