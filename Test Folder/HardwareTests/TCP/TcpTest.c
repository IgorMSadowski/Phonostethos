/**
 * TcpTest.c - checks Tcp Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - set_timeouts(): set timeouts.
 * - send_all(): retry partial TCP sends using an offset and a cumulative deadline.
 * - receive_all(): receive all.
 * - serve(): serve.
 * - connect_client(): connect client.
 * - main(): run this standalone test and return success only if its checks pass.
 */

#include <stdbool.h> // Include C bool, true and false.
// Two-terminal TCP check. Uses port 9090 so it is separate from the real server.
#include <stdint.h> // Include fixed-width PCM and protocol integer types.
#include <arpa/inet.h> // Include IPv4 text/binary conversion and network byte order.
#include <sys/socket.h> // Include TCP socket creation, connection, sending, receiving and options.
#include <poll.h> // Include bounded descriptor-readiness checks for sockets and udev.
#include <unistd.h> // Include POSIX descriptor read/write/close and synchronization calls.
#include <errno.h> // Include errno and symbolic system-call error codes.
#include <stdio.h> // Include printf/fprintf/puts console diagnostics.
#include <stdlib.h> // Include process exit, allocation and numeric conversion helpers.
#include <string.h> // Include memory/string copying, comparisons and strerror diagnostics.

static bool set_timeouts(int fd) { // Finish the parameters/condition and enter the block below.
    struct timeval timeout = {3, 0}; // Group the related fields in one plain data record.
    /* API: setsockopt
     * Set the named socket option using a pointer and its exact byte size. Check the return; options here configure reuse and per-call send/receive timeouts.
     */
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 && // Return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 && to the caller.
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0; // Call setsockopt with the arguments shown here.
} // End this block, declaration or argument list.

static bool send_all(int fd, const uint8_t* bytes, size_t length) { // Retry partial TCP sends using an offset and a cumulative deadline.
    size_t offset = 0; // Initialize offset to 0.
    while (offset < length) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        // API: send may accept only part of the bytes. Keep the accepted offset.
        ssize_t count = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL); // Initialize count to send(fd, bytes + offset, length - offset, MSG_NOSIGNAL).
        if (count < 0 && errno == EINTR) continue; // Test the condition count < 0 && errno == EINTR. If true, skip the rest of this pass and check the next loop iteration.
        if (count <= 0) return false; // Test the condition count <= 0. If true, report failure or no available item to the caller.
        offset += (size_t)(count); // Update offset using += (size_t)(count).
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

