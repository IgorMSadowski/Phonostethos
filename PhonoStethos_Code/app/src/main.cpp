#include "phonostethos/SystemController.h"

#include <csignal>
#include <cctype>
#include <iostream>

using namespace std;

namespace phono_app {
atomic<bool> running{true};
}

namespace {
void handle_signal(int) { phono_app::running.store(false); }

bool parse_options(int argc, char** argv, phono_app::Options& options,
                   bool& show_help) {
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];
        if (arg == "--help") {
            show_help = true;
            cout << "Usage: phonostethos-server [--bind IPv4] [--port PORT] "
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
            cerr << "Missing value for " << arg << '\n';
            return false;
        }
        const string value = argv[++i];
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
            // Read decimal digits explicitly. Check the range after each digit
            // so even a very long argument cannot overflow the accumulator.
            unsigned port = 0;
            if (value.empty()) {
                cerr << "Missing port number\n";
                return false;
            }
            for (char digit : value) {
                if (digit < '0' || digit > '9') {
                    cerr << "Invalid port: " << value << '\n';
                    return false;
                }
                port = port * 10 + (digit - '0');
                if (port > 65535) {
                    cerr << "Invalid port: " << value << '\n';
                    return false;
                }
            }
            if (port == 0) {
                cerr << "Port must be between 1 and 65535\n";
                return false;
            }
            options.port = static_cast<uint16_t>(port);
        } else {
            cerr << "Unknown option: " << arg << '\n';
            return false;
        }
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    phono_app::Options options;
    bool show_help = false;
    if (!parse_options(argc, argv, options, show_help)) {
        if (show_help) {
            return 0;
        }
        return 2;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    phono_app::SystemController runtime(options);
    if (!runtime.initSystem()) {
        return 1;
    }
    runtime.run();
    return 0;
}
