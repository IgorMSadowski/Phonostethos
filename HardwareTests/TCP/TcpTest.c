#include <stdbool.h>
// Two-terminal TCP check. Uses port 9090 so it is separate from the real server.
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool set_timeouts(int fd) {
    struct timeval timeout = {3, 0};
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

static bool send_all(int fd, const uint8_t* bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        // API: send may accept only part of the bytes. Keep the accepted offset.
        ssize_t count = send(fd, bytes + offset, length - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += (size_t)(count);
    }
    return true;
}

static bool receive_all(int fd, uint8_t* bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        // API: recv returns bytes, not whole protocol packets. Zero means disconnection.
        ssize_t count = recv(fd, bytes + offset, length - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += (size_t)(count);
    }
    return true;
}

static int serve(uint16_t port) {
    // API: socket -> bind -> listen prepares a TCP listening endpoint.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        perror("FAIL: bind/listen");
        close(listener);
        return 1;
    }
    printf("Listening on TCP port %u; waiting up to 30 seconds for the test client\n", port);
    fflush(stdout);
    struct pollfd event = {listener, POLLIN, 0};
    int ready;
    do { ready = poll(&event, 1, 30000); } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || !(event.revents & POLLIN)) {
        fprintf(stderr, "FAIL: no client arrived within the test window\n");
        close(listener);
        return 1;
    }
    // API: accept returns a NEW descriptor for the client, separate from listener.
    int client = accept(listener, NULL, NULL);
    close(listener);
    if (client < 0) { perror("accept"); return 1; }
    bool success = set_timeouts(client);
    uint8_t bytes[5];
    // The demo sends SET_GAIN (2), value 101 (0.5x), as five big-endian bytes.
    const uint8_t expected[5] = {2, 0, 0, 0, 101};
    if (!success || !receive_all(client, bytes, sizeof(bytes)) ||
        memcmp(bytes, expected, sizeof(bytes)) != 0) {
        fprintf(stderr, "FAIL: did not receive the expected five-byte command\n");
        close(client);
        return 1;
    }
    printf("Received SET_GAIN code 101 (0.5x)\n");
    uint8_t reply[5] = {0x02, 1, 1, 1, 0}; // Test Status response.
    success = send_all(client, reply, sizeof(reply));
    if (close(client) != 0) success = false;
    if (!success) { fprintf(stderr, "FAIL: sending Status reply\n"); return 1; }
    puts("PASS: TCP accept, command reception and reply transmission");
    return 0;
}

static int connect_client(const char* host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1 || !set_timeouts(fd)) {
        fprintf(stderr, "FAIL: use an IPv4 address and check socket permissions\n");
        close(fd);
        return 1;
    }
    // API: connect initiates TCP to the test listener (not the PhonoStethos GUI server).
    if (connect(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        perror("FAIL: connect");
        close(fd);
        return 1;
    }
    const uint8_t bytes[5] = {2, 0, 0, 0, 101};
    // Deliberately submit the command in two parts. The receiver must assemble it.
    bool success = send_all(fd, bytes, 2);
    usleep(20000);
    success = success && send_all(fd, bytes + 2, 3);
    uint8_t reply[5];
    uint8_t expected[5] = {0x02, 1, 1, 1, 0};
    if (!success || !receive_all(fd, reply, sizeof(reply)) ||
        memcmp(reply, expected, sizeof(reply)) != 0) {
        fprintf(stderr, "FAIL: fragmented command or Status reply differed\n");
        close(fd);
        return 1;
    }
    if (close(fd) != 0) return 1;
    puts("PASS: TCP connection, split five-byte command and exact Status reply");
    return 0;
}

int main(int argc, char** argv) {
    bool server = argc >= 2 && strcmp(argv[1], "server") == 0;
    bool client = argc >= 3 && strcmp(argv[1], "client") == 0;
    if ((!server && !client) || (server && argc > 3) || (client && argc > 4)) {
        puts("Usage: TcpTest server [PORT]\n       TcpTest client PI_IP [PORT]\nDefault port: 9090");
        return 2;
    }
    long port = 9090;
    const char* port_text = NULL;
    if (server && argc == 3) port_text = argv[2];
    if (client && argc == 4) port_text = argv[3];
    if (port_text != NULL) {
        char* end = NULL;
        port = strtol(port_text, &end, 10);
        if (*port_text == '\0' || *end != '\0' || port < 1 || port > 65535) return 2;
    }
    if (server) return serve((uint16_t)(port));
    return connect_client(argv[2], (uint16_t)(port));
}