static bool receive_all(int fd, uint8_t* bytes, size_t length) { // Finish the parameters/condition and enter the block below.
    size_t offset = 0; // Initialize offset to 0.
    while (offset < length) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        // API: recv returns bytes, not whole protocol packets. Zero means disconnection.
        ssize_t count = recv(fd, bytes + offset, length - offset, 0); // Initialize count to recv(fd, bytes + offset, length - offset, 0).
        if (count < 0 && errno == EINTR) continue; // Test the condition count < 0 && errno == EINTR. If true, skip the rest of this pass and check the next loop iteration.
        if (count <= 0) return false; // Test the condition count <= 0. If true, report failure or no available item to the caller.
        offset += (size_t)(count); // Update offset using += (size_t)(count).
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

static int serve(uint16_t port) { // Finish the parameters/condition and enter the block below.
    // API: socket -> bind -> listen prepares a TCP listening endpoint.
    int listener = socket(AF_INET, SOCK_STREAM, 0); // Initialize listener to socket(AF_INET, SOCK_STREAM, 0).
    if (listener < 0) { perror("socket"); return 1; } // Test the condition listener < 0. If true, continue the expression or parameter list from the preceding line.
    int reuse = 1; // Initialize reuse to 1.
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); // Call setsockopt with the arguments shown here.
    struct sockaddr_in address = {0}; // Group the related fields in one plain data record.
    address.sin_family = AF_INET; // Set address.sin family to AF_INET.
    address.sin_addr.s_addr = htonl(INADDR_ANY); // Set address.sin addr.s addr to htonl(INADDR_ANY).
    address.sin_port = htons(port); // Set address.sin port to htons(port).
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 || // Test the condition bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||.
        listen(listener, 1) != 0) { // Call listen with the arguments shown here.
        perror("FAIL: bind/listen"); // Call perror with the arguments shown here.
        close(listener); // Call close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    printf("Listening on TCP port %u; waiting up to 30 seconds for the test client\n", port); // Print the diagnostic or test result for the operator.
    fflush(stdout); // Call fflush with the arguments shown here.
    struct pollfd event = {listener, POLLIN, 0}; // Group the related fields in one plain data record.
    int ready; // Declare ready for this operation.
    do { ready = poll(&event, 1, 30000); } while (ready < 0 && errno == EINTR); // Complete the call above with these remaining arguments.
    if (ready <= 0 || !(event.revents & POLLIN)) { // Test the condition ready <= 0 || !(event.revents & POLLIN).
        fprintf(stderr, "FAIL: no client arrived within the test window\n"); // Print the diagnostic or test result for the operator.
        close(listener); // Call close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    // API: accept returns a NEW descriptor for the client, separate from listener.
    int client = accept(listener, NULL, NULL); // Initialize client to accept(listener, NULL, NULL).
    close(listener); // Call close with the arguments shown here.
    if (client < 0) { perror("accept"); return 1; } // Test the condition client < 0. If true, continue the expression or parameter list from the preceding line.
    bool success = set_timeouts(client); // Initialize success to set_timeouts(client).
    uint8_t bytes[5]; // Declare bytes for this operation.
    // The demo sends SET_GAIN (2), value 101 (0.5x), as five big-endian bytes.
    const uint8_t expected[5] = {2, 0, 0, 0, 101}; // Initialize expected to {2, 0, 0, 0, 101}.
    if (!success || !receive_all(client, bytes, sizeof(bytes)) || // Test the condition !success || !receive_all(client, bytes, sizeof(bytes)) ||.
        memcmp(bytes, expected, sizeof(bytes)) != 0) { // Call memcmp with the arguments shown here.
        fprintf(stderr, "FAIL: did not receive the expected five-byte command\n"); // Print the diagnostic or test result for the operator.
        close(client); // Call close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    printf("Received SET_GAIN code 101 (0.5x)\n"); // Print the diagnostic or test result for the operator.
    uint8_t reply[5] = {0x02, 1, 1, 1, 0}; // Test StatusPacket response.
    success = send_all(client, reply, sizeof(reply)); // Set success to send_all(client, reply, sizeof(reply)).
    if (close(client) != 0) success = false; // Test the condition close(client) != 0. If true, set success to false.
    if (!success) { fprintf(stderr, "FAIL: sending StatusPacket reply\n"); return 1; } // Test the condition !success. If true, print the diagnostic or test result for the operator.
    puts("PASS: TCP accept, command reception and reply transmission"); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

static int connect_client(const char* host, uint16_t port) { // Finish the parameters/condition and enter the block below.
    int fd = socket(AF_INET, SOCK_STREAM, 0); // Initialize fd to socket(AF_INET, SOCK_STREAM, 0).
    if (fd < 0) { perror("socket"); return 1; } // Test the condition fd < 0. If true, continue the expression or parameter list from the preceding line.
    struct sockaddr_in address = {0}; // Group the related fields in one plain data record.
    address.sin_family = AF_INET; // Set address.sin family to AF_INET.
    address.sin_port = htons(port); // Set address.sin port to htons(port).
    /* API: inet_pton
     * Convert the textual bind address into binary IPv4 form. AF_INET success returns 1; invalid text returns 0 and a call error returns -1.
     */
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 || !set_timeouts(fd)) { // Test the condition inet_pton(AF_INET, host, &address.sin_addr) != 1 || !set_timeouts(fd).
        fprintf(stderr, "FAIL: use an IPv4 address and check socket permissions\n"); // Print the diagnostic or test result for the operator.
        close(fd); // Call close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    // API: connect initiates TCP to the test listener (not the PhonoStethos GUI server).
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) { // Test the condition connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0.
        perror("FAIL: connect"); // Call perror with the arguments shown here.
        close(fd); // Call close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    const uint8_t bytes[5] = {2, 0, 0, 0, 101}; // Initialize bytes to {2, 0, 0, 0, 101}.
    // Deliberately submit the command in two parts. The receiver must assemble it.
    bool success = send_all(fd, bytes, 2); // Initialize success to send_all(fd, bytes, 2).
    usleep(20000); // Call usleep with the arguments shown here.
    success = success && send_all(fd, bytes + 2, 3); // Set success to success && send_all(fd, bytes + 2, 3).
    uint8_t reply[5]; // Declare reply for this operation.
    uint8_t expected[5] = {0x02, 1, 1, 1, 0}; // Initialize expected to {0x02, 1, 1, 1, 0}.
    if (!success || !receive_all(fd, reply, sizeof(reply)) || // Test the condition !success || !receive_all(fd, reply, sizeof(reply)) ||.
        memcmp(reply, expected, sizeof(reply)) != 0) { // Call memcmp with the arguments shown here.
        fprintf(stderr, "FAIL: fragmented command or StatusPacket reply differed\n"); // Print the diagnostic or test result for the operator.
        close(fd); // Call close with the arguments shown here.
        return 1; // Return 1 to the caller.
    } // End this block, declaration or argument list.
    if (close(fd) != 0) return 1; // Test the condition close(fd) != 0. If true, return 1 to the caller.
    puts("PASS: TCP connection, split five-byte command and exact StatusPacket reply"); // Call puts with the arguments shown here.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.

int main(int argc, char** argv) { // Run this standalone test and return success only if its checks pass.
    bool server = argc >= 2 && strcmp(argv[1], "server") == 0; // Initialize server to argc >= 2 && strcmp(argv[1], "server") == 0.
    bool client = argc >= 3 && strcmp(argv[1], "client") == 0; // Initialize client to argc >= 3 && strcmp(argv[1], "client") == 0.
    if ((!server && !client) || (server && argc > 3) || (client && argc > 4)) { // Test the condition (!server && !client) || (server && argc > 3) || (client && argc > 4).
        puts("Usage: TcpTest server [PORT]\n       TcpTest client PI_IP [PORT]\nDefault port: 9090"); // Call puts with the arguments shown here.
        return 2; // Return 2 to the caller.
    } // End this block, declaration or argument list.
    long port = 9090; // Initialize port to 9090.
    const char* port_text = NULL; // Initialize port text to NULL.
    if (server && argc == 3) port_text = argv[2]; // Test the condition server && argc == 3. If true, set port text to argv[2].
    if (client && argc == 4) port_text = argv[3]; // Test the condition client && argc == 4. If true, set port text to argv[3].
    if (port_text != NULL) { // Test the condition port_text != NULL.
        char* end = NULL; // Initialize end to NULL.
        port = strtol(port_text, &end, 10); // Set port to strtol(port_text, &end, 10).
        if (*port_text == '\0' || *end != '\0' || port < 1 || port > 65535) return 2; // Test the condition *port_text == '\0' || *end != '\0' || port < 1 || port > 65535. If true, return 2 to the caller.
    } // End this block, declaration or argument list.
    if (server) return serve((uint16_t)(port)); // Test the condition server. If true, return serve((uint16_t)(port)) to the caller.
    return connect_client(argv[2], (uint16_t)(port)); // Return connect_client(argv[2], (uint16_t)(port)) to the caller.
} // End this block, declaration or argument list.
