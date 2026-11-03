#pragma once

namespace phono_app {
struct SystemController;

class NetworkManager {
public:
    static int run(SystemController& controller);
    static void serve_client(int socket_fd, SystemController& controller);
};
}  // namespace phono_app
