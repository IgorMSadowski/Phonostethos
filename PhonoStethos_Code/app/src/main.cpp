#include "phonostethos/NetworkManager.h"
#include "phonostethos/SystemController.h"
#include "phonostethos/protocol.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace phono_app {
std::atomic<bool> running{true};

bool storage_ready(const Options& options) {
    std::error_code error;
    if (!std::filesystem::is_directory(options.storage, error) || error) {
        return false;
    }
    if (!options.require_mounted_storage) return true;
    std::ifstream mounts("/proc/self/mounts");
    if (!mounts) return false;
    std::string line, device, mountpoint;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        if (fields >> device >> mountpoint &&
            mountpoint == options.storage.string()) {
            return true;
        }
    }
    return false;
}
}  // namespace phono_app

namespace {
using namespace phono_app;

void handle_signal(int) {
    running.store(false);
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "Usage: phonostethos-server [--bind IPv4] [--port PORT] "
                         "[--capture-pcm NAME] [--playback-pcm NAME] "
                         "[--storage PATH] [--led-device PATH] "
                         "[--allow-unmounted-storage]\n";
            return false;
        }
        if (arg == "--allow-unmounted-storage") {
            options.require_mounted_storage = false;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << '\n';
            return false;
        }
        const std::string value = argv[++i];
        if (arg == "--bind") {
            options.bind_address = value;
        } else if (arg == "--capture-pcm") {
            options.capture_pcm = value;
        } else if (arg == "--playback-pcm") {
            options.playback_pcm = value;
        } else if (arg == "--storage") {
            options.storage = value;
        } else if (arg == "--led-device") {
            options.led_device = value;
        } else if (arg == "--port") {
            try {
                const auto port = std::stoul(value);
                if (port == 0 || port > 65535) return false;
                options.port = static_cast<std::uint16_t>(port);
            } catch (const std::exception&) {
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << arg << '\n';
            return false;
        }
    }
    return true;
}


}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 2;
    }
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    SystemController runtime(options);
    if (!runtime.led_driver.open()) {
        std::cerr << "LED status outputs are unavailable; continuing without LEDs\n";
    }
    runtime.update_leds();
    runtime.audio = std::make_unique<AudioEngine>(
        options, runtime.mode, runtime.gain, runtime.recording,
        runtime.error_code, runtime.telemetry, runtime.configuration_mutex,
        runtime.telemetry_mutex,
        runtime.dropped_telemetry, runtime.writer);

    const int server_result = NetworkManager::run(runtime);
    running.store(false);
    runtime.transport.store(phono::protocol::Transport::Stopping);
    runtime.recording.store(false);
    runtime.audio->stop();
    if (!runtime.writer.finish_and_wait()) {
        std::cerr << "Recording did not finalize cleanly during shutdown\n";
    }
    runtime.writer.stop();
    runtime.active_session.store(0);
    runtime.transport.store(phono::protocol::Transport::Live);
    runtime.led_driver.close();
    return server_result;
}
